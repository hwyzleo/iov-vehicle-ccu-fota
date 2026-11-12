// =============================================================================
// tests/ota/installed_version_registration_test.cpp
// 安装结果登记集成测试 (CCU-FOTA-DSN-CR-013 §测试矩阵 集成 + 验收标准)
// -----------------------------------------------------------------------------
// 装配：FotaOrchestrator + Mock 车内端口 + Fake 云侧端点 + Fake DIAG 登记端口
// （FakeInstalledVersionRegistrar，与生产同端口/同幂等语义）+ framework store。
//
// 覆盖：
//   1. resultAccepted 之后、Execution 关闭之前登记恰一次；
//   2. 登记失败 -> 保持 RESULT_PENDING -> 同身份重试成功后收口（Fake DIAG 只应用一次）；
//   3. DUPLICATE 回放 -> 记录既有 registrationId 并收口；
//   4. 登记成功后重启 -> 不重复登记（fota.execution 记录生效）；
//   5. 回滚 / 失败不登记；
//   6. 登记能力不可用：不阻止清单读取与上报，但阻止新安装；
//   7. 已启动成功执行登记失败/不可用时停留 RESULT_PENDING，能力恢复后登记成功收口。
// =============================================================================

#include "ccu/fota/ota/fota_cloud_proxy_via_transport.hpp"
#include "ccu/fota/ota/fota_orchestrator.hpp"
#include "ccu/fota/ota/mock/fake_vehicle_message_transport.hpp"
#include "ccu/fota/ota/mock/mock_ports.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"
#include "ccu/fota/store/fota_state_store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace ccu_fota::ota;
using namespace ccu_fota::ota::mock;
using namespace ccu_fota::store;
using namespace ccu_fota::store::fota;

namespace {

fs::path makeUniqueRoot() {
    static int counter = 0;
    auto root = fs::temp_directory_path() /
                ("fota-reg-" + std::to_string(getpid()) + "-" +
                 std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

std::string scenarioJson(const std::string& faults = "[]") {
    return R"({
      "scenario": "installed-version-registration",
      "clock": "virtual",
      "inventory": {"mode": "FULL", "baseline": "BASE-001", "fota_master_version": "1.0.0"},
      "packages": [{"package_id": "PKG-VCU-001", "bytes": 4096, "failures_before_success": 0}],
      "executor": {
        "stages": [
          {"stage": "INSTALL", "progress": [100], "result": "SUCCEEDED"},
          {"stage": "REBOOT", "result": "SUCCEEDED"},
          {"stage": "POST_CHECK", "result": "SUCCEEDED"}
        ]
      },
      "faults": )" + faults + R"(
    })";
}

// 报告回滚的执行器（模拟真实安装回滚成功）。
class RollbackReportingExecutor : public MockInstallExecutor {
public:
    explicit RollbackReportingExecutor(ScenarioScript s) : MockInstallExecutor(std::move(s)) {}

    FinalResultData readFinalResult() override {
        FinalResultData d = MockInstallExecutor::readFinalResult();
        d.rollbackOccurred = true;
        return d;
    }
};

struct Harness {
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
    std::unique_ptr<FakeInstalledVersionRegistrar> registrar;
    std::unique_ptr<FotaOrchestrator> orch;

    void init(const std::string& json, bool rollbackExecutor = false) {
        root = makeUniqueRoot();
        auto s = FotaStateStore::open(root, 100, 3600000);
        fotaStore = std::make_shared<FotaStateStore>(std::move(s));
        cloudStore = std::make_unique<FotaCloudStateStore>(fotaStore->underlyingStore());
        auto scenario = parseScenario(json);
        transport = std::make_unique<FakeVehicleMessageTransport>(scenario);
        cloud = std::make_unique<FotaCloudProxyViaTransport>(*transport);
        inv = std::make_unique<MockInventoryProvider>(scenario);
        consent = std::make_unique<MockConsentProvider>();
        dl = std::make_unique<MockPackageDownloader>(scenario);
        cond = std::make_unique<MockVehicleConditionProvider>();
        if (rollbackExecutor) {
            exec = std::make_unique<RollbackReportingExecutor>(scenario);
        } else {
            exec = std::make_unique<MockInstallExecutor>(scenario);
        }
        log = std::make_unique<MockLogCollector>();
        registrar = std::make_unique<FakeInstalledVersionRegistrar>();
        FotaOrchestratorConfig cfg;
        cfg.inventoryIdentityRetryBackoff = std::chrono::milliseconds(0);
        orch = std::make_unique<FotaOrchestrator>(
            *cloud, *inv, *consent, *dl, *cond, *exec, *log, *cloudStore, cfg,
            /*clockSync=*/nullptr,
            [this]() { return fotaStore->allocateSnapshotSeq(); },
            registrar.get());
    }

