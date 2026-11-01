// =============================================================================
// tests/test_fota_state_recovery.cpp
// CCU-FOTA 状态恢复器单元测试 (CCU-FOTA-DSN-CR-005)
// 覆盖恢复矩阵：无 job / 各 phase / CompletedPendingCleanup 对账 /
//               active_job 损坏 / sequence 阻断与回退。
// =============================================================================

#include "ccu/fota/store/fota_state_recovery.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace ccu_fota;
using namespace ccu_fota::store;

namespace {
fs::path makeUniqueRoot() {
    static int counter = 0;
    auto root = fs::temp_directory_path() /
                ("fota-recovery-test-" + std::to_string(getpid()) +
                 "-" + std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void corruptKeyFile(const fs::path& root, const std::string& key) {
    fs::path file = root / "fota" / (key + ".dat");
    ASSERT_TRUE(fs::exists(file));
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (int i = 0; i < 64; ++i) out.put(static_cast<char>(i * 7));
}

ActiveJobState makeJob(JobPhase phase, std::uint64_t seq = 0) {
    ActiveJobState j;
    j.requestId = "req-1";
    j.reportId = "rpt-1";
    j.snapshotSeq = seq;
    j.reason = TriggerReason::AutoStart;
    j.phase = phase;
    j.attempt = 0;
    j.idempotencyKey = (seq > 0) ? "idem-1" : "";
    return j;
}

LastSuccessState makeMatchingLastSuccess(const ActiveJobState& job) {
    LastSuccessState ls;
    ls.reportId = job.reportId;
    ls.snapshotSeq = job.snapshotSeq;
    ls.completedAt = 1700000000000;
    ls.registryVersion = "1.0.0";
    ls.overallResult = CollectionStatus::ALL_OK;
    ls.snapshot.snapshot_seq = job.snapshotSeq;
    ls.snapshot.vin = "LSJAAAAAAAAAAAAAA";
    ls.snapshot.registry_version = "1.0.0";
    return ls;
}
} // namespace

// ===========================================================================
// 无在途任务
// ===========================================================================
TEST(FotaStateRecoveryTest, NoActiveJobReturnsNone) {
    auto root = makeUniqueRoot();
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::None);
    fs::remove_all(root);
}

// ===========================================================================
// Accepted / Collecting -> ReCollect
// ===========================================================================
TEST(FotaStateRecoveryTest, AcceptedPhaseReCollects) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.saveActiveJob(makeJob(JobPhase::Accepted));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::ReCollect);
    ASSERT_TRUE(plan.job.has_value());
    EXPECT_EQ(plan.job->reportId, "rpt-1");
    fs::remove_all(root);
}

TEST(FotaStateRecoveryTest, CollectingPhaseReCollects) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.saveActiveJob(makeJob(JobPhase::Collecting));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::ReCollect);
    fs::remove_all(root);
}

// ===========================================================================
// Assembled / SubmitPrepared -> Resubmit
// ===========================================================================
TEST(FotaStateRecoveryTest, AssembledPhaseResubmits) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();  // seq=1
        store.saveActiveJob(makeJob(JobPhase::Assembled, 1));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::Resubmit);
    ASSERT_TRUE(plan.job.has_value());
    EXPECT_EQ(plan.job->snapshotSeq, 1u);
    EXPECT_EQ(plan.job->idempotencyKey, "idem-1");
    fs::remove_all(root);
}

TEST(FotaStateRecoveryTest, SubmitPreparedPhaseResubmits) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
        store.saveActiveJob(makeJob(JobPhase::SubmitPrepared, 1));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::Resubmit);
    fs::remove_all(root);
}

TEST(FotaStateRecoveryTest, AssembledWithoutSeqReCollects) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        // Assembled 但序号未分配（不一致）
        store.saveActiveJob(makeJob(JobPhase::Assembled, 0));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::ReCollect);
    fs::remove_all(root);
}

// ===========================================================================
// SubmitUnknown / RetryWaiting -> Retry
// ===========================================================================
TEST(FotaStateRecoveryTest, SubmitUnknownPhaseRetries) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
        store.saveActiveJob(makeJob(JobPhase::SubmitUnknown, 1));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::Retry);
    ASSERT_TRUE(plan.job.has_value());
    EXPECT_EQ(plan.job->snapshotSeq, 1u);
    fs::remove_all(root);
}

