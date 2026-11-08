// =============================================================================
// tests/ota/inventory_revision_test.cpp
// 清单版本不膨胀集成测试 (CCU-FOTA-DSN-CR-012 §验收标准)
// -----------------------------------------------------------------------------
// 通过真实装配（FotaOrchestrator + Mock 车内端口 + Fake 云侧端点 + framework
// store）验证：内容不变不新增版本、内容变化只新增 1 个版本、失败重试复用同一
// revision、采集失败不上行、终态后继续检测且不膨胀。
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
                ("fota-inv-rev-" + std::to_string(getpid()) + "-" +
                 std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

std::string scenarioJson(const std::string& faults = "[]") {
    return R"({
      "scenario": "inventory-revision",
      "clock": "virtual",
      "inventory": {"mode": "FULL", "baseline": "BASE-001", "fota_master_version": "1.0.0"},
      "packages": [{"package_id": "PKG-VCU-001", "bytes": 1024, "failures_before_success": 0}],
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
    std::unique_ptr<FotaOrchestrator> orch;

    void init(const std::string& json, bool changeDetection = true) {
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
        exec = std::make_unique<MockInstallExecutor>(scenario);
        log = std::make_unique<MockLogCollector>();

        FotaOrchestratorConfig cfg;
        cfg.inventoryChangeDetectionEnabled = changeDetection;
        // 测试中不等待退避（退避本身由 planner 单测覆盖）。
        cfg.inventoryIdentityRetryBackoff = std::chrono::milliseconds(0);
        orch = std::make_unique<FotaOrchestrator>(
            *cloud, *inv, *consent, *dl, *cond, *exec, *log, *cloudStore, cfg,
            /*clockSync=*/nullptr,
            [this]() { return fotaStore->allocateSnapshotSeq(); });
    }

    std::uint64_t highestSeq() const {
        auto seq = fotaStore->loadSequence();
        return seq.has_value() ? seq->highestAllocated : 0;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 内容不变：连续 10 个检测周期只产生 1 个清单版本
// ---------------------------------------------------------------------------
TEST(InventoryRevision, UnchangedContentDoesNotGrowVersions) {
    Harness h;
    h.init(scenarioJson(R"(["inventory_no_task"])"));

    for (int i = 0; i < 10; ++i) {
        auto o = h.orch->step();
        EXPECT_TRUE(o.error.empty()) << "cycle " << i << " err=" << o.error;
        EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::None);
    }

    EXPECT_EQ(h.transport->inventoryFullCount(), 1u);
    EXPECT_EQ(h.transport->inventoryDigestCount(), 9u);
    EXPECT_EQ(h.transport->inventoryDigestMismatchCount(), 0u);
    EXPECT_EQ(h.transport->inventoryVersionCount(), 1u);
    EXPECT_EQ(h.highestSeq(), 1u);

    auto rec = h.cloudStore->loadInventory();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->status, "ACCEPTED");
    EXPECT_EQ(rec->inventoryRevision, 1u);
    EXPECT_EQ(rec->acceptedInventoryRevision, 1u);
}

// ---------------------------------------------------------------------------
// 2. 单个 ECU 版本变化：恰好新增 1 个版本，随后回到 DIGEST
// ---------------------------------------------------------------------------
TEST(InventoryRevision, ContentChangeAddsExactlyOneVersion) {
    Harness h;
    h.init(scenarioJson(R"(["inventory_no_task"])"));

    ASSERT_TRUE(h.orch->step().error.empty());
    ASSERT_EQ(h.transport->inventoryVersionCount(), 1u);

    h.inv->setSoftwareVersion("2.0.0");
    ASSERT_TRUE(h.orch->step().error.empty());

    EXPECT_EQ(h.transport->inventoryFullCount(), 2u);
    EXPECT_EQ(h.transport->inventoryVersionCount(), 2u);
    EXPECT_EQ(h.highestSeq(), 2u);
    EXPECT_EQ(h.transport->cloudInventoryRevision(), 2u);

    // 变化后继续检测：内容未变 -> DIGEST，不再新增版本。
    ASSERT_TRUE(h.orch->step().error.empty());
    EXPECT_EQ(h.transport->inventoryFullCount(), 2u);
    EXPECT_EQ(h.transport->inventoryDigestCount(), 1u);
    EXPECT_EQ(h.transport->inventoryVersionCount(), 2u);
    EXPECT_EQ(h.highestSeq(), 2u);
}

// ---------------------------------------------------------------------------
// 3. 传输失败：复用同一 revision 重试，不产生新版本
// ---------------------------------------------------------------------------
TEST(InventoryRevision, TransportTimeoutReusesSameRevision) {
    Harness h;
    h.init(scenarioJson(R"(["cloud_timeout"])"));

    auto first = h.orch->step();
    EXPECT_TRUE(first.transportFailed) << "err=" << first.error;
    EXPECT_EQ(h.highestSeq(), 1u);            // 已分配
    EXPECT_EQ(h.transport->inventoryVersionCount(), 0u);  // 云端未收到

    auto second = h.orch->step();
    EXPECT_TRUE(second.error.empty()) << "err=" << second.error;

    EXPECT_EQ(h.highestSeq(), 1u);            // 未新增
    EXPECT_EQ(h.transport->inventoryVersionCount(), 1u);
    EXPECT_EQ(h.transport->inventoryFullCount(), 1u);

    auto rec = h.cloudStore->loadInventory();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->status, "ACCEPTED");
    EXPECT_EQ(rec->inventoryRevision, 1u);
    EXPECT_GE(rec->attempt, 2u);              // 同一身份第二次尝试
}

