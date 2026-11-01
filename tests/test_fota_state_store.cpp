// =============================================================================
// tests/test_fota_state_store.cpp
// CCU-FOTA 状态存储封装单元测试 (CCU-FOTA-DSN-CR-005)
// 覆盖：首次启动、序号分配与 durability、溢出、损坏处置（各 key）、
//       迁移、权限（0700/0600）、序号间隙不复用。
// =============================================================================

#include "ccu/fota/store/fota_state_store.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace ccu_fota;
using namespace ccu_fota::store;

namespace {
// 每个测试用独立临时根目录。
fs::path makeUniqueRoot() {
    static int counter = 0;
    auto root = fs::temp_directory_path() /
                ("fota-store-test-" + std::to_string(getpid()) +
                 "-" + std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

// 直接损坏某 key 的持久化文件（写入随机字节）。
void corruptKeyFile(const fs::path& root, const std::string& key) {
    fs::path file = root / "fota" / (key + ".dat");
    ASSERT_TRUE(fs::exists(file)) << "missing file: " << file;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    std::mt19937 rng(42);
    for (int i = 0; i < 64; ++i) {
        out.put(static_cast<char>(rng() & 0xFF));
    }
}

// 截断某 key 文件（部分写入）。
void truncateKeyFile(const fs::path& root, const std::string& key, std::size_t len) {
    fs::path file = root / "fota" / (key + ".dat");
    ASSERT_TRUE(fs::exists(file));
    std::string content((std::istreambuf_iterator<char>(std::ifstream(file).rdbuf())),
                         std::istreambuf_iterator<char>());
    if (len > content.size()) len = content.size();
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(len));
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
// 首次启动
// ===========================================================================
TEST(FotaStateStoreTest, OpenOnEmptyDirIsFirstBoot) {
    auto root = makeUniqueRoot();
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_FALSE(store.isSequenceBlocked());
    EXPECT_TRUE(store.isLastSuccessAvailable());
    EXPECT_TRUE(store.isDedupeAvailable());
    EXPECT_FALSE(store.isActiveJobCorrupted());
    EXPECT_FALSE(store.loadLastSuccess().has_value());
    EXPECT_FALSE(store.loadActiveJob().has_value());
    fs::remove_all(root);
}

// ===========================================================================
// 序号分配
// ===========================================================================
TEST(FotaStateStoreTest, SequenceFirstBootReturnsOne) {
    auto root = makeUniqueRoot();
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_EQ(store.allocateSnapshotSeq(), 1u);
    fs::remove_all(root);
}

TEST(FotaStateStoreTest, SequenceIncrements) {
    auto root = makeUniqueRoot();
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_EQ(store.allocateSnapshotSeq(), 1u);
    EXPECT_EQ(store.allocateSnapshotSeq(), 2u);
    EXPECT_EQ(store.allocateSnapshotSeq(), 3u);
    fs::remove_all(root);
}

TEST(FotaStateStoreTest, SequenceDurableAcrossReopen) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
        store.allocateSnapshotSeq();
        store.allocateSnapshotSeq();
    }
    // 重启后序号继续递增，不复用
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_EQ(store.allocateSnapshotSeq(), 4u);
    fs::remove_all(root);
}

TEST(FotaStateStoreTest, SequenceGapAllowedNoReuse) {
    // 崩溃发生在 durable save 之后、业务使用之前：序号已分配但未使用 -> 间隙允许。
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();  // 分配 1，未使用即“崩溃”
        store.allocateSnapshotSeq();  // 分配 2，未使用即“崩溃”
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    // 重启后从 3 开始，不复用 1/2（间隙允许，不回退/不复用）
    EXPECT_EQ(store.allocateSnapshotSeq(), 3u);
    fs::remove_all(root);
}

// ===========================================================================
// 序号损坏处置
// ===========================================================================
TEST(FotaStateStoreTest, SequenceCorruptionBlocksAllocation) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
    }
    // 损坏 sequence 文件
    corruptKeyFile(root, keys::SEQUENCE);
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_TRUE(store.isSequenceBlocked());
    EXPECT_THROW(store.allocateSnapshotSeq(), SeqAllocBlocked);
    fs::remove_all(root);
}

TEST(FotaStateStoreTest, SequenceTruncationBlocks) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
    }
    truncateKeyFile(root, keys::SEQUENCE, 5);  // 截断到 5 字节（< header 28）
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_TRUE(store.isSequenceBlocked());
    fs::remove_all(root);
}

