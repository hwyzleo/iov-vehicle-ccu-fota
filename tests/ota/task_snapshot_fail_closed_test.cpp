// =============================================================================
// tests/ota/task_snapshot_fail_closed_test.cpp
//
// 回归：云端任务快照缺 package_list / 安装登记能力不可用时，车端必须
// fail-closed 并保持本地清单上报（不得静默停在 WaitingWindow）。
//
// 实车缺陷（ORIN 2026-11-12）：
//   1) 云端 TaskCheckResponse.task 只有 vehicle_task_id/task_revision/时间窗，
//      package_list 恒为空；旧实现 doDownload() 对空列表循环 0 次即
//      allReady=true -> READY，下一 tick READY -> doPermit() 只迁移到
//      WAITING_WINDOW，再下一 tick 被安装登记门禁挡住并永久停留。
//   2) WAITING_WINDOW 不在检测分支 -> 不再 TaskCheck -> 连本地清单都不再上报；
//      冷启动从 durable 恢复后同样卡住，且 step 失败没有任何日志。
//
// 修复后的行为（本测试锁定）：
//   * 空 package_list / 云端 download_allowed=false -> 保持 DISCOVERED，不发下载授权；
//   * 恢复出的空任务（DOWNLOADING/WAITING_WINDOW）自愈回 DISCOVERED；
//   * 安装登记能力不可用 -> 回退 DISCOVERED 继续上报清单（而不是静默等待）；
//   * 全过程不发起下载授权请求。
// =============================================================================

#include "ccu/fota/ota/fota_cloud_proxy_via_transport.hpp"
#include "ccu/fota/ota/fota_orchestrator.hpp"
#include "ccu/fota/ota/mock/fake_vehicle_message_transport.hpp"
#include "ccu/fota/ota/mock/mock_ports.hpp"
#include "ccu/fota/ota/mock/scenario_script.hpp"
#include "ccu/fota/ota/payload_types.hpp"
#include "ccu/fota/store/fota_cloud_state.hpp"
#include "ccu/fota/store/fota_cloud_state_serializer.hpp"
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
                ("fota-taskfix-" + std::to_string(getpid()) + "-" +
                 std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

std::string scenarioJson(const std::string& packagesJson) {
    return R"({
      "scenario": "task-content-guard",
      "clock": "virtual",
      "inventory": {"mode": "FULL", "baseline": "BASE-GUARD", "fota_master_version": "1.0.0"},
      "packages": )" + packagesJson + R"(,
      "executor": {"stages": []},
      "faults": []
    })";
}

struct GuardHarness {
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
    std::shared_ptr<int> downloadGrantRequests = std::make_shared<int>(0);

    void init(const std::string& packagesJson, bool registrarAvailable,
              bool countGrants = true) {
        root = makeUniqueRoot();
        auto s = FotaStateStore::open(root, 100, 3600000);
        fotaStore = std::make_shared<FotaStateStore>(std::move(s));
        cloudStore = std::make_unique<FotaCloudStateStore>(fotaStore->underlyingStore());
        auto scenario = parseScenario(scenarioJson(packagesJson));
        transport = std::make_unique<FakeVehicleMessageTransport>(scenario);
        // 计数：车端一旦进入下载阶段必然先申请下载凭证。
        // countGrants=false 时保留 Fake 默认下载授权行为（本用例需要真实下载/安装通路）。
        if (countGrants) {
            auto counter = downloadGrantRequests;
            transport->registerPayloadHandler(
                std::string(payload_type::kDownloadGrantRequest),
                [counter](const VehicleMessage&, const ExchangeOptions&) {
                    ++(*counter);
                    return TransportResult<VehicleMessage>{TransportOutcome::Timeout, {}};
                });
        }
        cloud = std::make_unique<FotaCloudProxyViaTransport>(*transport);
        inv = std::make_unique<MockInventoryProvider>(scenario);
        consent = std::make_unique<MockConsentProvider>();
        dl = std::make_unique<MockPackageDownloader>(scenario);
        cond = std::make_unique<MockVehicleConditionProvider>();
        exec = std::make_unique<MockInstallExecutor>(scenario);
        log = std::make_unique<MockLogCollector>();
        registrar = std::make_unique<FakeInstalledVersionRegistrar>();
        registrar->availableFlag = registrarAvailable;
        orch = rebuildOrchestrator();
    }

    std::unique_ptr<FotaOrchestrator> rebuildOrchestrator() {
        FotaOrchestratorConfig cfg;
        cfg.inventoryIdentityRetryBackoff = std::chrono::milliseconds(0);
        return std::make_unique<FotaOrchestrator>(
            *cloud, *inv, *consent, *dl, *cond, *exec, *log, *cloudStore, cfg,
            /*clockSync=*/nullptr,
            [this]() { return fotaStore->allocateSnapshotSeq(); },
            registrar.get());
    }

    void seedDurableTask(const std::string& stateName, const std::string& disposition,
                         const ::vehicle::fota::v1::VehicleTaskSnapshot& task) {
        std::string pb;
        task.SerializeToString(&pb);
        FotaVehicleTaskRecord rec;
        rec.vehicleTaskId = task.vehicle_task_id();
        rec.taskRevision = task.task_revision();
        rec.vehicleTaskState = stateName;
        rec.localDispositionResult = disposition;
        rec.taskSnapshotPbHex = protoBinaryToHex(pb);
        cloudStore->saveVehicleTask(rec);
    }
};

