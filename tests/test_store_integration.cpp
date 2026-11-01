// =============================================================================
// tests/test_store_integration.cpp
// CCU-FOTA 状态持久化集成测试 (CCU-FOTA-DSN-CR-005)
// 覆盖：崩溃恢复（各检查点）、对账、序号继续、安全。
// 说明：InventoryReporter 驱动的“上报周期写 store”集成用例随死代码清理移除
// （InventoryReporter 已删除），此处仅保留直接基于 store/StateRecovery 的用例。
// =============================================================================

#include "ccu/fota/store/fota_state_store.hpp"
#include "ccu/fota/store/fota_state_recovery.hpp"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <filesystem>

namespace fs = std::filesystem;
using namespace ccu_fota;
using namespace ccu_fota::store;

namespace {
fs::path makeUniqueRoot() {
    static int counter = 0;
    auto root = fs::temp_directory_path() /
                ("fota-store-int-" + std::to_string(getpid()) +
                 "-" + std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

VehicleSoftwareSnapshot makeSnapshot(std::uint64_t seq) {
    VehicleSoftwareSnapshot s;
    s.vin = "LSJAAAAAAAAAAAAAA";
    s.baseline_source = BaselineSource::FACTORY;
    s.registry_version = "1.0.0";
    s.collected_at = "2026-08-07T10:00:00Z";
    s.overall_result = CollectionStatus::ALL_OK;
    s.snapshot_seq = seq;
    return s;
}
} // namespace

// ===========================================================================
// 崩溃恢复：SubmitPrepared 阶段崩溃 -> 重启恢复计划判定为 Resubmit
// ===========================================================================
TEST(StoreIntegrationTest, CrashAtSubmitPreparedRecoversAsResubmit) {
    auto root = makeUniqueRoot();
    // 模拟崩溃：保存 SubmitPrepared 检查点（seq=1）
    {
        auto store = std::make_shared<FotaStateStore>(
            FotaStateStore::open(root, 100, 3600000));
        store->allocateSnapshotSeq();  // seq=1
        ActiveJobState job;
        job.reportId = "1";
        job.snapshotSeq = 1;
        job.phase = JobPhase::SubmitPrepared;
        job.idempotencyKey = "idem-1";
        job.reason = TriggerReason::AutoStart;
        store->saveActiveJob(job);
    }

    // 重启：新 store + 恢复
    auto store = std::make_shared<FotaStateStore>(
        FotaStateStore::open(root, 100, 3600000));
    auto plan = StateRecovery::recover(*store);
    EXPECT_EQ(plan.action, RecoveryAction::Resubmit);
    ASSERT_TRUE(plan.job.has_value());
    EXPECT_EQ(plan.job->idempotencyKey, "idem-1");

    fs::remove_all(root);
}

// ===========================================================================
// 崩溃恢复：CompletedPendingCleanup 阶段崩溃 -> 重启对账清理
// ===========================================================================
TEST(StoreIntegrationTest, CrashAtCompletedPendingCleanupReconciles) {
    auto root = makeUniqueRoot();
    {
        auto store = std::make_shared<FotaStateStore>(
            FotaStateStore::open(root, 100, 3600000));
        store->allocateSnapshotSeq();  // seq=1
        ActiveJobState job;
        job.reportId = "1";
        job.snapshotSeq = 1;
        job.phase = JobPhase::CompletedPendingCleanup;
        job.idempotencyKey = "idem-1";
        store->saveActiveJob(job);
        // last_success 已保存（匹配）
        LastSuccessState ls;
        ls.reportId = "1";
        ls.snapshotSeq = 1;
        ls.completedAt = 1;
        ls.registryVersion = "1.0.0";
        ls.overallResult = CollectionStatus::ALL_OK;
        ls.snapshot = makeSnapshot(1);
        store->saveLastSuccess(ls);
    }

    auto store = std::make_shared<FotaStateStore>(
        FotaStateStore::open(root, 100, 3600000));
    auto plan = StateRecovery::recover(*store);
    EXPECT_EQ(plan.action, RecoveryAction::None);
    EXPECT_FALSE(plan.job.has_value());  // 已清理

    // ActiveJob 已删除
    EXPECT_FALSE(store->loadActiveJob().has_value());
    // Dedupe 已补齐
    DedupeState d = store->loadDedupe();
    ASSERT_EQ(d.entries.size(), 1u);
    EXPECT_EQ(d.entries[0].snapshotSeq, 1u);

    fs::remove_all(root);
}

// ===========================================================================
// 崩溃恢复：恢复后序号不复用（继续递增）
// ===========================================================================
TEST(StoreIntegrationTest, SequenceContinuesAfterRecovery) {
    auto root = makeUniqueRoot();
    {
        auto store = std::make_shared<FotaStateStore>(
            FotaStateStore::open(root, 100, 3600000));
        store->allocateSnapshotSeq();  // seq=1
        store->allocateSnapshotSeq();  // seq=2
    }
    auto store = std::make_shared<FotaStateStore>(
        FotaStateStore::open(root, 100, 3600000));
    // 重启后序号从 3 继续，不复用 1/2
    EXPECT_EQ(store->allocateSnapshotSeq(), 3u);
    fs::remove_all(root);
}

// ===========================================================================
// 安全：store 错误日志字段不含 VIN/snapshot/device_sn
// ===========================================================================
TEST(StoreIntegrationTest, StoreFilesDoNotLeakVinInPlaintextPath) {
    auto root = makeUniqueRoot();
    auto store = std::make_shared<FotaStateStore>(
        FotaStateStore::open(root, 100, 3600000));

    // 保存含 VIN 的 LastSuccess
    LastSuccessState ls;
    ls.reportId = "1";
    ls.snapshotSeq = 1;
    ls.completedAt = 1;
    ls.registryVersion = "1.0.0";
    ls.snapshot = makeSnapshot(1);
    store->saveLastSuccess(ls);

    // 文件名不含 VIN（仅 key.dat）
    for (const auto& entry : fs::directory_iterator(root / "fota")) {
        std::string name = entry.path().filename().string();
        EXPECT_EQ(name.find("LSJ"), std::string::npos)
            << "VIN leaked in filename: " << name;
    }
    fs::remove_all(root);
}