// ===========================================================================
// 最近成功快照
// ===========================================================================
TEST(FotaStateStoreTest, LastSuccessRoundTrip) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        LastSuccessState s;
        s.reportId = "rpt-1";
        s.snapshotSeq = 5;
        s.completedAt = 1700000000000;
        s.registryVersion = "1.0.0";
        s.overallResult = CollectionStatus::ALL_OK;
        s.snapshot = makeSnapshot(5);
        store.saveLastSuccess(s);
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto loaded = store.loadLastSuccess();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->reportId, "rpt-1");
    EXPECT_EQ(loaded->snapshotSeq, 5u);
    EXPECT_EQ(loaded->snapshot.vin, "LSJAAAAAAAAAAAAAA");
    fs::remove_all(root);
}

TEST(FotaStateStoreTest, LastSuccessCorruptionDisablesDedup) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        LastSuccessState s;
        s.reportId = "rpt-1";
        s.snapshotSeq = 5;
        s.completedAt = 1;
        s.registryVersion = "rv";
        s.snapshot = makeSnapshot(5);
        store.saveLastSuccess(s);
    }
    corruptKeyFile(root, keys::LAST_SUCCESS);
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_FALSE(store.isLastSuccessAvailable());
    EXPECT_FALSE(store.loadLastSuccess().has_value());
    fs::remove_all(root);
}

// ===========================================================================
// 去重窗口
// ===========================================================================
TEST(FotaStateStoreTest, DedupeRoundTrip) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        DedupeState d = store.loadDedupe();  // 首次为空
        EXPECT_TRUE(d.entries.empty());
        DedupeEntry e;
        e.requestId = "req-1";
        e.reportId = "rpt-1";
        e.snapshotSeq = 1;
        e.overallResult = "ALL_OK";
        e.completedAt = 100;
        e.expiresAt = 200;
        d.entries.push_back(e);
        store.saveDedupe(d);
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    DedupeState d = store.loadDedupe();
    ASSERT_EQ(d.entries.size(), 1u);
    EXPECT_EQ(d.entries[0].requestId, "req-1");
    EXPECT_EQ(d.maxEntries, 100u);
    fs::remove_all(root);
}

TEST(FotaStateStoreTest, DedupeCorruptionReturnsEmpty) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        DedupeState d;
        DedupeEntry e;
        e.requestId = "req-1";
        e.reportId = "rpt-1";
        d.entries.push_back(e);
        store.saveDedupe(d);
    }
    corruptKeyFile(root, keys::DEDUPE);
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_FALSE(store.isDedupeAvailable());
    DedupeState d = store.loadDedupe();
    EXPECT_TRUE(d.entries.empty());  // 损坏 -> 空逻辑窗口
    fs::remove_all(root);
}

// ===========================================================================
// 在途/重试检查点
// ===========================================================================
TEST(FotaStateStoreTest, ActiveJobRoundTrip) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        ActiveJobState j;
        j.requestId = "req-1";
        j.reportId = "rpt-1";
        j.snapshotSeq = 7;
        j.reason = TriggerReason::CloudRequest;
        j.phase = JobPhase::SubmitPrepared;
        j.attempt = 1;
        j.idempotencyKey = "idem";
        store.saveActiveJob(j);
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    auto loaded = store.loadActiveJob();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->reportId, "rpt-1");
    EXPECT_EQ(loaded->phase, JobPhase::SubmitPrepared);
    EXPECT_EQ(loaded->idempotencyKey, "idem");
    store.removeActiveJob();
    EXPECT_FALSE(store.loadActiveJob().has_value());
    fs::remove_all(root);
}

TEST(FotaStateStoreTest, ActiveJobCorruptionDoesNotRestoreAsSuccess) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        ActiveJobState j;
        j.reportId = "rpt-1";
        j.snapshotSeq = 7;
        j.phase = JobPhase::CompletedPendingCleanup;  // 即使是待清理阶段
        store.saveActiveJob(j);
    }
    corruptKeyFile(root, keys::ACTIVE_JOB);
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_TRUE(store.isActiveJobCorrupted());
    EXPECT_FALSE(store.loadActiveJob().has_value());  // 不得恢复为成功
    fs::remove_all(root);
}

