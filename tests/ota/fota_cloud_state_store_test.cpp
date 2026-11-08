// =============================================================================
// tests/ota/fota_cloud_state_store_test.cpp
// CCU-FOTA 车云 FOTA 状态存储单元测试 (CCU-FOTA-DSN-CR-011 §Store)
// 覆盖：各记录 round-trip、删除、per-item key、proto-hex 辅助、未知新版本 fail-closed。
// =============================================================================

#include "ccu/fota/store/fota_cloud_state_serializer.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"
#include "ccu/fota/store/fota_state_store.hpp"

#include "vehicle/fota/v1/task.pb.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace ccu_fota::store;
using namespace ccu_fota::store::fota;

namespace {
fs::path makeUniqueRoot() {
    static int counter = 0;
    auto root = fs::temp_directory_path() /
                ("fota-cloud-store-test-" + std::to_string(getpid()) +
                 "-" + std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void corruptKeyFile(const fs::path& root, const std::string& key) {
    fs::path file = root / "fota" / (key + ".dat");
    ASSERT_TRUE(fs::exists(file)) << file;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    std::mt19937 rng(7);
    for (int i = 0; i < 64; ++i) out.put(static_cast<char>(rng() & 0xFF));
}
} // namespace

// ---------------------------------------------------------------------------
// proto-binary <-> hex 辅助
// ---------------------------------------------------------------------------
TEST(FotaCloudStateSerializer, ProtoHexRoundTrip) {
    ::vehicle::fota::v1::VehicleTaskSnapshot snap;
    snap.set_vehicle_task_id("VT-001");
    snap.set_task_revision(1);
    snap.set_target_baseline_code("BASE-001");
    snap.set_install_plan_version("plan-1");
    snap.set_start_time_ms(100);
    snap.set_end_time_ms(200);

    std::string bin;
    ASSERT_TRUE(snap.SerializeToString(&bin));
    std::string hex = protoBinaryToHex(bin);
    ASSERT_FALSE(hex.empty());
    std::string back = hexToProtoBinary(hex);
    EXPECT_EQ(back, bin);

    ::vehicle::fota::v1::VehicleTaskSnapshot snap2;
    ASSERT_TRUE(snap2.ParseFromString(back));
    EXPECT_EQ(snap2.vehicle_task_id(), "VT-001");
    EXPECT_EQ(snap2.task_revision(), 1u);
    EXPECT_EQ(snap2.start_time_ms(), 100);
}

TEST(FotaCloudStateSerializer, HexOddLengthFails) {
    EXPECT_THROW(hexToProtoBinary("abc"), StateDecodeError);
    EXPECT_THROW(hexToProtoBinary("xy"), StateDecodeError);
}

// ---------------------------------------------------------------------------
// 记录 round-trip（通过真实 store）
// ---------------------------------------------------------------------------
class FotaCloudStateStoreTest : public ::testing::Test {
protected:
    fs::path root = makeUniqueRoot();
    std::shared_ptr<FotaStateStore> fotaStore;
    std::unique_ptr<FotaCloudStateStore> store;

    void SetUp() override {
        auto s = FotaStateStore::open(root, 100, 3600000);
        fotaStore = std::make_shared<FotaStateStore>(std::move(s));
        store = std::make_unique<FotaCloudStateStore>(fotaStore->underlyingStore());
    }
};

TEST_F(FotaCloudStateStoreTest, VehicleTaskRoundTrip) {
    EXPECT_FALSE(store->loadVehicleTask().has_value());
    FotaVehicleTaskRecord r;
    r.vehicleTaskId = "VT-001";
    r.taskRevision = 7;
    r.targetBaselineCode = "BASE-001";
    r.vehicleTaskState = "DISCOVERED";
    r.frozenAtMs = 1000;
    r.taskSnapshotPbHex = "0a035654303031";
    r.superseded = false;
    store->saveVehicleTask(r);

    auto loaded = store->loadVehicleTask();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->vehicleTaskId, "VT-001");
    EXPECT_EQ(loaded->taskRevision, 7u);
    EXPECT_EQ(loaded->vehicleTaskState, "DISCOVERED");
    EXPECT_FALSE(loaded->superseded);

    store->removeVehicleTask();
    EXPECT_FALSE(store->loadVehicleTask().has_value());
}

TEST_F(FotaCloudStateStoreTest, InventoryRoundTrip) {
    FotaInventoryRecord r;
    r.mode = "FULL";
    r.inventoryRevision = 3;
    r.algorithm = "sha-256";
    r.ecuListDigestHex = "abc123";
    r.baselineCode = "BASE-001";
    r.fotaMasterVersion = "1.0.0";
    r.collectedAtMs = 5000;
    r.fullRequired = true;
    store->saveInventory(r);
    auto loaded = store->loadInventory();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->mode, "FULL");
    EXPECT_EQ(loaded->inventoryRevision, 3u);
    EXPECT_EQ(loaded->fotaMasterVersion, "1.0.0");
    EXPECT_TRUE(loaded->fullRequired);
}