    // 模拟进程重启：复用同一 durable store 与同一 Fake DIAG 幂等状态。
    void restart(const std::string& json) {
        auto scenario = parseScenario(json);
        transport = std::make_unique<FakeVehicleMessageTransport>(scenario);
        cloud = std::make_unique<FotaCloudProxyViaTransport>(*transport);
        FotaOrchestratorConfig cfg;
        cfg.inventoryIdentityRetryBackoff = std::chrono::milliseconds(0);
        orch = std::make_unique<FotaOrchestrator>(
            *cloud, *inv, *consent, *dl, *cond, *exec, *log, *cloudStore, cfg,
            /*clockSync=*/nullptr,
            [this]() { return fotaStore->allocateSnapshotSeq(); },
            registrar.get());
        orch->reconcileOnStart();
    }

    StepOutcome runToTerminal(int maxSteps = 60) {
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
// 1. 登记在 resultAccepted 之后、Execution 关闭之前，且恰一次
// ---------------------------------------------------------------------------
TEST(InstalledVersionRegistration, RegisteredOnceAfterResultAccepted) {
    Harness h;
    h.init(scenarioJson());

    auto last = h.runToTerminal();
    ASSERT_TRUE(last.terminal) << "action=" << last.action << " err=" << last.error;
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    EXPECT_TRUE(h.transport->finalAccepted());

    // 恰一次调用、恰一次应用（Fake DIAG 侧）。
    ASSERT_EQ(h.registrar->callCount, 1);
    EXPECT_EQ(h.registrar->appliedCountTotal, 1);
    EXPECT_EQ(h.registrar->duplicateCount, 0);

    // items 映射自冻结任务包清单（ecuId/packageId/packageRevision/targetVersion）。
    const auto& report = h.registrar->calls.front();
    EXPECT_EQ(report.vehicleTaskId, "VT-001");
    EXPECT_EQ(report.taskRevision, 1u);
    ASSERT_EQ(report.attemptNo, 1u);
    EXPECT_EQ(report.idempotencyKey,
              report.executionId + ":" + std::to_string(report.attemptNo));
    ASSERT_EQ(report.items.size(), 1u);
    EXPECT_EQ(report.items[0].ecuId, "VCU-001");
    EXPECT_EQ(report.items[0].packageId, "PKG-VCU-001");
    EXPECT_EQ(report.items[0].packageRevision, "prev-1");
    EXPECT_EQ(report.items[0].installedVersion, "2.0.0");

    // durable：fota.execution 记录携带登记标识（收口的必要条件）。
    auto ex = h.cloudStore->loadExecution();
    ASSERT_TRUE(ex.has_value());
    EXPECT_FALSE(ex->installedVersionRegistrationId.empty());
    EXPECT_EQ(ex->schemaVersion, ccu_fota::store::fota::schema::EXECUTION_VERSION);
}

// ---------------------------------------------------------------------------
// 2. 登记失败 -> RESULT_PENDING -> 同身份重试成功后收口（只应用一次）
// ---------------------------------------------------------------------------
TEST(InstalledVersionRegistration, RetrySameIdentityAppliesOnce) {
    Harness h;
    h.init(scenarioJson());
    h.registrar->scriptedDispositions = {
        RegistrationDisposition::RetryableFailed};

    StepOutcome last;
    bool sawPending = false;
    for (int i = 0; i < 60; ++i) {
        last = h.orch->step();
        if (h.orch->executionState() == ExecutionState::ResultPending) {
            sawPending = true;
            // Execution 未关闭：云端已接受最终结果，但登记未完成。
            EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Executing);
            auto ex = h.cloudStore->loadExecution();
            ASSERT_TRUE(ex.has_value());
            EXPECT_EQ(ex->executionState, "RESULT_PENDING");
            EXPECT_TRUE(ex->installedVersionRegistrationId.empty());
        }
        if (last.terminal) break;
    }
    EXPECT_TRUE(sawPending);
    ASSERT_TRUE(last.terminal);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);

