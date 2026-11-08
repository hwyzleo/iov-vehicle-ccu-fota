// =============================================================================
// tests/ota/ota_integration_test.cpp
// CCU-FOTA 车云 FOTA 编排集成测试 (CCU-FOTA-DSN-CR-009 §验收 / CR-010 迁移 / CR-011)
// fake TBOX/cloud server = FakeVehicleMessageTransport（通用传输端口）；
// FotaCloudProxyViaTransport 在端口之上实现强类型 FotaCloudProxy；Mock 端口驱动车内副作用。
// 覆盖：Happy Path 九阶段、下载/许可/事件/云超时故障恢复、断电恢复、授权拒绝、策略、控制去重。
// =============================================================================

#include "ccu/fota/ota/mock/fake_vehicle_message_transport.hpp"
#include "ccu/fota/ota/mock/mock_ports.hpp"
#include "ccu/fota/ota/fota_cloud_proxy_via_transport.hpp"
#include "ccu/fota/ota/fota_orchestrator.hpp"
#include "ccu/fota/ota/ports/production_ports.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"
#include "ccu/fota/store/fota_state_migration.hpp"
#include "ccu/fota/store/fota_state_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;
using namespace ccu_fota::ota;
using namespace ccu_fota::ota::mock;
using namespace ccu_fota::store;
using namespace ccu_fota::store::fota;

namespace {

fs::path makeUniqueRoot() {
    static int counter = 0;
    auto root = fs::temp_directory_path() /
                ("fota-ota-int-" + std::to_string(getpid()) +
                 "-" + std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

std::string happyScenarioJson() {
    return R"({
      "scenario": "happy-path",
      "clock": "virtual",
      "inventory": {"mode": "FULL", "baseline": "BASE-001", "fota_master_version": "1.0.0"},
      "packages": [{"package_id": "PKG-VCU-001", "bytes": 1048576, "failures_before_success": 0}],
      "executor": {
        "stages": [
          {"stage": "INSTALL", "progress": [25, 50, 100], "result": "SUCCEEDED"},
          {"stage": "REBOOT", "result": "SUCCEEDED"},
          {"stage": "POST_CHECK", "result": "SUCCEEDED"}
        ]
      },
      "faults": []
    })";
}

std::string scenarioWithFault(const std::string& fault) {
    auto base = happyScenarioJson();
    auto pos = base.find("\"faults\": []");
    base.replace(pos, std::string("\"faults\": []").size(),
                 std::string("\"faults\": [\"") + fault + "\"]");
    return base;
}

// 完整 Mock 装配。所有组件共享同一 scenario 与 store；云侧经通用传输端口接入。
struct MockHarness {
    fs::path root;
    std::shared_ptr<FotaStateStore> fotaStore;
    std::unique_ptr<FotaCloudStateStore> cloudStore;
    std::unique_ptr<FakeVehicleMessageTransport> transport;
    std::unique_ptr<FotaCloudProxyViaTransport> cloud;
    std::unique_ptr<MockInventoryProvider> inv;
    std::unique_ptr<MockConsentProvider> consent;
    std::unique_ptr<MockPackageDownloader> dl;
    std::unique_ptr<MockVehicleConditionProvider> cond;
    std::unique_ptr<MockInstallExecutor> exec;
    std::unique_ptr<MockLogCollector> log;
    // CCU-FOTA-DSN-CR-013：安装结果登记端口（Fake DIAG 侧，幂等 + 可注入失败）。
    std::unique_ptr<FakeInstalledVersionRegistrar> registrar;
    std::unique_ptr<FotaOrchestrator> orch;