// ---------------------------------------------------------------------------
// 4. 采集失败/空清单：不上行、不分配 revision
// ---------------------------------------------------------------------------
TEST(InventoryRevision, CollectFailureSkipsUplinkWithoutAllocating) {
    Harness h;
    h.init(scenarioJson(R"(["inventory_no_task"])"));
    h.inv->setAvailable(false);

    auto o = h.orch->step();
    EXPECT_FALSE(o.error.empty());
    EXPECT_EQ(h.transport->inventoryFullCount(), 0u);
    EXPECT_EQ(h.transport->inventoryDigestCount(), 0u);
    EXPECT_EQ(h.highestSeq(), 0u);
    EXPECT_FALSE(h.cloudStore->loadInventory().has_value());
}

// ---------------------------------------------------------------------------
// 5. 终态后回到检测态，继续发现任务且不新增清单版本
// ---------------------------------------------------------------------------
TEST(InventoryRevision, TerminalResumesDetectionWithoutNewVersion) {
    Harness h;
    h.init(scenarioJson());

    StepOutcome last;
    for (int i = 0; i < 50; ++i) {
        last = h.orch->step();
        if (last.terminal) break;
    }
    ASSERT_TRUE(last.terminal);
    ASSERT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::Completed);
    ASSERT_EQ(h.transport->inventoryVersionCount(), 1u);

    // 终态后：下一步回到检测态（不再永久 terminal）。
    auto reset = h.orch->step();
    EXPECT_EQ(reset.action, "resetToDetection");
    EXPECT_FALSE(reset.terminal);
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::None);

    // 再下一步继续任务检测：内容未变化 -> DGEST（不新增版本），并携带上一次终态对账。
    auto check = h.orch->step();
    EXPECT_EQ(check.action, "checkTask") << "err=" << check.error;
    EXPECT_TRUE(check.error.empty()) << "err=" << check.error;
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::ConsentPending);

    auto sent = h.transport->lastTaskCheck();
    EXPECT_EQ(sent.inventory_mode(), ::vehicle::fota::v1::INVENTORY_MODE_DIGEST);
    EXPECT_EQ(sent.local_vehicle_task_id(), "VT-001");
    EXPECT_EQ(sent.local_vehicle_task_status(),
              ::vehicle::fota::v1::VEHICLE_TASK_STATUS_COMPLETED);
    EXPECT_EQ(h.transport->inventoryVersionCount(), 1u);
    EXPECT_EQ(h.highestSeq(), 1u);
    EXPECT_EQ(h.transport->inventoryDigestCount(), 1u);
}

// ---------------------------------------------------------------------------
// 6. 云端无任务不是错误（正常检测结果）
// ---------------------------------------------------------------------------
TEST(InventoryRevision, NoTaskIsNotError) {
    Harness h;
    h.init(scenarioJson(R"(["inventory_no_task"])"));

    auto o = h.orch->step();
    EXPECT_TRUE(o.error.empty());
    EXPECT_EQ(o.action, "checkTaskIdle");
    EXPECT_EQ(h.orch->vehicleTaskState(), VehicleTaskState::None);
}