    // 两次调用、一次应用（重试用同一 executionId+attemptNo，DIAG 侧幂等）。
    EXPECT_EQ(h.registrar->callCount, 2);
    EXPECT_EQ(h.registrar->appliedCountTotal, 1);
    ASSERT_GE(h.registrar->calls.size(), 2u);
    EXPECT_EQ(h.registrar->calls[0].idempotencyKey,
              h.registrar->calls[1].idempotencyKey);
    // 重试必须保持 items 内容不变（否则 DIAG 会返回 IDEMPOTENCY_CONFLICT）。
    ASSERT_EQ(h.registrar->calls[0].items.size(), h.registrar->calls[1].items.size());
    EXPECT_EQ(h.registrar->calls[0].items[0].installedVersion,
              h.registrar->calls[1].items[0].installedVersion);
}

// ---------------------------------------------------------------------------
// 3. DUPLICATE 回放：记录既有 registrationId 并收口
// ---------------------------------------------------------------------------
TEST(InstalledVersionRegistration, DuplicateDispositionCompletes) {
    Harness h;
    h.init(scenarioJson());
    h.registrar->scriptedDispositions = {RegistrationDisposition::Duplicate};

    auto last = h.runToTerminal();
    ASSERT_TRUE(last.terminal);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    auto ex = h.cloudStore->loadExecution();
    ASSERT_TRUE(ex.has_value());
    EXPECT_EQ(ex->installedVersionRegistrationId, "REG-EXISTING");
}

// ---------------------------------------------------------------------------
// 3b. REJECTED_INVALID（永久性拒绝）：不重试，记录 ERROR 后收口
// ---------------------------------------------------------------------------
TEST(InstalledVersionRegistration, PermanentlyRejectedIsNotRetried) {
    Harness h;
    h.init(scenarioJson());
    h.registrar->scriptedDispositions = {
        RegistrationDisposition::RejectedInvalid};

    auto last = h.runToTerminal();
    ASSERT_TRUE(last.terminal);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    // 仅一次调用：永久性拒绝不得无限重试。
    EXPECT_EQ(h.registrar->callCount, 1);
    EXPECT_EQ(h.registrar->appliedCountTotal, 0);
    // 没有登记标识：清单事实未变更，由运维修正后触发新一轮任务。
    auto ex = h.cloudStore->loadExecution();
    ASSERT_TRUE(ex.has_value());
    EXPECT_TRUE(ex->installedVersionRegistrationId.empty());
}

// ---------------------------------------------------------------------------
// 4. 登记成功后重启：不重复登记（fota.execution 记录生效）
// ---------------------------------------------------------------------------
TEST(InstalledVersionRegistration, RestartAfterRegistrationDoesNotReRegister) {
    Harness h;
    h.init(scenarioJson());
    auto last = h.runToTerminal();
    ASSERT_TRUE(last.terminal);
    ASSERT_EQ(h.registrar->callCount, 1);

    // 重启（同一 durable store 与同一 Fake DIAG 状态）。
    h.restart(scenarioJson());
    // 恢复后仍是终态：下一次 step 回到检测态，不产生任何登记调用。
    for (int i = 0; i < 5; ++i) h.orch->step();
    EXPECT_EQ(h.registrar->callCount, 1);
    EXPECT_EQ(h.registrar->appliedCountTotal, 1);
}

// ---------------------------------------------------------------------------
// 5. 回滚 / 失败：不登记
// ---------------------------------------------------------------------------
TEST(InstalledVersionRegistration, RolledBackExecutionIsNotRegistered) {
    Harness h;
    h.init(scenarioJson(), /*rollbackExecutor=*/true);

    auto last = h.runToTerminal();
    ASSERT_TRUE(last.terminal);
    EXPECT_TRUE(h.transport->finalAccepted());
    EXPECT_EQ(h.registrar->callCount, 0);
    EXPECT_EQ(h.registrar->appliedCountTotal, 0);
}

TEST(InstalledVersionRegistration, FailedExecutionIsNotRegistered) {
    Harness h;
    h.init(scenarioJson(R"(["final_retry_pending"])"));

    // 云端以 RETRY_PENDING 收口本次执行（失败）：Execution 进入 FAILED，
    // 不登记（失败/回滚成功都不写 DIAG 清单事实）。
    bool failed = false;
    for (int i = 0; i < 20 && !failed; ++i) {
        h.orch->step();
        failed = h.orch->executionState() == ExecutionState::Failed;
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(h.registrar->callCount, 0);
    EXPECT_EQ(h.registrar->appliedCountTotal, 0);
}

// ---------------------------------------------------------------------------
// 6. 登记能力不可用：清单读取与上报不受影响，但阻止新的安装
// ---------------------------------------------------------------------------
TEST(InstalledVersionRegistration, UnavailableCapabilityBlocksNewInstallOnly) {
    Harness h;
    h.init(scenarioJson());
    h.registrar->availableFlag = false;

    StepOutcome last;
    for (int i = 0; i < 12; ++i) {
        last = h.orch->step();
        if (last.error.find("registration unavailable") != std::string::npos) break;
    }

    // 清单已上报（读取/上报不受影响）。
    EXPECT_EQ(h.transport->inventoryVersionCount(), 1u);
    // 但未开始安装：云端未收到最终结果、无登记调用。
    EXPECT_FALSE(h.transport->finalAccepted());
    // 2026-11-12 修复：登记能力不可用属于静态能力缺失，不得把状态机永久钉在
    // WaitingWindow（那样不再 TaskCheck，本地清单会停止上报且现场无日志）。
    // 新行为：记录 FOTA-TASK-REGISTRATION-BLOCKED 并回退 DISCOVERED，
    // 保持清单上报，能力恢复后自动重新走到安装许可。
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Discovered);
    EXPECT_NE(last.error.find("FOTA-TASK-REGISTRATION-BLOCKED"), std::string::npos)
        << last.error;
    EXPECT_EQ(h.registrar->callCount, 0);
}

// ---------------------------------------------------------------------------
// 7. 已启动成功执行：登记失败/不可用 -> RESULT_PENDING；恢复后登记成功收口
// ---------------------------------------------------------------------------
TEST(InstalledVersionRegistration, PendingUntilRegistrationCapabilityRecovers) {
    Harness h;
    h.init(scenarioJson());
    h.registrar->scriptedDispositions = {
        RegistrationDisposition::RetryableFailed};

    // 第一步登记失败：最终结果已被接受，但 Execution 保持 RESULT_PENDING。
    StepOutcome last;
    bool pending = false;
    for (int i = 0; i < 20 && !pending; ++i) {
        last = h.orch->step();
        pending = h.orch->executionState() == ExecutionState::ResultPending;
    }
    ASSERT_TRUE(pending);
    EXPECT_TRUE(h.transport->finalAccepted());
    EXPECT_NE(h.orch->vehicleTaskState(), VehicleTaskState::Completed);

    // 能力进一步不可用：仍保持 RESULT_PENDING（不得跳过收口去关闭 Execution）。
    h.registrar->availableFlag = false;
    last = h.orch->step();
    EXPECT_EQ(h.orch->executionState(), ExecutionState::ResultPending);
    EXPECT_NE(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    EXPECT_NE(last.error.find("registration unavailable"), std::string::npos);

    // 能力恢复：使用同一身份登记成功并收口。
    h.registrar->availableFlag = true;
    last = h.runToTerminal();
    EXPECT_TRUE(last.terminal);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    EXPECT_EQ(h.registrar->appliedCountTotal, 1);
    // 历次调用均使用同一幂等身份（executionId:attemptNo）。
    ASSERT_GE(h.registrar->calls.size(), 2u);
    const std::string key = h.registrar->calls.front().idempotencyKey;
    for (const auto& c : h.registrar->calls) {
        EXPECT_EQ(c.idempotencyKey, key);
    }
    auto ex = h.cloudStore->loadExecution();
    ASSERT_TRUE(ex.has_value());
    EXPECT_FALSE(ex->installedVersionRegistrationId.empty());
}

// ---------------------------------------------------------------------------
// 8. 登记成功后清单内容变化：下一轮恰好新增 1 个 revision + FULL
//    （登记与云端接受之间不得由 FOTA 推算版本，CR-013 §内容变化检测）
// ---------------------------------------------------------------------------
TEST(InstalledVersionRegistration, VersionChangeAfterRegistrationAddsOneRevision) {
    Harness h;
    h.init(scenarioJson(R"(["inventory_no_task"])"));

    ASSERT_TRUE(h.orch->step().error.empty());
    ASSERT_EQ(h.transport->inventoryVersionCount(), 1u);

    // 模拟 DIAG 侧版本事实变化（登记 / seed->hardware 切换的效果）。
    h.inv->setSoftwareVersion("2.0.0");
    ASSERT_TRUE(h.orch->step().error.empty());

    EXPECT_EQ(h.transport->inventoryFullCount(), 2u);
    EXPECT_EQ(h.transport->inventoryVersionCount(), 2u);
    EXPECT_EQ(h.transport->cloudInventoryRevision(), 2u);

    // 内容未变：回到 DIGEST，不再新增版本（登记/上报不得自造版本）。
    ASSERT_TRUE(h.orch->step().error.empty());
    EXPECT_EQ(h.transport->inventoryVersionCount(), 2u);
}