    void init(const std::string& scenarioJson) {
        root = makeUniqueRoot();
        auto s = FotaStateStore::open(root, 100, 3600000);
        fotaStore = std::make_shared<FotaStateStore>(std::move(s));
        // 启动迁移（幂等；本 harness 无旧 key，直接返回）
        auto rawStore = fotaStore->underlyingStore();
        migrateOtaToFota(rawStore);
        cloudStore = std::make_unique<FotaCloudStateStore>(fotaStore->underlyingStore());
        auto scenario = parseScenario(scenarioJson);
        transport = std::make_unique<FakeVehicleMessageTransport>(scenario);
        cloud = std::make_unique<FotaCloudProxyViaTransport>(*transport);
        inv = std::make_unique<MockInventoryProvider>(scenario);
        consent = std::make_unique<MockConsentProvider>();
        dl = std::make_unique<MockPackageDownloader>(scenario);
        cond = std::make_unique<MockVehicleConditionProvider>();
        exec = std::make_unique<MockInstallExecutor>(scenario);
        log = std::make_unique<MockLogCollector>();
        registrar = std::make_unique<FakeInstalledVersionRegistrar>();
        FotaOrchestratorConfig cfg;
        // 测试内不等待同身份退避（退避语义由 planner 单测覆盖）。
        cfg.inventoryIdentityRetryBackoff = std::chrono::milliseconds(0);
        // CCU-FOTA-DSN-CR-012：注入 durable revision 分配器（生产由 main.cpp 注入）。
        // CCU-FOTA-DSN-CR-013：注入登记端口（生产由 DIAG SOME/IP Client 实现）。
        orch = std::make_unique<FotaOrchestrator>(
            *cloud, *inv, *consent, *dl, *cond, *exec, *log, *cloudStore, cfg,
            /*clockSync=*/nullptr,
            [this]() { return fotaStore->allocateSnapshotSeq(); },
            registrar.get());
    }

    // 重新装配 orchestrator（模拟重启），复用同一 store。
    void restart(const std::string& scenarioJson) {
        auto scenario = parseScenario(scenarioJson);
        transport = std::make_unique<FakeVehicleMessageTransport>(scenario);
        cloud = std::make_unique<FotaCloudProxyViaTransport>(*transport);
        inv = std::make_unique<MockInventoryProvider>(scenario);
        consent = std::make_unique<MockConsentProvider>();
        dl = std::make_unique<MockPackageDownloader>(scenario);
        cond = std::make_unique<MockVehicleConditionProvider>();
        exec = std::make_unique<MockInstallExecutor>(scenario);
        log = std::make_unique<MockLogCollector>();
        // 重启复用同一 Fake DIAG 侧幂等状态（模拟 DIAG 记录不丢失）。
        if (!registrar) registrar = std::make_unique<FakeInstalledVersionRegistrar>();
        FotaOrchestratorConfig cfg;
        cfg.inventoryIdentityRetryBackoff = std::chrono::milliseconds(0);
        orch = std::make_unique<FotaOrchestrator>(
            *cloud, *inv, *consent, *dl, *cond, *exec, *log, *cloudStore, cfg,
            /*clockSync=*/nullptr,
            [this]() { return fotaStore->allocateSnapshotSeq(); },
            registrar.get());
        orch->reconcileOnStart();
    }

