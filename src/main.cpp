#include "ccu/fota/config/fota_config.hpp"
#include "ccu/fota/store/fota_state_store.hpp"
#include "ccu/fota/store/fota_state_recovery.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"
#include "ccu/fota/store/fota_state_migration.hpp"
#include "ccu/fota/someip/diag_inventory_client.hpp"
#include "ccu/fota/someip/tbox_generic_transport_contract.hpp"
#include "ccu/fota/ota/ports/production_ports.hpp"
#include "ccu/fota/daemon/fota_daemon.hpp"
#include "config.h"          // ccu::fw::config::Config / LoadOptions / ConfigException
#include "constants.h"
#include "snapshot_assembler.h"
#include "fota_log_adapter.h"
#include "fota_log_context.h"
#include "ccu/fw/someip/runtime.hpp"
#include "ccu/fw/someip/types.hpp"
#include <iostream>
#include <filesystem>
#include <signal.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

using namespace ccu_fota;
namespace someip_fw = ccu::fw::someip;

volatile bool running = true;

void signalHandler(int signum) {
    // 信号处理仍使用 stderr，因为 Logger 可能正在关闭
    std::cerr << "Interrupt signal (" << signum << ") received.\n";
    running = false;
}

int main(int argc, char* argv[]) {
    // Register signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    FotaLogAdapter::orchestrator().info(fota_events::SERVICE_STARTING,
        "CCU-FOTA Service Starting"
    );

    // ---- 1. 建立稳定 WorkingDirectory 与 configRoots (CCU-FOTA-DSN-CR-004) ----
    namespace fs = std::filesystem;
    ccu::fw::config::LoadOptions options;
    options.cwd = fs::current_path();
    // 量产默认 /etc/ccu；argv[1] 可指定开发/测试 config 根（需含 common.yaml）。
    if (argc > 1) {
        options.configRoots = {fs::path(argv[1])};
    } else {
        options.configRoots = {"/etc/ccu"};
    }

    // ---- 2/3/4. 配置阶段：load + FotaConfig::from + logConfigFrom (统一 fail-closed) ----
    std::shared_ptr<const ccu::fw::config::ConfigSnapshot> snapshot;
    FotaConfig fotaConfig;
    ccu::fw::log::LogConfig log_config;
    try {
        snapshot = ccu::fw::config::Config::load("fota", options);
        fotaConfig = FotaConfig::from(*snapshot);
        log_config = FotaConfig::logConfigFrom(*snapshot);
    } catch (const ccu::fw::config::ConfigException& e) {
        std::cerr << "FATAL: config error [" << e.code << "] " << e.what()
                  << " (path=" << e.path << ", key=" << e.key << ")" << std::endl;
        return 1;
    } catch (const FotaConfigException& e) {
        std::cerr << "FATAL: fota config validation failed: " << e.what() << std::endl;
        return 1;
    }

    // ---- 初始化 Logger（配置阶段之后、业务模块与 SOME/IP 开放之前）----
    auto log_result = FotaLogAdapter::init("ccu-fota", log_config);
    if (log_result.error != ccu::fw::log::LogError::kOk) {
        std::cerr << "FATAL: Logger init failed: " << log_result.error_message << std::endl;
        return 1;
    }

    FotaLogAdapter::orchestrator().info(fota_events::SERVICE_CONFIG_LOADED,
        "Configuration loaded successfully",
        {flog::f_str("service", "fota")}
    );

    FotaLogAdapter::orchestrator().info(fota_events::SERVICE_LOG_INITIALIZED,
        "Logger initialized successfully",
        {flog::f_str("service", "ccu-fota"),
         flog::f_str("log_level", ccu::fw::log::logLevelToString(log_config.level)),
         flog::f_bool("strict_mode", log_config.strict)}
    );

    // ---- 5. 打开 Store、格式迁移与状态恢复 (CCU-FOTA-DSN-CR-005) ----
    std::shared_ptr<ccu_fota::store::FotaStateStore> state_store;
    ccu_fota::store::RecoveryPlan recovery_plan;
    try {
        auto store_obj = ccu_fota::store::FotaStateStore::open(
            fotaConfig.storeRoot, fotaConfig.dedupeMaxEntries, fotaConfig.dedupeTtlMs);
        state_store = std::make_shared<ccu_fota::store::FotaStateStore>(std::move(store_obj));
        recovery_plan = ccu_fota::store::StateRecovery::recover(*state_store);
    } catch (const std::exception& e) {
        std::cerr << "FATAL: store open/recovery failed: " << e.what() << std::endl;
        return 1;
    }

    if (recovery_plan.action == ccu_fota::store::RecoveryAction::Blocked) {
        FotaLogAdapter::orchestrator().error(
            ccu_fota::fota_events::STORE_RECOVERY_BLOCKED,
            "State recovery blocked, auto/active reporting disabled",
            {flog::f_str("reason", recovery_plan.reason)});
    }

    // ---- 契约 B durable 状态恢复：旧 ota.* -> fota.* 迁移 + FotaCloudStateStore ----
    // (CCU-FOTA-DSN-CR-011 §Store)：迁移失败 fail-closed，不清零业务状态。
    std::shared_ptr<ccu_fota::store::fota::FotaCloudStateStore> cloud_state_store;
    try {
        auto raw_store = state_store->underlyingStore();
        ccu_fota::store::migrateOtaToFota(raw_store);
        cloud_state_store = std::make_shared<ccu_fota::store::fota::FotaCloudStateStore>(
            std::move(raw_store));
    } catch (const std::exception& e) {
        std::cerr << "FATAL: FOTA durable state recovery failed: " << e.what()
                  << std::endl;
        return 1;
    }

    // ============================================================
    // CCU-FOTA-DSN-CR-007: 单 ccu-framework-someip Runtime 接线
    // 启动顺序：Config -> Logger -> Store -> 构造 SomeIpConfig/Runtime ->
    //   Provider/Client -> 注册 handler/availability -> runtime.start() ->
    //   request service -> 等待可用 -> offer -> ready/恢复/首次采集
    // ============================================================

    // ---- 2. 构造 SomeIpConfig，创建进程唯一 Runtime、Provider 和两个 Client ----
    someip_fw::SomeIpConfig someIpCfg = FotaConfig::someIpConfigFrom(*snapshot);

    someip_fw::SomeIpRuntime runtime;
    try {
        runtime = someip_fw::SomeIpRuntime::create(someIpCfg);
    } catch (const someip_fw::SomeIpException& e) {
        // Runtime／配置／Registry 错误阻止服务开放 (0301/0302)
        std::cerr << "FATAL: SomeIpRuntime create failed [" << e.code << "] "
                  << e.what() << std::endl;
        return 1;
    }

    // ServiceKey 与 InterfaceVersion（寻址 SSOT = VEH-SOMEIP Registry）
    // 契约 B：CCU-FOTA 作为 client 访问 TBOX 通用消息服务（0x6101），下行订阅其
    // Event；不再提供旧契约 A 的 0x1120 Provider / 0x6101 专用清单提交（已随 CR-011
    // 迁移到通用传输，旧 Method 0x0001 在 TBOX 侧永久保留删除）。
    someip_fw::ServiceKey diagKey{DEFAULT_DIAG_SERVICE_ID, DEFAULT_DIAG_INSTANCE_ID};
    someip_fw::InterfaceVersion ifaceV{1, 0};

    someip_fw::Client fwDiagClient = runtime.createClient(diagKey, ifaceV);

    // ---- 3. 构造业务适配器（包装 framework Client）----
    auto diag_client = std::make_shared<someip::DiagInventoryClient>(
        std::move(fwDiagClient), fotaConfig.diagCollectTimeout);

    auto assembler = std::make_shared<SnapshotAssembler>(diag_client);
    assembler->setThrottleInterval(static_cast<uint32_t>(fotaConfig.minReportInterval.count()));
    assembler->setMaxEcuCount(DEFAULT_MAX_ECU_COUNT);
    assembler->setStateStore(state_store);  // CCU-FOTA-DSN-CR-005: durable 序号分配
    assembler->setDiagRetryPolicy(fotaConfig.diagRetry.maxAttempts,
                                  static_cast<uint32_t>(fotaConfig.diagRetry.backoff.count()));

    // ---- 3. Client 注册 availability 回调 ----

    // DIAG/TBOX availability 回调（仅记录状态，不直接执行采集/提交）
    diag_client->onAvailability([](someip_fw::Availability a) {
        const char* label = "Unknown";
        switch (a) {
            case someip_fw::Availability::Available:       label = "Available"; break;
            case someip_fw::Availability::Unavailable:     label = "Unavailable"; break;
            case someip_fw::Availability::VersionMismatch: label = "VersionMismatch"; break;
            case someip_fw::Availability::Stopping:        label = "Stopping"; break;
            default: break;
        }
        FotaLogAdapter::diag_client().info(
            "fota.diag.availability",
            "DIAG service availability changed",
            {flog::f_str("availability", label)});
    });

    // ---- 4. runtime.start()，request DIAG service ----
    try {
        runtime.start();
    } catch (const someip_fw::SomeIpException& e) {
        std::cerr << "FATAL: SomeIpRuntime start failed [" << e.code << "] "
                  << e.what() << std::endl;
        return 1;
    }
    diag_client->requestService();

    // ---- 5. 等待 DIAG 可用或进入有界降级 ----
    // 有界等待 DIAG 可用（最多 5s），未就绪则降级运行（契约 B 采集按 availability 决策）。
    {
        constexpr int kWaitMs = 5000;
        for (int i = 0; i < kWaitMs && running; ++i) {
            bool diagOk = (diag_client->availability() == someip_fw::Availability::Available);
            if (diagOk) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    FotaLogAdapter::orchestrator().info(fota_events::SERVICE_READY,
        "CCU-FOTA Service started successfully",
        {flog::f_str("someip_application", someIpCfg.application)}
    );

    // ============================================================
    // 契约 B：TBOX 通用消息服务（vehicle.fota.v1）生产装配
    // (CCU-FOTA-DSN-CR-010 §状态与生命周期 / CR-011)
    // 复用进程唯一 SomeIpRuntime；Service/Instance 0x6101/0x0001 已由 Registry
    // 分配（constants.h 过渡 SSOT），generic Method/Event/Eventgroup ID 必须来自
    // Registry 分配并经运行配置注入。Registry 未分配/冲突/版本不一致 -> fail-closed
    // （禁止猜测 ID、禁止回退 Fake 或旧私有协议）。
    // ============================================================
    std::unique_ptr<ccu_fota::daemon::FotaDaemon> fota_daemon;
    if (fotaConfig.cloud.enabled) {
        ccu_fota::someip::TboxGenericTransportAddress tbox_generic_addr;
        bool resolved = false;
        try {
            resolved = ccu_fota::someip::resolveTboxGenericTransport(
                fotaConfig, tbox_generic_addr);
        } catch (const ccu_fota::FotaConfigException& e) {
            std::cerr << "FATAL: contract B Registry/config error: " << e.what()
                      << std::endl;
            return 1;
        }
        if (!resolved) {
            // Registry 未分配 generic ID -> fail-closed：禁止以猜测 ID 运行契约 B。
            std::cerr
                << "FATAL: contract B blocked: TBOX generic transport Method/Event/"
                << "Eventgroup ID not allocated by Service Registry (fota.someip."
                << "generic_transport.*). Set fota.cloud.enabled=false to run "
                << "contract A only; do not guess IDs." << std::endl;
            return 1;
        }

        try {
            // 生产车内端口（复用契约 A 采集；其余 fail-closed 保守占位，独立边界）。
            auto prod_inv = std::make_shared<ccu_fota::ota::InventoryProviderFromAssembler>(
                assembler, fotaConfig.cloud.protocolVersion);
            auto prod_consent = std::make_shared<ccu_fota::ota::ConservativeConsentProvider>();
            auto prod_dl = std::make_shared<ccu_fota::ota::ConservativePackageDownloader>();
            auto prod_cond = std::make_shared<ccu_fota::ota::ConservativeVehicleConditionProvider>();
            auto prod_exec = std::make_shared<ccu_fota::ota::ConservativeInstallExecutor>();
            auto prod_log = std::make_shared<ccu_fota::ota::ConservativeLogCollector>();

            // 时钟门禁判定来源：上行 Envelope 的 timestamp_ms/expire_at_ms 由
            // CLOCK_REALTIME 推导，CCU 上电后 RTC 可能尚未与可信时间源同步，
            // 此时 expire_at_ms 落在过去，云侧网关判定 MESSAGE_EXPIRED 并丢入上行
            // DLQ。注入后，编排器在时钟同步前不发任何上行（含首次版本/清单上报）。
            ccu_fota::ota::SystemClockSyncConfig clock_cfg;
            clock_cfg.minValidEpochMs = fotaConfig.cloud.clockMinValidEpochMs;
            auto prod_clock =
                std::make_shared<ccu_fota::ota::SystemClockSyncProvider>(clock_cfg);

            ccu_fota::ota::FotaOrchestratorConfig orch_cfg;
            orch_cfg.protocolVersion = fotaConfig.cloud.protocolVersion;
            orch_cfg.deviceId = "ccu-fota";
            // 车辆 VIN 是契约 B Envelope 身份的一部分：云侧（VAGW/IOV-OTA）以
            // Kafka Key(VIN) = Envelope.vin 为硬契约，空 VIN 上行会被判为不可恢复
            // 契约错误并整体丢入 DLQ。VIN 与车内软件清单同源：DIAG（CR-007 Client）。
            // 解析失败 fail-closed：不启动契约 B，绝不发出无 VIN 的上行。
            std::string vehicle_vin;
            if (diag_client->availability() != someip_fw::Availability::Available ||
                !diag_client->getVin(vehicle_vin) || vehicle_vin.empty()) {
                FotaLogAdapter::orchestrator().error(
                    fota_events::CLOUD_VIN_UNRESOLVED,
                    "Vehicle VIN unresolved from DIAG, contract B blocked",
                    {flog::f_str("source", "diag"),
                     flog::f_str("availability",
                                 diag_client->availability() ==
                                         someip_fw::Availability::Available
                                     ? "Available"
                                     : "Unavailable")});
                return 1;
            }
            orch_cfg.vin = vehicle_vin;
            orch_cfg.cloudCallTimeout = fotaConfig.cloud.cloudCallTimeout;
            orch_cfg.eventOutboxMax = fotaConfig.cloud.eventOutboxMax;
            orch_cfg.taskCheckInterval = fotaConfig.cloud.taskCheckInterval;
            orch_cfg.requireClockSync = fotaConfig.cloud.requireClockSync;
            orch_cfg.clockSyncPollInterval = fotaConfig.cloud.clockSyncPollInterval;

            fota_daemon = ccu_fota::daemon::FotaDaemon::create(
                runtime, tbox_generic_addr, fotaConfig.transport, orch_cfg,
                /*businessTrigger=*/fotaConfig.cloud.enabled,
                std::move(prod_inv), std::move(prod_consent),
                std::move(prod_dl), std::move(prod_cond),
                std::move(prod_exec), std::move(prod_log),
                cloud_state_store, std::move(prod_clock));
            fota_daemon->start();  // transport -> 下行订阅 -> reconcile -> 触发

            FotaLogAdapter::orchestrator().info(
                "fota.contract_b.ready",
                "Contract B (vehicle.fota.v1 over TBOX generic transport) started",
                {flog::f_str("service", "vehicle.fota"),
                 flog::f_str("generic_method", hex_id(tbox_generic_addr.method))});
        } catch (const std::exception& e) {
            std::cerr << "FATAL: contract B wiring failed: " << e.what()
                      << std::endl;
            return 1;
        }
    } else {
        FotaLogAdapter::orchestrator().info(
            "fota.contract_b.disabled",
            "Contract B disabled (fota.cloud.enabled=false)");
    }

    // 契约 A 的启动自动清单提交（旧 0x6101/0x0001 推送）已随 CR-011 移除；
    // 车内软件清单改由契约 B 的 InventoryProviderFromAssembler 随 TaskCheck 上报。

    // Main loop
    while (running) {
        sleep(1);
    }

    // ============================================================
    // CCU-FOTA-DSN-CR-007 关闭顺序：
    //   1. 服务状态 Stopping，Provider 拒绝新请求 (stopOffer)
    //   2. 停止新自动任务并取消业务 retry timer（reporter 生命周期结束）
    //   3. 保存/flush 必要 active_job 状态（Store 持有，析构时 flush）
    //   4. release DIAG/TBOX service
    //   5. stopOffer FOTA Provider（有界等待/取消 in-flight handler/call）
    //   6. runtime.stop()，停止 executor 并验证资源清零
    // ============================================================
    FotaLogAdapter::orchestrator().info(fota_events::SERVICE_SHUTTING_DOWN,
        "Stopping CCU-FOTA Service..."
    );

    // 契约 B 关闭顺序（CR-010 §状态与生命周期）：
    //   拒绝新调用 -> 停止 timer -> 保存业务状态 -> 收敛 in-flight ->
    //   取消下行订阅 -> 销毁 transport（SomeIpRuntime 由下方统一 stop）。
    if (fota_daemon) {
        fota_daemon->stop();
        fota_daemon.reset();
    }

    diag_client->releaseService();   // 4: release DIAG
    runtime.stop();                  // 6: 停止 Runtime/executor（幂等）

    FotaLogAdapter::orchestrator().info("fota.service.stopped",
        "CCU-FOTA Service stopped"
    );

    return 0;
}