TEST_F(FotaCloudStateStoreTest, ConsentRoundTrip) {
    FotaConsentRecord r;
    r.vehicleTaskId = "VT-001";
    r.effectiveStatus = "ACCEPTED";
    r.receiptId = "RCP-001";
    r.receiptExpiresAtMs = 99999;
    r.termsId = "T-1";
    r.termsVersion = "v1";
    r.termsDigestHex = std::string(64, 'a');
    r.consentTimeMs = 1234;
    r.channel = "vehicle";
    store->saveConsent(r);
    auto loaded = store->loadConsent();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->effectiveStatus, "ACCEPTED");
    EXPECT_EQ(loaded->receiptId, "RCP-001");
    EXPECT_EQ(loaded->termsDigestHex, std::string(64, 'a'));
}

TEST_F(FotaCloudStateStoreTest, DownloadPerItemRoundTrip) {
    // per-item key：fota.downloads:<package_id>
    FotaDownloadRecord r;
    r.packageId = "PKG-001";
    r.packageRevision = "prev-1";
    r.etag = "etag-1";
    r.currentOffsetBytes = 1024;
    r.offsetScope = "STORED_OBJECT";
    r.verifyStatus = "SUCCEEDED";
    r.stageResultId = "SR-001";
    r.stageResultDigestHex = "digest-1";
    r.ready = true;
    store->saveDownload(r);

    auto loaded = store->loadDownload("PKG-001");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->packageId, "PKG-001");
    EXPECT_EQ(loaded->currentOffsetBytes, 1024u);
    EXPECT_TRUE(loaded->ready);

    // 其它包不可见（per-item 隔离）
    EXPECT_FALSE(store->loadDownload("PKG-OTHER").has_value());

    store->removeDownload("PKG-001");
    EXPECT_FALSE(store->loadDownload("PKG-001").has_value());
}

TEST_F(FotaCloudStateStoreTest, ExecutionRoundTrip) {
    FotaExecutionRecord r;
    r.vehicleTaskId = "VT-001";
    r.executionId = "EX-001";
    r.attemptNo = 1;
    r.permitId = "PMT-001";
    r.permitToken = "tok";
    r.controlRevision = 5;
    r.validUntilMs = 8000;
    r.executionState = "INSTALL";
    r.installPlanVersion = "plan-1";
    store->saveExecution(r);
    auto loaded = store->loadExecution();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->executionId, "EX-001");
    EXPECT_EQ(loaded->attemptNo, 1u);
    EXPECT_EQ(loaded->controlRevision, 5u);
    EXPECT_EQ(loaded->executionState, "INSTALL");
}