    StepOutcome runToTerminal(int maxSteps = 50) {
        StepOutcome last;
        for (int i = 0; i < maxSteps; ++i) {
            last = orch->step();
            if (last.terminal) break;
        }
        return last;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 1. Happy Path：完整九阶段交互成功
// ---------------------------------------------------------------------------
TEST(OtaIntegration, HappyPathCompletes) {
    MockHarness h;
    h.init(happyScenarioJson());

    auto last = h.runToTerminal();
    EXPECT_TRUE(last.terminal) << "action=" << last.action << " err=" << last.error;
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    EXPECT_TRUE(h.transport->finalAccepted());
    EXPECT_GT(h.orch->acceptedSequenceNo(), 0u);

    // 持久化：vehicle_task 记录存在且为 COMPLETED
    auto vt = h.cloudStore->loadVehicleTask();
    ASSERT_TRUE(vt.has_value());
    EXPECT_EQ(vt->vehicleTaskState, "COMPLETED");
    // 下载记录存在且 ready
    auto dl = h.cloudStore->loadDownload("PKG-VCU-001");
    ASSERT_TRUE(dl.has_value());
    EXPECT_TRUE(dl->ready);
    EXPECT_EQ(dl->verifyStatus, "SUCCEEDED");
}

// ---------------------------------------------------------------------------
// 2. 下载断网恢复
// ---------------------------------------------------------------------------
TEST(OtaIntegration, DownloadDisconnectRecovers) {
    MockHarness h;
    h.init(scenarioWithFault("download_disconnect"));
    auto last = h.runToTerminal(80);
    EXPECT_TRUE(last.terminal);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
}

// ---------------------------------------------------------------------------
// 3. hash 校验失败恢复
// ---------------------------------------------------------------------------
TEST(OtaIntegration, HashFailedRecovers) {
    MockHarness h;
    h.init(scenarioWithFault("hash_failed"));
    auto last = h.runToTerminal(80);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
}

// ---------------------------------------------------------------------------
// 4. ETag 变化触发 RESET_OFFSET（per-package 记录保留新 etag）
// ---------------------------------------------------------------------------
TEST(OtaIntegration, EtagChangedResetsOffset) {
    MockHarness h;
    h.init(scenarioWithFault("etag_changed"));
    auto last = h.runToTerminal(80);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    auto dl = h.cloudStore->loadDownload("PKG-VCU-001");
    ASSERT_TRUE(dl.has_value());
    EXPECT_NE(dl->etag, "etag-PKG-VCU-001");
}

// ---------------------------------------------------------------------------
// 5. 门禁失败恢复（云端拒绝一次后许可）
// ---------------------------------------------------------------------------
TEST(OtaIntegration, GuardFailedRecovers) {
    MockHarness h;
    h.init(scenarioWithFault("guard_failed"));
    auto last = h.runToTerminal(80);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
}

// ---------------------------------------------------------------------------
// 6. 事件丢失恢复（BUFFERED 后补发）
// ---------------------------------------------------------------------------
TEST(OtaIntegration, EventDropRecovers) {
    MockHarness h;
    h.init(scenarioWithFault("event_drop"));
    auto last = h.runToTerminal(80);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    EXPECT_TRUE(h.transport->finalAccepted());
}

// ---------------------------------------------------------------------------
// 7. 事件乱序恢复（BUFFERED + 缺失范围补发）
// ---------------------------------------------------------------------------
TEST(OtaIntegration, EventReorderRecovers) {
    MockHarness h;
    h.init(scenarioWithFault("event_reorder"));
    auto last = h.runToTerminal(80);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
}

// ---------------------------------------------------------------------------
// 8. 云超时恢复
// ---------------------------------------------------------------------------
TEST(OtaIntegration, CloudTimeoutRecovers) {
    MockHarness h;
    h.init(scenarioWithFault("cloud_timeout"));
    auto last = h.runToTerminal(80);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
}

// ---------------------------------------------------------------------------
// 9. 授权拒绝 -> Ended
// ---------------------------------------------------------------------------
TEST(OtaIntegration, ConsentRejectedEnds) {
    MockHarness h;
    h.init(happyScenarioJson());
    h.consent->setRejected(true);

    // 推进到 ENDED（终态本身不再“永久 terminal”，见 CR-012 D8）。
    bool ended = false;
    for (int i = 0; i < 20 && !ended; ++i) {
        h.orch->step();
        ended = (h.orch->vehicleTaskState() == VehicleTaskState::Ended);
    }
    EXPECT_TRUE(ended);

    // 终态之后回到检测态，继续周期任务检测。
    auto reset = h.orch->step();
    EXPECT_EQ(reset.action, "resetToDetection");
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::None);
}

// ---------------------------------------------------------------------------
// 10. 断电恢复：执行到下载后重启，新 orchestrator 从 durable 状态恢复并完成
// ---------------------------------------------------------------------------
TEST(OtaIntegration, RestartResumesAndCompletes) {
    MockHarness h;
    h.init(happyScenarioJson());

    for (int i = 0; i < 20; ++i) {
        auto last = h.orch->step();
        if (h.orch->vehicleTaskState() == VehicleTaskState::Ready ||
            h.orch->vehicleTaskState() == VehicleTaskState::WaitingWindow ||
            h.orch->vehicleTaskState() == VehicleTaskState::PermitPending) {
            break;
        }
    }
    VehicleTaskState midState = h.orch->vehicleTaskState();
    ASSERT_NE(midState, VehicleTaskState::None);
    ASSERT_NE(midState, VehicleTaskState::Completed);

    h.restart(happyScenarioJson());
    EXPECT_EQ(h.orch->vehicleTaskState(), midState)
        << "reconcile should restore state";

    auto last = h.runToTerminal(80);
    EXPECT_TRUE(last.terminal);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    EXPECT_TRUE(h.transport->finalAccepted());
}

// ---------------------------------------------------------------------------
// 11. 策略同步
// ---------------------------------------------------------------------------
TEST(OtaIntegration, PolicySync) {
    MockHarness h;
    h.init(happyScenarioJson());
    EXPECT_TRUE(h.orch->syncPolicy());
    auto p = h.cloudStore->loadPolicy();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->preferenceVersion, "pv-1");
    EXPECT_FALSE(p->conflict);
}

// ---------------------------------------------------------------------------
// 12. 控制去重：相同 controlRevision 不重复应用
// ---------------------------------------------------------------------------
TEST(OtaIntegration, ControlDedup) {
    MockHarness h;
    h.init(happyScenarioJson());
    h.runToTerminal(80);
    ::vehicle::fota::v1::ControlCommand cmd;
    cmd.set_control_id("CTRL-1");
    cmd.set_control_revision(1);
    cmd.set_action(::vehicle::fota::v1::CONTROL_ACTION_PAUSE);
    cmd.set_scope(::vehicle::fota::v1::CONTROL_SCOPE_EXECUTION);
    cmd.set_apply_mode(::vehicle::fota::v1::APPLY_MODE_AT_SAFE_POINT);
    cmd.set_issued_at_ms(1000);
    auto o1 = h.orch->applyControl(cmd);
    EXPECT_EQ(o1.status, ::vehicle::fota::v1::CONTROL_ACK_STATUS_APPLIED);
    auto o2 = h.orch->applyControl(cmd);  // 相同 revision
    EXPECT_EQ(o2.status, ::vehicle::fota::v1::CONTROL_ACK_STATUS_RECEIVED);
}

// ---------------------------------------------------------------------------
// 13. 时钟门禁：系统时钟未与可信时间源同步前不发出任何上行
//
// 上行 Envelope 的 timestamp_ms/expire_at_ms 由 CLOCK_REALTIME 推导。CCU 上电后
// RTC 可能尚未同步，此时 expire_at_ms 落在过去，云侧网关按契约判定 MESSAGE_EXPIRED
// 并丢入上行 DLQ（消息永久丢失且无业务回执）。因此启动期的版本/清单上报必须等到
// 时钟同步之后，而不是先发一条注定被拒的消息。
// ---------------------------------------------------------------------------
namespace {

// 可控时钟判定来源：由测试切换就绪状态并记录查询次数。
class FakeClockSyncProvider : public ClockSyncProvider {
public:
    ClockSyncStatus status() override {
        ++queries;
        ClockSyncStatus st;
        st.synchronized = synchronized;
        // 未同步时返回 RTC 回落后的时间（1970），同步后返回合理时间。
        st.systemTimeMs = synchronized ? 1767225600000LL : 1000LL;
        st.detail = synchronized ? "test: synced" : "test: RTC unset";
        return st;
    }

