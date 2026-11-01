#pragma once

// =============================================================================
// include/ccu/fota/daemon/fota_daemon.hpp
// CCU-FOTA 契约 B daemon 装配 (CCU-FOTA-DSN-CR-010 §状态与生命周期 / CR-011)
// =============================================================================
// FotaDaemon 将契约 B 的传输/编解码/云代理/编排器装配到进程唯一 SomeIpRuntime 上，
// 并与 daemon 生命周期绑定。main.cpp 与 daemon 级测试共用同一装配代码路径。
//
// 职责隔离（CR-010/011）：
//   * 复用调用方持有的进程唯一 SomeIpRuntime，不创建第二个 Runtime/application；
//     本类只在其上 createClient。
//   * 下行经 FotaCloudProxy 订阅（service=vehicle.fota），解码 ControlCommand 后
//     交 FotaOrchestrator.applyControl；回执经同一通用端口发送。
//   * 不在 SOME/IP I/O 线程推进状态机（transport 内部有界队列 + 独立 executor）。
//
// 启动顺序：transport -> codec/proxy -> orchestrator -> 下行订阅 -> reconcile ->
// 初始 step -> 开放业务触发（定时 step）。
// 关闭顺序：拒绝新调用 -> 停止 timer -> 保存业务状态 -> 收敛 in-flight -> 取消
// 下行订阅 -> 销毁 transport。SomeIpRuntime.stop() 由调用方（main.cpp）统一执行。
// =============================================================================

#include "ccu/fota/config/fota_config.hpp"
#include "ccu/fota/ota/fota_cloud_proxy_via_transport.hpp"
#include "ccu/fota/ota/fota_orchestrator.hpp"
#include "ccu/fota/ota/someip_vehicle_message_transport.hpp"
#include "ccu/fota/someip/tbox_generic_transport_contract.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"

#include "ccu/fw/someip/runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <thread>

namespace ccu_fota {
namespace daemon {

class FotaDaemon {
public:
    // runtime 必须已 start()（进程唯一 Runtime）。address 必须 fullyAllocated
    // （resolveTboxGenericTransport 校验通过）。端口与 cloudStore 由共享所有权持有
    // （daemon 生命周期内保持存活）。businessTrigger：true 时 start() 会启动定时
    // step()（开放业务触发）。
    static std::unique_ptr<FotaDaemon> create(
        ccu::fw::someip::SomeIpRuntime& runtime,
        const ccu_fota::someip::TboxGenericTransportAddress& address,
        ccu_fota::SomeIpTransportConfig transportCfg,
        ota::FotaOrchestratorConfig orchCfg,
        bool businessTrigger,
        std::shared_ptr<ota::InventoryProvider> inv,
        std::shared_ptr<ota::ConsentProvider> consent,
        std::shared_ptr<ota::PackageDownloader> dl,
        std::shared_ptr<ota::VehicleConditionProvider> cond,
        std::shared_ptr<ota::InstallExecutor> exec,
        std::shared_ptr<ota::LogCollector> log,
        std::shared_ptr<store::fota::FotaCloudStateStore> cloudStore);

    ~FotaDaemon();

    FotaDaemon(const FotaDaemon&) = delete;
    FotaDaemon& operator=(const FotaDaemon&) = delete;

    // 开放：transport.start -> 下行订阅 -> reconcile -> 初始 step -> 业务触发。
    void start();
    // 关闭：见头注释顺序。runtime.stop() 由调用方执行。
    void stop();

    // ---- 状态查询（daemon 级测试断言）----
    ota::SomeIpVehicleMessageTransport& transport() const;
    ota::FotaCloudProxyViaTransport& cloud() const;
    ota::FotaOrchestrator& orchestrator() const;
    bool running() const { return running_.load(); }
    bool businessTriggerEnabled() const { return businessTrigger_; }

private:
    FotaDaemon(ccu::fw::someip::SomeIpRuntime& runtime,
               const ccu_fota::someip::TboxGenericTransportAddress& address,
               ccu_fota::SomeIpTransportConfig transportCfg,
               ota::FotaOrchestratorConfig orchCfg,
               bool businessTrigger,
               std::shared_ptr<ota::InventoryProvider> inv,
               std::shared_ptr<ota::ConsentProvider> consent,
               std::shared_ptr<ota::PackageDownloader> dl,
               std::shared_ptr<ota::VehicleConditionProvider> cond,
               std::shared_ptr<ota::InstallExecutor> exec,
               std::shared_ptr<ota::LogCollector> log,
               std::shared_ptr<store::fota::FotaCloudStateStore> cloudStore);

    void timerLoop();

    ccu::fw::someip::SomeIpRuntime& runtime_;
    ccu_fota::SomeIpTransportConfig transportCfg_;
    ota::FotaOrchestratorConfig orchCfg_;
    bool businessTrigger_;

    std::unique_ptr<ota::SomeIpVehicleMessageTransport> transport_;
    std::unique_ptr<ota::FotaCloudProxyViaTransport> cloud_;
    std::unique_ptr<ota::FotaOrchestrator> orch_;
    ota::Subscription downlinkSub_;
    // 共享所有权持有：端口与 cloudStore 在 daemon 生命周期内存活。
    std::shared_ptr<ota::InventoryProvider> inv_;
    std::shared_ptr<ota::ConsentProvider> consent_;
    std::shared_ptr<ota::PackageDownloader> dl_;
    std::shared_ptr<ota::VehicleConditionProvider> cond_;
    std::shared_ptr<ota::InstallExecutor> exec_;
    std::shared_ptr<ota::LogCollector> log_;
    std::shared_ptr<store::fota::FotaCloudStateStore> cloudStore_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::thread timerThread_;
    mutable std::mutex timerMutex_;
    std::condition_variable timerCv_;
};

} // namespace daemon
} // namespace ccu_fota