TEST_F(FotaCloudStateStoreTest, EventOutboxMetaAndPerItemRoundTrip) {
    FotaEventOutboxMeta meta;
    meta.nextSequenceNo = 4;
    meta.acceptedSequenceNo = 2;
    store->saveEventOutboxMeta(meta);
    auto m = store->loadEventOutboxMeta();
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->nextSequenceNo, 4u);
    EXPECT_EQ(m->acceptedSequenceNo, 2u);

    FotaEventOutboxRecord e;
    e.sequenceNo = 3;
    e.eventId = "EVT-003";
    e.eventDigestHex = "d3";
    e.stage = "INSTALL";
    e.eventStatus = "SUCCEEDED";
    e.progress = 50;
    e.occurredAtMs = 1234;
    e.sendStatus = "PENDING";
    e.eventPbHex = "0a03455654";
    store->saveEvent(e);
    auto loaded = store->loadEvent(3);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->eventId, "EVT-003");
    EXPECT_EQ(loaded->sendStatus, "PENDING");

    // 其它序号不可见
    EXPECT_FALSE(store->loadEvent(4).has_value());
}

TEST_F(FotaCloudStateStoreTest, ControlPerItemRoundTrip) {
    FotaControlRecord r;
    r.controlId = "CTRL-1";
    r.controlRevision = 2;
    r.action = "PAUSE";
    r.scope = "EXECUTION";
    r.applyMode = "AT_SAFE_POINT";
    r.expiresAtMs = 9000;
    r.ackStatus = "APPLIED";
    r.ackSequenceNo = 1;
    store->saveControl(r);
    auto loaded = store->loadControl(2);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->controlRevision, 2u);
    EXPECT_EQ(loaded->action, "PAUSE");

    // 其它 revision 不可见
    EXPECT_FALSE(store->loadControl(3).has_value());
}

TEST_F(FotaCloudStateStoreTest, PolicyRoundTrip) {
    FotaPolicyRecord r;
    r.localPolicyVersion = "pv-1";
    r.basePreferenceVersion = "pref-0";
    r.preferenceVersion = "pv-1";
    r.effectivePolicyPbHex = "0a01";
    r.conflict = false;
    store->savePolicy(r);
    auto loaded = store->loadPolicy();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->preferenceVersion, "pv-1");
    EXPECT_FALSE(loaded->conflict);
}

TEST_F(FotaCloudStateStoreTest, LogJobPerItemRoundTrip) {
    FotaLogJobRecord r;
    r.logRequestId = "LOG-001";
    r.objectKey = "obj-1";
    r.digestHex = "abc";
    r.algorithm = "sha-256";
    r.sizeBytes = 4096;
    r.status = "UPLOADED";
    r.completedAtMs = 7777;
    store->saveLogJob(r);
    auto loaded = store->loadLogJob("LOG-001");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->logRequestId, "LOG-001");
    EXPECT_EQ(loaded->sizeBytes, 4096u);

    EXPECT_FALSE(store->loadLogJob("LOG-OTHER").has_value());
}

// ---------------------------------------------------------------------------
// 损坏 fail-closed
// ---------------------------------------------------------------------------
TEST_F(FotaCloudStateStoreTest, CorruptedExecutionFailsClosed) {
    FotaExecutionRecord r;
    r.executionId = "EX-001";
    r.executionState = "READY";
    r.vehicleTaskId = "VT-1";
    r.permitId = "p";
    r.permitToken = "t";
    store->saveExecution(r);
    ASSERT_TRUE(store->loadExecution().has_value());

    corruptKeyFile(root, fota::keys::EXECUTION);
    EXPECT_ANY_THROW(store->loadExecution());
}

// ---------------------------------------------------------------------------
// CCU-FOTA-DSN-CR-013：fota.execution v2 登记标识 round-trip
// ---------------------------------------------------------------------------
TEST(FotaCloudStateSerializerStandalone, ExecutionRegistrationIdRoundTrip) {
    FotaExecutionRecord r;
    r.executionId = "EX-1";
    r.vehicleTaskId = "VT-1";
    r.attemptNo = 2;
    r.executionState = "RESULT_PENDING";
    r.permitId = "p";
    r.permitToken = "t";
    r.installedVersionRegistrationId = "REG-42";

    auto d = decodeExecution(encodeExecution(r));
    EXPECT_EQ(d.schemaVersion, ccu_fota::store::fota::schema::EXECUTION_VERSION);
    EXPECT_EQ(d.executionId, "EX-1");
    EXPECT_EQ(d.attemptNo, 2u);
    EXPECT_EQ(d.installedVersionRegistrationId, "REG-42");
}