    bool synchronized = false;
    int queries = 0;
};

} // namespace

TEST(OtaIntegration, ClockSyncGateWithholdsUplinkUntilSynchronized) {
    MockHarness h;
    h.init(happyScenarioJson());

    FakeClockSyncProvider clock;  // 初始未同步
    FotaOrchestratorConfig cfg;   // requireClockSync 缺省 true
    cfg.inventoryIdentityRetryBackoff = std::chrono::milliseconds(0);
    h.orch = std::make_unique<FotaOrchestrator>(
        *h.cloud, *h.inv, *h.consent, *h.dl, *h.cond, *h.exec, *h.log, *h.cloudStore,
        cfg, &clock, [&h]() { return h.fotaStore->allocateSnapshotSeq(); },
        h.registrar.get());

    // 时钟未同步：反复 step 都停在门禁前，状态不推进，云侧收不到任何上行。
    for (int i = 0; i < 3; ++i) {
        auto o = h.orch->step();
        EXPECT_TRUE(o.awaitingClockSync);
        EXPECT_EQ(o.action, "awaitClockSync");
        EXPECT_FALSE(o.terminal);
        EXPECT_NE(o.error.find("system clock not synchronized"), std::string::npos);
        EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::None);
    }
    EXPECT_EQ(clock.queries, 3);
    EXPECT_EQ(h.transport->acceptedSequenceNo(), 0u);
    EXPECT_FALSE(h.transport->finalAccepted());

