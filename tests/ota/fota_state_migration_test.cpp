// =============================================================================
// tests/ota/fota_state_migration_test.cpp
// CCU-FOTA 旧 ota.* -> fota.* 状态迁移测试 (CCU-FOTA-DSN-CR-011 §Store 迁移流程 / 测试矩阵 4)
// 覆盖：完整迁移、字段保留（revision/offset/sequence/摘要/时间）、重复迁移幂等、
// 断电恢复（marker 中断重试）、未知新格式/损坏 fail-closed、完成后遗留清理。
// =============================================================================

#include "ccu/fota/store/fota_cloud_state.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"
#include "ccu/fota/store/fota_state_migration.hpp"
#include "ccu/fota/store/fota_state_serializer.hpp"
#include "ccu/fota/store/fota_state_store.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace ccu_fota::store;
using namespace ccu_fota::store::fota;
using ccu::fw::store::Store;

namespace {

fs::path makeUniqueRoot() {
    static int counter = 0;
    auto root = fs::temp_directory_path() /
                ("fota-migration-test-" + std::to_string(getpid()) +
                 "-" + std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

// 写一条旧格式记录（FTSE envelope + JSON payload，schemaVersion=1）。
void saveLegacy(Store& store, const std::string& key, const json& j) {
    store.save<std::string>(key, encodeEnvelope(1, j.dump()));
}

// 打开（无 fota 迁移触发，测试显式调用 migrateOtaToFota）。
Store openStore(const fs::path& root) {
    ccu::fw::store::StoreOptions opts;
    opts.root = root;
    opts.flushMode = ccu::fw::store::FlushMode::Synchronous;
    return Store::open("fota", opts);
}

// 写一组完整的旧 ota.* 状态（模拟 CR-009 时代的持久化状态）。
void writeFullLegacyState(Store& store) {
    saveLegacy(store, "ota.vehicle_task", json{
        {"schemaVersion", 1},
        {"vehicleTaskId", "VT-001"},
        {"taskRevision", "7"},
        {"targetBaselineId", "BASE-001"},
        {"vehicleTaskState", "DOWNLOADING"},
        {"frozenAtMs", 1000},
        {"frozenSnapshotPbHex", "0a035654303031"},
        {"localDispositionResult", "download"},
        {"superseded", false}});

    saveLegacy(store, "ota.inventory", json{
        {"schemaVersion", 1},
        {"mode", "FULL"},
        {"inventoryRevision", "3"},
        {"algorithm", "sha-256"},
        {"ecuListDigest", "abc123"},
        {"collectedAtMs", 5000},
        {"inventoryPbHex", "0a01"},
        {"fullRequired", true}});

    saveLegacy(store, "ota.consent", json{
        {"schemaVersion", 1},
        {"vehicleTaskId", "VT-001"},
        {"effectiveStatus", "ACCEPTED"},
        {"receiptId", "RCP-001"},
        {"receiptExpiresAtMs", 99999},
        {"consentReceiptPbHex", "0a01"},
        {"termsId", "T-1"},
        {"termsVersion", "v1"}});

    json dlEntries = json::array();
    dlEntries.push_back(json{
        {"packageId", "PKG-VCU-001"},
        {"packageRevision", "prev-1"},
        {"etag", "etag-1"},
        {"offset", 1024},
        {"credentialExpiresAtMs", 0},
        {"verifyStatus", "SUCCEEDED"},
        {"stageResultId", "SR-001"},
        {"stageResultDigest", "digest-1"},
        {"ready", true}});
    saveLegacy(store, "ota.downloads", json{
        {"schemaVersion", 1},
        {"entries", dlEntries},
        {"packageManifestDigestHex", "deadbeef"},
        {"packageManifestAlgorithm", "sha-256"},
        {"allReady", true}});

    saveLegacy(store, "ota.execution", json{
        {"schemaVersion", 1},
        {"vehicleTaskId", "VT-001"},
        {"executionId", "EX-001"},
        {"attemptNo", 1},
        {"permitId", "PMT-001"},
        {"permitToken", "tok"},
        {"controlRevision", "9"},
        {"validUntilMs", 8000},
        {"executionState", "INSTALL"},
        {"stage", "INSTALL"},
        {"progressPercent", 50},
        {"checkpointPbHex", "0a01"},
        {"acceptedSequenceNo", 2},
        {"nextSequenceNo", 4},
        {"finalSequenceNo", 0},
        {"resultStatus", ""},
        {"offlinePolicyPbHex", "0a01"},
        {"timeoutPolicyPbHex", "0a01"}});

    json evtEntries = json::array();
    evtEntries.push_back(json{
        {"sequenceNo", 2},
        {"eventId", "EVT-002"},
        {"eventDigest", "d2"},
        {"stage", "INSTALL"},
        {"progressPercent", 50},
        {"result", "SUCCEEDED"},
        {"timestampMs", 2000},
        {"payloadSummary", "50%"},
        {"sendStatus", "ACKED"},
        {"eventPbHex", "0a02"}});
    saveLegacy(store, "ota.event_outbox", json{
        {"schemaVersion", 1},
        {"nextSequenceNo", 4},
        {"acceptedSequenceNo", 2},
        {"entries", evtEntries}});

    json ctlEntries = json::array();
    ctlEntries.push_back(json{
        {"controlRevision", "9"},
        {"commandType", "PAUSE"},
        {"applyMode", "AT_SAFE_POINT"},
        {"expiresAtMs", 9000},
        {"reason", "user"},
        {"ackStatus", "APPLIED"},
        {"ackSequenceNo", 1},
        {"appliedAtMs", 9500}});
    saveLegacy(store, "ota.controls", json{
        {"schemaVersion", 1},
        {"lastAppliedRevision", "9"},
        {"entries", ctlEntries}});

    saveLegacy(store, "ota.policy", json{
        {"schemaVersion", 1},
        {"basePreferenceVersion", "pref-0"},
        {"preferenceVersion", "pref-1"},
        {"effectivePolicyPbHex", "0a01"},
        {"conflict", false}});

    json logEntries = json::array();
    logEntries.push_back(json{
        {"logRequestId", "LOG-001"},
        {"objectKey", "obj-1"},
        {"digestHex", "abc"},
        {"sizeBytes", 4096},
        {"status", "UPLOADED"},
        {"completedAtMs", 7777}});
    saveLegacy(store, "ota.log_jobs", json{
        {"schemaVersion", 1},
        {"entries", logEntries}});
}

class MigrationTest : public ::testing::Test {
protected:
    fs::path root = makeUniqueRoot();
    Store store;

    MigrationTest() : store(openStore(root)) {}
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 无旧 key：迁移 no-op
// ---------------------------------------------------------------------------
TEST_F(MigrationTest, NoLegacyKeysNoop) {
    auto r = migrateOtaToFota(store);
    EXPECT_FALSE(r.ran);
    EXPECT_TRUE(r.completed);
}

// ---------------------------------------------------------------------------
// 2. 完整迁移：旧 key 全部迁移并清理，marker 完成
// ---------------------------------------------------------------------------
TEST_F(MigrationTest, FullMigrationMigratesAllKeys) {
    writeFullLegacyState(store);
    auto r = migrateOtaToFota(store);
    EXPECT_TRUE(r.ran);
    EXPECT_TRUE(r.completed);

    // 新 key 可读且字段保留
    auto vt = fota::decodeVehicleTask(store.load<std::string>("fota.vehicle_task"));
    EXPECT_EQ(vt.vehicleTaskId, "VT-001");
    EXPECT_EQ(vt.taskRevision, 7u);            // 字符串 "7" -> uint64
    EXPECT_EQ(vt.targetBaselineCode, "BASE-001");
    EXPECT_EQ(vt.vehicleTaskState, "DOWNLOADING");

    auto inv = fota::decodeInventory(store.load<std::string>("fota.inventory"));
    EXPECT_EQ(inv.inventoryRevision, 3u);
    EXPECT_EQ(inv.ecuListDigestHex, "abc123");
    EXPECT_TRUE(inv.fullRequired);

    auto cons = fota::decodeConsent(store.load<std::string>("fota.consent"));
    EXPECT_EQ(cons.receiptId, "RCP-001");
    EXPECT_EQ(cons.termsVersion, "v1");

    auto dl = fota::decodeDownload(store.load<std::string>("fota.downloads:PKG-VCU-001"));
    EXPECT_EQ(dl.currentOffsetBytes, 1024u);   // offset 保留
    EXPECT_TRUE(dl.ready);
    EXPECT_EQ(dl.etag, "etag-1");
    EXPECT_FALSE(store.has("fota.downloads:PKG-OTHER"));

    auto ex = fota::decodeExecution(store.load<std::string>("fota.execution"));
    EXPECT_EQ(ex.executionId, "EX-001");
    EXPECT_EQ(ex.controlRevision, 9u);         // 字符串 "9" -> uint64
    EXPECT_EQ(ex.executionState, "INSTALL");

    auto meta = fota::decodeEventOutboxMeta(store.load<std::string>("fota.event_outbox_meta"));
    EXPECT_EQ(meta.nextSequenceNo, 4u);        // 水位保留
    EXPECT_EQ(meta.acceptedSequenceNo, 2u);
    auto evt = fota::decodeEventOutbox(store.load<std::string>("fota.event_outbox:2"));
    EXPECT_EQ(evt.eventId, "EVT-002");
    EXPECT_EQ(evt.sendStatus, "ACKED");

    auto ctl = fota::decodeControl(store.load<std::string>("fota.controls:9"));
    EXPECT_EQ(ctl.controlRevision, 9u);
    EXPECT_EQ(ctl.action, "PAUSE");
    EXPECT_EQ(ctl.applyMode, "AT_SAFE_POINT");

    auto pol = fota::decodePolicy(store.load<std::string>("fota.policy"));
    EXPECT_EQ(pol.basePreferenceVersion, "pref-0");
    EXPECT_EQ(pol.preferenceVersion, "pref-1");

    auto lj = fota::decodeLogJob(store.load<std::string>("fota.log_jobs:LOG-001"));
    EXPECT_EQ(lj.sizeBytes, 4096u);
    EXPECT_EQ(lj.status, "UPLOADED");

    // 旧 key 清理 + marker 完成
    for (const auto* k : {"ota.vehicle_task", "ota.inventory", "ota.consent",
                          "ota.downloads", "ota.execution", "ota.event_outbox",
                          "ota.controls", "ota.policy", "ota.log_jobs"}) {
        EXPECT_FALSE(store.has(k)) << k;
    }
    EXPECT_TRUE(store.has("fota.migration_marker"));
    EXPECT_EQ(r.migratedKeys, 9u);
}

// ---------------------------------------------------------------------------
// 3. 重复迁移幂等
// ---------------------------------------------------------------------------
TEST_F(MigrationTest, MigrationIsIdempotent) {
    writeFullLegacyState(store);
    auto r1 = migrateOtaToFota(store);
    EXPECT_TRUE(r1.ran);
    EXPECT_TRUE(r1.completed);

    // 第二次：marker 已完成且无旧 key -> no-op
    auto r2 = migrateOtaToFota(store);
    EXPECT_FALSE(r2.ran);
    EXPECT_TRUE(r2.completed);

    // 新 key 值未被破坏
    auto dl = fota::decodeDownload(store.load<std::string>("fota.downloads:PKG-VCU-001"));
    EXPECT_EQ(dl.currentOffsetBytes, 1024u);
}

// ---------------------------------------------------------------------------
// 4. 断电恢复：迁移已写入完成 marker 但清理阶段中断（旧 key 残留），重试清理
// ---------------------------------------------------------------------------
TEST_F(MigrationTest, InterruptedCleanupResumes) {
    writeFullLegacyState(store);
    // 第一次完整迁移：新 key + marker 写入，旧 key 清理。
    auto r1 = migrateOtaToFota(store);
    EXPECT_TRUE(r1.ran);
    EXPECT_TRUE(r1.completed);
    EXPECT_FALSE(store.has("ota.vehicle_task"));

    // 模拟崩溃发生在「写完成 marker 之后、清理旧 key 之前」：旧 key 重新残留，
    // 新 key 与已完成 marker 都在。重试应仅清理遗留旧 key，不破坏新 key。
    writeFullLegacyState(store);
    auto r2 = migrateOtaToFota(store);
    EXPECT_TRUE(r2.ran);
    EXPECT_TRUE(r2.completed);
    EXPECT_FALSE(store.has("ota.vehicle_task"));
    EXPECT_TRUE(store.has("fota.vehicle_task"));
    auto dl = fota::decodeDownload(store.load<std::string>("fota.downloads:PKG-VCU-001"));
    EXPECT_EQ(dl.currentOffsetBytes, 1024u);
}

// ---------------------------------------------------------------------------
// 5. 未知新格式/损坏 fail-closed（保留旧 key，可重试）
// ---------------------------------------------------------------------------
TEST_F(MigrationTest, CorruptLegacyKeyFailsClosed) {
    // 只写 vehicle_task，且 payload 非法
    store.save<std::string>("ota.vehicle_task", "not-a-valid-envelope");
    EXPECT_THROW(migrateOtaToFota(store), StateDecodeError);
    // 旧 key 保留，未产生部分新 key/marker
    EXPECT_TRUE(store.has("ota.vehicle_task"));
    EXPECT_FALSE(store.has("fota.migration_marker"));
}

TEST_F(MigrationTest, UnknownNewerLegacyVersionFailsClosed) {
    // schemaVersion=2 的旧记录 -> 未知新格式 fail-closed
    json j = {{"schemaVersion", 2}, {"vehicleTaskId", "VT-X"}, {"taskRevision", "1"}};
    store.save<std::string>("ota.vehicle_task", encodeEnvelope(2, j.dump()));
    EXPECT_THROW(migrateOtaToFota(store), StateDecodeError);
    EXPECT_TRUE(store.has("ota.vehicle_task"));
}