// ---------------------------------------------------------------------------
// CCU-FOTA-DSN-CR-013：v1 记录可升级读取（登标识为空 => 重启后重新登记，
// DIAG 侧以 executionId+attemptNo 幂等）
// ---------------------------------------------------------------------------
TEST(FotaCloudStateSerializerStandalone, ExecutionV1UpgradesWithEmptyRegistrationId) {
    FotaExecutionRecord r;
    r.executionId = "EX-V1";
    r.vehicleTaskId = "VT-V1";
    r.attemptNo = 1;
    r.executionState = "PERMIT_PERSISTED";
    r.permitId = "p";
    r.permitToken = "t";
    std::string bytes = encodeExecution(r);
    ASSERT_GE(bytes.size(), 8u);
    // 还原为 v1 记录：schemaVersion=1（字段缺省 = 空）。
    bytes[4] = 1; bytes[5] = 0; bytes[6] = 0; bytes[7] = 0;

    auto d = decodeExecution(bytes);
    EXPECT_EQ(d.schemaVersion, ccu_fota::store::fota::schema::EXECUTION_VERSION);
    EXPECT_EQ(d.executionId, "EX-V1");
    EXPECT_EQ(d.executionState, "PERMIT_PERSISTED");
    EXPECT_TRUE(d.installedVersionRegistrationId.empty());
}

// ---------------------------------------------------------------------------
// CCU-FOTA-DSN-CR-014 D5：fota.inventory schema v3
// v1/v2 -> v3 迁移保留 revision、accepted 状态、回执、attempt 与 digestFailures；
// 缺失 wireCanonicalization 回填 inventory-ecu-list-v1，不重算旧摘要。
// ---------------------------------------------------------------------------
TEST(FotaCloudStateSerializerStandalone, InventoryV3RoundTrip) {
    FotaInventoryRecord r;
    r.status = "ACCEPTED";
    r.mode = "DIGEST";
    r.inventoryRevision = 9;
    r.acceptedInventoryRevision = 9;
    r.algorithm = "sha-256";
    r.ecuListDigestHex = std::string(64, 'b');
    r.fingerprintCanonicalization = "cgw-fota-version-v2";
    r.fingerprintHex = std::string(64, 'c');
    r.wireCanonicalization = "inventory-ecu-list-v2";
    r.baselineCode = "BASE-9";
    r.attempt = 4;
    r.digestFailures = 2;

    auto d = decodeInventory(encodeInventory(r));
    EXPECT_EQ(d.schemaVersion, ccu_fota::store::fota::schema::INVENTORY_VERSION);
    EXPECT_EQ(d.schemaVersion, 3u);
    EXPECT_EQ(d.status, "ACCEPTED");
    EXPECT_EQ(d.inventoryRevision, 9u);
    EXPECT_EQ(d.acceptedInventoryRevision, 9u);
    EXPECT_EQ(d.fingerprintCanonicalization, "cgw-fota-version-v2");
    EXPECT_EQ(d.wireCanonicalization, "inventory-ecu-list-v2");
    EXPECT_EQ(d.attempt, 4u);
    EXPECT_EQ(d.digestFailures, 2u);
}

