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
        FotaOrchestratorConfig cfg;
        orch = std::make_unique<FotaOrchestrator>(*cloud, *inv, *consent, *dl, *cond,
                                                  *exec, *log, *cloudStore, cfg);
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
        FotaOrchestratorConfig cfg;
        orch = std::make_unique<FotaOrchestrator>(*cloud, *inv, *consent, *dl, *cond,
                                                  *exec, *log, *cloudStore, cfg);
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
    auto last = h.runToTerminal(20);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Ended);
    EXPECT_TRUE(last.terminal);
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