// ===========================================================================
// 权限
// ===========================================================================
TEST(FotaStateStoreTest, DirectoryAndFilePermissions) {
    auto root = makeUniqueRoot();
    {
        auto store = FotaStateStore::open(root, 100, 3600000);
        store.allocateSnapshotSeq();
    }
    fs::path svcDir = root / "fota";
    ASSERT_TRUE(fs::exists(svcDir));
    auto perms = fs::status(svcDir).permissions();
    // 目录 0700
    EXPECT_TRUE((perms & fs::perms::owner_all) == fs::perms::owner_all);
    EXPECT_FALSE((perms & fs::perms::group_all) != fs::perms::none && true);
    fs::path seqFile = svcDir / "inventory.sequence.dat";
    ASSERT_TRUE(fs::exists(seqFile));
    auto fperms = fs::status(seqFile).permissions();
    // 文件 0600
    EXPECT_TRUE((fperms & fs::perms::owner_read) != fs::perms::none);
    EXPECT_TRUE((fperms & fs::perms::owner_write) != fs::perms::none);
    EXPECT_TRUE((fperms & fs::perms::owner_exec) == fs::perms::none);
    fs::remove_all(root);
}

// ===========================================================================
// 不可写根目录 fail-closed
// ===========================================================================
TEST(FotaStateStoreTest, UnwritableRootFailsClosed) {
    // 使用一个不存在的、无法创建的路径（父目录无权限）
    fs::path badRoot = fs::temp_directory_path() / "fota-store-noperm-root";
    fs::remove_all(badRoot);
    fs::create_directories(badRoot);
    fs::permissions(badRoot, fs::perms::none);
    bool threw = false;
    try {
        FotaStateStore::open(badRoot / "sub", 100, 3600000);
    } catch (const ccu::fw::store::StoreException&) {
        threw = true;
    } catch (const std::exception&) {
        threw = true;
    }
    fs::permissions(badRoot, fs::perms::owner_all);
    fs::remove_all(badRoot);
    EXPECT_TRUE(threw);
}

// ===========================================================================
// 迁移：写入 v0 envelope，重开时迁移到 v1
// ===========================================================================
TEST(FotaStateStoreTest, MigratesOldSequenceVersionOnOpen) {
    auto root = makeUniqueRoot();
    {
        // 用底层框架 store 直接写入 v0 sequence
        ccu::fw::store::StoreOptions opts;
        opts.root = root;
        opts.flushMode = ccu::fw::store::FlushMode::Synchronous;
        auto inner = ccu::fw::store::Store::open("fota", opts);
        // v0 payload: 无 schemaVersion / updatedAt
        std::string v0Env = encodeEnvelope(0, R"({"highestAllocated":7})");
        inner.save<std::string>(keys::SEQUENCE, v0Env);
    }
    // 重开：应迁移到 v1 并验证
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_FALSE(store.isSequenceBlocked());
    // 迁移后继续从 7 递增
    EXPECT_EQ(store.allocateSnapshotSeq(), 8u);
    // 迁移备份应存在
    fs::path backup = root / "fota" / "migration.backup.inventory.sequence.v0.dat";
    EXPECT_TRUE(fs::exists(backup));
    fs::remove_all(root);
}

TEST(FotaStateStoreTest, MigratesOldLastSuccessVersionOnOpen) {
    auto root = makeUniqueRoot();
    {
        ccu::fw::store::StoreOptions opts;
        opts.root = root;
        opts.flushMode = ccu::fw::store::FlushMode::Synchronous;
        auto inner = ccu::fw::store::Store::open("fota", opts);
        nlohmann::json snap = {{"vin", "V"}, {"baseline_source", "UNKNOWN"},
                               {"registry_version", "rv"}, {"collected_at", "t"},
                               {"overall_result", "ALL_OK"}, {"snapshot_seq", 3},
                               {"ecu_list", nlohmann::json::array()}};
        nlohmann::json v0 = {{"reportId", "r"}, {"snapshotSeq", 3}, {"completedAt", 1},
                             {"registryVersion", "rv"}, {"overallResult", "ALL_OK"},
                             {"snapshot", snap}};
        inner.save<std::string>(keys::LAST_SUCCESS, encodeEnvelope(0, v0.dump()));
    }
    auto store = FotaStateStore::open(root, 100, 3600000);
    EXPECT_TRUE(store.isLastSuccessAvailable());
    auto loaded = store.loadLastSuccess();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->snapshotSeq, 3u);
    EXPECT_TRUE(loaded->fingerprints.algorithm.empty());  // 迁移补齐为未知
    fs::remove_all(root);
}