TEST(FotaStateRecoveryTest, RetryWaitingPhaseRetries) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
        store.saveActiveJob(makeJob(JobPhase::RetryWaiting, 1));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::Retry);
    fs::remove_all(root);
}

// ===========================================================================
// CompletedPendingCleanup 对账
// ===========================================================================
TEST(FotaStateRecoveryTest, CompletedMatchingLastSuccessCleansUp) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();  // seq=1
        ActiveJobState job = makeJob(JobPhase::CompletedPendingCleanup, 1);
        store.saveActiveJob(job);
        store.saveLastSuccess(makeMatchingLastSuccess(job));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::None);
    EXPECT_FALSE(plan.job.has_value());  // 已清理
    // active_job 应已删除
    EXPECT_FALSE(store.loadActiveJob().has_value());
    // dedupe 应已补齐
    DedupeState d = store.loadDedupe();
    ASSERT_EQ(d.entries.size(), 1u);
    EXPECT_EQ(d.entries[0].snapshotSeq, 1u);
    fs::remove_all(root);
}

TEST(FotaStateRecoveryTest, CompletedWithoutLastSuccessReCollects) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
        store.saveActiveJob(makeJob(JobPhase::CompletedPendingCleanup, 1));
        // 未保存 last_success
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::ReCollect);
    fs::remove_all(root);
}

TEST(FotaStateRecoveryTest, CompletedWithMismatchedLastSuccessReCollects) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
        store.allocateSnapshotSeq();  // seq=2
        ActiveJobState job = makeJob(JobPhase::CompletedPendingCleanup, 2);
        store.saveActiveJob(job);
        // last_success 对应不同的 reportId
        LastSuccessState ls = makeMatchingLastSuccess(job);
        ls.reportId = "different-rpt";
        ls.snapshot.snapshot_seq = 2;
        store.saveLastSuccess(ls);
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::ReCollect);
    fs::remove_all(root);
}

// ===========================================================================
// active_job 损坏 -> ReCollectFresh
// ===========================================================================
TEST(FotaStateRecoveryTest, CorruptedActiveJobReCollectsFresh) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
        store.saveActiveJob(makeJob(JobPhase::SubmitPrepared, 1));
    }
    corruptKeyFile(root, keys::ACTIVE_JOB);
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_TRUE(store.isActiveJobCorrupted());
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::ReCollectFresh);
    EXPECT_FALSE(plan.job.has_value());  // 不恢复原任务
    fs::remove_all(root);
}

// ===========================================================================
// sequence 阻断 -> Blocked
// ===========================================================================
TEST(FotaStateRecoveryTest, SequenceBlockedReturnsBlocked) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
    }
    corruptKeyFile(root, keys::SEQUENCE);
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_TRUE(store.isSequenceBlocked());
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::Blocked);
    fs::remove_all(root);
}

// ===========================================================================
// sequence 回退（last_success.seq > sequence.highestAllocated）-> Blocked
// ===========================================================================
TEST(FotaStateRecoveryTest, SequenceRollbackDetected) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();  // seq=1
        store.allocateSnapshotSeq();  // seq=2
        // 保存 last_success.seq=2
        ActiveJobState job = makeJob(JobPhase::CompletedPendingCleanup, 2);
        store.saveLastSuccess(makeMatchingLastSuccess(job));
    }
    // 篡改 sequence 为旧值（highestAllocated=1 < last_success.seq=2）
    {
        ccu::fw::store::StoreOptions opts;
        opts.root = root;
        opts.flushMode = ccu::fw::store::FlushMode::Synchronous;
        auto inner = ccu::fw::store::Store::open("fota", opts);
        SequenceState s;
        s.highestAllocated = 1;  // 回退
        inner.save<std::string>(keys::SEQUENCE, encodeSequence(s));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto plan = StateRecovery::recover(store);
    EXPECT_EQ(plan.action, RecoveryAction::Blocked);
    fs::remove_all(root);
}