    // 时钟同步后上报不丢，只是延后：同一实例继续推进到终态。
    clock.synchronized = true;
    auto last = h.runToTerminal(80);
    EXPECT_FALSE(last.awaitingClockSync);
    EXPECT_TRUE(last.terminal);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    EXPECT_TRUE(h.transport->finalAccepted());
}

TEST(OtaIntegration, ClockSyncGateDisabledKeepsLegacyBehaviour) {
    MockHarness h;
    h.init(happyScenarioJson());

    FakeClockSyncProvider clock;  // 保持未同步
    FotaOrchestratorConfig cfg;
    cfg.requireClockSync = false;  // 实验台：显式关闭门禁
    cfg.inventoryIdentityRetryBackoff = std::chrono::milliseconds(0);
    h.orch = std::make_unique<FotaOrchestrator>(
        *h.cloud, *h.inv, *h.consent, *h.dl, *h.cond, *h.exec, *h.log, *h.cloudStore,
        cfg, &clock, [&h]() { return h.fotaStore->allocateSnapshotSeq(); },
        h.registrar.get());

    auto o = h.orch->step();
    EXPECT_FALSE(o.awaitingClockSync);
    EXPECT_EQ(o.action, "checkTask");
    EXPECT_EQ(clock.queries, 0);  // 门禁关闭时不查询时钟
}

// ---------------------------------------------------------------------------
// 14. SystemClockSyncProvider：合理性下限与就绪锁存
// ---------------------------------------------------------------------------
TEST(SystemClockSync, RejectsClockBelowMinValidEpoch) {
    SystemClockSyncConfig cfg;
    cfg.minValidEpochMs = 4102444800000LL;  // 2100-01-01，真实系统时间必定更早
    cfg.syncMarkerPath = "";                // 不依赖授时服务标记
    SystemClockSyncProvider p(cfg);

    auto st = p.status();
    EXPECT_FALSE(st.synchronized);
    EXPECT_GT(st.systemTimeMs, 0);
    EXPECT_NE(std::string(st.detail).find("min_valid_epoch"), std::string::npos);
}

TEST(SystemClockSync, AcceptsPlausibleClockAndLatches) {
    SystemClockSyncConfig cfg;
    cfg.minValidEpochMs = 1LL;
    cfg.syncMarkerPath = "";
    cfg.requireSyncEvidence = false;  // 无授时证据来源，仅做合理性下限判定
    SystemClockSyncProvider p(cfg);

    EXPECT_TRUE(p.status().synchronized);
    // 第二次查询走锁存路径，不再重复判定（避免门禁抖动导致业务反复停摆）。
    auto second = p.status();
    EXPECT_TRUE(second.synchronized);
    EXPECT_STREQ(second.detail, "latched");
}