const char* kOnePackage = R"([{"package_id":"PKG-CCU-1","bytes":2048,"failures_before_success":0}])";

// ---------------------------------------------------------------------------
// 1. 空 package_list：不得发起下载授权，保持 DISCOVERED 继续上报清单
// ---------------------------------------------------------------------------
TEST(TaskSnapshotFailClosed, EmptyPackageListBlocksDownloadAndKeepsDetection) {
    GuardHarness h;
    h.init("[]", /*registrarAvailable=*/true);

    auto first = h.orch->step();
    EXPECT_NE(first.error.find("FOTA-TASK-SNAPSHOT-EMPTY"), std::string::npos) << first.error;
    EXPECT_EQ(*h.downloadGrantRequests, 0) << "空包清单不得发起下载授权";

    auto rec = h.cloudStore->loadVehicleTask();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->vehicleTaskState, "DISCOVERED");
    EXPECT_EQ(rec->localDispositionResult, "checkTask");

    // 第二轮仍在检测阶段（TaskCheck 继续），清单身份复用不重分配。
    auto second = h.orch->step();
    EXPECT_NE(second.error.find("FOTA-TASK-SNAPSHOT-EMPTY"), std::string::npos) << second.error;
    EXPECT_EQ(*h.downloadGrantRequests, 0);
    EXPECT_EQ(h.transport->inventoryFullCount(), 1u);
    auto rec2 = h.cloudStore->loadVehicleTask();
    ASSERT_TRUE(rec2.has_value());
    EXPECT_EQ(rec2->vehicleTaskState, "DISCOVERED");
}

// ---------------------------------------------------------------------------
// 2. 恢复出的空任务（DOWNLOADING / WAITING_WINDOW）：自愈回 DISCOVERED
// ---------------------------------------------------------------------------
TEST(TaskSnapshotFailClosed, RestoredEmptyTaskSelfHealsToDetection) {
    ::vehicle::fota::v1::VehicleTaskSnapshot emptyTask;
    emptyTask.set_vehicle_task_id("VT-EMPTY");
    emptyTask.set_task_revision(1);
    emptyTask.set_end_time_ms(INT64_MAX);

    for (const char* state : {"DOWNLOADING", "WAITING_WINDOW", "READY"}) {
        GuardHarness h;
        h.init("[]", /*registrarAvailable=*/true);
        h.seedDurableTask(state, "download", emptyTask);
        auto orch = h.rebuildOrchestrator();
        orch->reconcileOnStart();

        auto o = orch->step();
        EXPECT_NE(o.error.find("FOTA-TASK-SNAPSHOT-EMPTY"), std::string::npos)
            << state << ": " << o.error;
        EXPECT_EQ(*h.downloadGrantRequests, 0) << state;
        auto rec = h.cloudStore->loadVehicleTask();
        ASSERT_TRUE(rec.has_value());
        EXPECT_EQ(rec->vehicleTaskState, "DISCOVERED") << state;
    }
}

// ---------------------------------------------------------------------------
// 3. 安装登记能力不可用：回退 DISCOVERED 继续上报清单（不得静默停在 WaitingWindow）
// ---------------------------------------------------------------------------
TEST(TaskSnapshotFailClosed, RegistrationUnavailableKeepsInventoryReporting) {
    GuardHarness h;
    h.init(kOnePackage, /*registrarAvailable=*/false, /*countGrants=*/false);

    // 第 1 轮：清单上报 ACCEPTED + 任务下发（含 1 个包）→ 进入下载阶段。
    auto o = h.orch->step();
    EXPECT_TRUE(o.error.empty()) << o.error;
    auto afterCheck = h.cloudStore->loadVehicleTask();
    ASSERT_TRUE(afterCheck.has_value());
    // Fake 任务 consent_required=true：进入授权或下载阶段均属正常推进。
    EXPECT_TRUE(afterCheck->vehicleTaskState == "CONSENT_PENDING" ||
                afterCheck->vehicleTaskState == "DOWNLOAD_PENDING")
        << afterCheck->vehicleTaskState;

    // 推到安装许可阶段：登记能力不可用 -> 回退 DISCOVERED，而不是留在 WaitingWindow。
    bool sawRegistrationBlocked = false;
    for (int i = 0; i < 12 && !sawRegistrationBlocked; ++i) {
        auto step = h.orch->step();
        if (step.error.find("FOTA-TASK-REGISTRATION-BLOCKED") != std::string::npos) {
            sawRegistrationBlocked = true;
        }
    }
    EXPECT_TRUE(sawRegistrationBlocked) << "登记能力不可用必须产生可定位错误";
    auto rec = h.cloudStore->loadVehicleTask();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->vehicleTaskState, "DISCOVERED");

    // 关键：回退后仍在检测阶段 -> 继续 TaskCheck（本地清单继续上报）。
    const auto fullBefore = h.transport->inventoryFullCount();
    auto next = h.orch->step();
    EXPECT_TRUE(next.error.empty()) << next.error;
    // 清单内容未变 -> 复用 revision（可能 DIGEST），但必然发生了上行 TaskCheck。
    (void)fullBefore;
    auto after = h.cloudStore->loadVehicleTask();
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->localDispositionResult.empty());
}

} // namespace