TEST(FotaCloudStateSerializerStandalone, InventoryV2MigratesToV3WithLegacyWireDomain) {
    // 手工构造真实的 v2 envelope（无 wireCanonicalization 字段）。
    using json = nlohmann::json;
    json v2 = {
        {"schemaVersion", 2},
        {"status", "ACCEPTED"},
        {"mode", "FULL"},
        {"inventoryRevision", 7},
        {"acceptedInventoryRevision", 7},
        {"requiredMode", "DIGEST"},
        {"algorithm", "sha-256"},
        {"ecuListDigestHex", std::string(64, 'd')},
        {"fingerprintCanonicalization", "cgw-fota-version-v1"},
        {"fingerprintHex", std::string(64, 'e')},
        {"baselineCode", "BASE-7"},
        {"fotaMasterVersion", "1.0.0"},
        {"collectedAtMs", 1234},
        {"requestPbHex", "cafe"},
        {"attempt", 3},
        {"nextRetryAtMs", 0},
        {"lastErrorCode", ""},
        {"digestFailures", 1},
        {"fullRequired", false}};
    std::string bytes = ccu_fota::store::encodeEnvelope(2, v2.dump());

    auto d = decodeInventory(bytes);
    EXPECT_EQ(d.schemaVersion, 3u);
    EXPECT_EQ(d.status, "ACCEPTED");
    EXPECT_EQ(d.inventoryRevision, 7u);
    EXPECT_EQ(d.acceptedInventoryRevision, 7u);
    EXPECT_EQ(d.requiredMode, "DIGEST");
    EXPECT_EQ(d.fingerprintCanonicalization, "cgw-fota-version-v1");
    EXPECT_EQ(d.fingerprintHex, std::string(64, 'e'));   // 不重算、不伪造
    EXPECT_EQ(d.ecuListDigestHex, std::string(64, 'd'));
    EXPECT_EQ(d.requestPbHex, "cafe");
    EXPECT_EQ(d.attempt, 3u);
    EXPECT_EQ(d.digestFailures, 1u);
    // 缺失 wire 域 => 回填历史 v1 域。
    EXPECT_EQ(d.wireCanonicalization, "inventory-ecu-list-v1");
}

TEST(FotaCloudStateSerializerStandalone, InventoryV1MigratesToV3) {
    FotaInventoryRecord r;
    r.mode = "FULL";
    r.inventoryRevision = 4;
    r.ecuListDigestHex = std::string(64, 'f');
    r.requestPbHex = "aabb";
    r.fullRequired = true;
    std::string bytes = encodeInventory(r);
    ASSERT_GE(bytes.size(), 8u);
    bytes[4] = 1; bytes[5] = 0; bytes[6] = 0; bytes[7] = 0;

    auto d = decodeInventory(bytes);
    EXPECT_EQ(d.schemaVersion, 3u);
    EXPECT_EQ(d.inventoryRevision, 4u);
    EXPECT_EQ(d.status, "REQUIRED_FULL");
    EXPECT_EQ(d.requestPbHex, "aabb");
    EXPECT_TRUE(d.fingerprintHex.empty());   // v1 无指纹，不得伪造
    EXPECT_TRUE(d.fingerprintCanonicalization.empty());
    EXPECT_EQ(d.wireCanonicalization, "inventory-ecu-list-v1");
}

// ---------------------------------------------------------------------------
// 序列化器：未知新版本 fail-closed
// ---------------------------------------------------------------------------
TEST(FotaCloudStateSerializerStandalone, UnknownNewerVersionFails) {
    FotaExecutionRecord r;
    r.executionId = "EX-X";
    r.executionState = "READY";
    r.vehicleTaskId = "VT-X";
    r.permitId = "p";
    r.permitToken = "t";
    std::string bytes = encodeExecution(r);
    // envelope: MAGIC(4) schemaVersion(4@off4) ... 篡改为“未知的新版本”。
    ASSERT_GE(bytes.size(), 8u);
    const std::uint32_t newer = ccu_fota::store::fota::schema::EXECUTION_VERSION + 1;
    bytes[4] = static_cast<char>(newer & 0xFF);
    bytes[5] = static_cast<char>((newer >> 8) & 0xFF);
    bytes[6] = static_cast<char>((newer >> 16) & 0xFF);
    bytes[7] = static_cast<char>((newer >> 24) & 0xFF);
    EXPECT_THROW(decodeExecution(bytes), StateDecodeError);
}
