// =============================================================================
// tests/test_fingerprint_store_migration.cpp
// CCU-FOTA Store 指纹迁移与校验测试 (CCU-FOTA-DSN-CR-006 §10.6 / §测试设计)
// 覆盖：v1(legacy fingerprint)->v2 迁移、v0->v2、v2 往返、Hex/algorithm/
//       canonicalization 校验 fail-closed、未知指纹放行、版本不匹配不误判。
// =============================================================================

#include "ccu/fota/store/fota_state_serializer.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

using namespace ccu_fota;
using namespace ccu_fota::store;
using json = nlohmann::json;

namespace {

std::string makeEnvelope(std::uint32_t schemaVersion, const std::string& payloadJson) {
    return encodeEnvelope(schemaVersion, payloadJson);
}

json snapshotJson(std::uint64_t seq) {
    return {{"vin", "V"}, {"vin_source", "UNKNOWN"}, {"baseline_source", "UNKNOWN"},
            {"registry_version", "rv"}, {"collected_at", "t"},
            {"overall_result", "ALL_OK"}, {"snapshot_seq", seq},
            {"ecu_list", json::array()}};
}

json fullFingerprintsJson() {
    return {{"algorithm", "sha-256"},
            {"canonicalization", "cgw-fota-snapshot-v1"},
            {"versionFingerprintHex", std::string(64, 'a')},
            {"snapshotFingerprintHex", std::string(64, 'b')},
            {"dedupeKeyHex", std::string(64, 'c')}};
}

} // namespace

// ===========================================================================
// v1(legacy) -> v2：旧 fingerprint 占位串被弃用，新 fingerprints 为未知（空）
// ===========================================================================
TEST(FingerprintStoreMigrationTest, LastSuccessV1ToV2DropsLegacyFingerprint) {
    json v1 = {{"schemaVersion", 1}, {"reportId", "r"}, {"snapshotSeq", 5},
               {"completedAt", 1}, {"registryVersion", "rv"},
               {"overallResult", "ALL_OK"}, {"fingerprint", "legacy-fp"},
               {"snapshot", snapshotJson(5)}};
    std::string env = makeEnvelope(1, v1.dump());
    LastSuccessState d = decodeLastSuccess(env);
    EXPECT_EQ(d.schemaVersion, schema::LAST_SUCCESS_VERSION);
    EXPECT_TRUE(d.fingerprints.algorithm.empty());           // 视为未知/不兼容
    EXPECT_TRUE(d.fingerprints.versionFingerprintHex.empty());
    EXPECT_TRUE(d.fingerprints.snapshotFingerprintHex.empty());
    EXPECT_TRUE(d.fingerprints.dedupeKeyHex.empty());
}

// ===========================================================================
// v0 -> v2：无 fingerprint 历史 -> 未知
// ===========================================================================
TEST(FingerprintStoreMigrationTest, LastSuccessV0ToV2Unknown) {
    json v0 = {{"reportId", "r"}, {"snapshotSeq", 5}, {"completedAt", 1},
               {"registryVersion", "rv"}, {"overallResult", "ALL_OK"},
               {"snapshot", snapshotJson(5)}};
    std::string env = makeEnvelope(0, v0.dump());
    LastSuccessState d = decodeLastSuccess(env);
    EXPECT_EQ(d.schemaVersion, schema::LAST_SUCCESS_VERSION);
    EXPECT_TRUE(d.fingerprints.algorithm.empty());
}

// ===========================================================================
// v2 往返：完整指纹保持
// ===========================================================================
TEST(FingerprintStoreMigrationTest, LastSuccessV2RoundTrip) {
    LastSuccessState s;
    s.reportId = "r";
    s.snapshotSeq = 7;
    s.completedAt = 1;
    s.registryVersion = "rv";
    s.overallResult = CollectionStatus::ALL_OK;
    s.fingerprints.algorithm = "sha-256";
    s.fingerprints.canonicalization = "cgw-fota-snapshot-v1";
    s.fingerprints.versionFingerprintHex = std::string(64, 'a');
    s.fingerprints.snapshotFingerprintHex = std::string(64, 'b');
    s.fingerprints.dedupeKeyHex = std::string(64, 'c');
    s.snapshot.vin = "V";
    s.snapshot.snapshot_seq = 7;
    std::string env = encodeLastSuccess(s);
    LastSuccessState d = decodeLastSuccess(env);
    EXPECT_EQ(d.fingerprints.algorithm, "sha-256");
    EXPECT_EQ(d.fingerprints.canonicalization, "cgw-fota-snapshot-v1");
    EXPECT_EQ(d.fingerprints.versionFingerprintHex, std::string(64, 'a'));
    EXPECT_EQ(d.fingerprints.dedupeKeyHex, std::string(64, 'c'));
}

// ===========================================================================
// 未知指纹（全空）放行：legacy/未建立指纹的记录可读，但不可比较
// ===========================================================================
TEST(FingerprintStoreMigrationTest, EmptyFingerprintsAllowed) {
    LastSuccessState s;
    s.reportId = "r";
    s.snapshotSeq = 7;
    s.snapshot.vin = "V";
    s.snapshot.snapshot_seq = 7;
    // fingerprints 全部默认空
    std::string env = encodeLastSuccess(s);
    EXPECT_NO_THROW(decodeLastSuccess(env));
}

// ===========================================================================
// Hex 校验 fail-closed：非 64 小写 hex -> StateDecodeError
// ===========================================================================
TEST(FingerprintStoreMigrationTest, BadHexFails) {
    json fp = fullFingerprintsJson();
    fp["versionFingerprintHex"] = "not-hex";  // 非法
    json p = {{"schemaVersion", 2}, {"reportId", "r"}, {"snapshotSeq", 5},
              {"completedAt", 1}, {"registryVersion", "rv"},
              {"overallResult", "ALL_OK"}, {"fingerprints", fp},
              {"snapshot", snapshotJson(5)}};
    std::string env = makeEnvelope(2, p.dump());
    EXPECT_THROW(decodeLastSuccess(env), StateDecodeError);
}

// ===========================================================================
// algorithm 校验 fail-closed：hex 非空但 algorithm != sha-256
// ===========================================================================
TEST(FingerprintStoreMigrationTest, BadAlgorithmFails) {
    json fp = fullFingerprintsJson();
    fp["algorithm"] = "md5";  // 非法
    json p = {{"schemaVersion", 2}, {"reportId", "r"}, {"snapshotSeq", 5},
              {"completedAt", 1}, {"registryVersion", "rv"},
              {"overallResult", "ALL_OK"}, {"fingerprints", fp},
              {"snapshot", snapshotJson(5)}};
    std::string env = makeEnvelope(2, p.dump());
    EXPECT_THROW(decodeLastSuccess(env), StateDecodeError);
}

// ===========================================================================
// canonicalization 校验 fail-closed：hex 非空但 canonicalization 不匹配
// ===========================================================================
TEST(FingerprintStoreMigrationTest, BadCanonicalizationFails) {
    json fp = fullFingerprintsJson();
    fp["canonicalization"] = "ccu-fota-snapshot-v2";  // 未知版本
    json p = {{"schemaVersion", 2}, {"reportId", "r"}, {"snapshotSeq", 5},
              {"completedAt", 1}, {"registryVersion", "rv"},
              {"overallResult", "ALL_OK"}, {"fingerprints", fp},
              {"snapshot", snapshotJson(5)}};
    std::string env = makeEnvelope(2, p.dump());
    EXPECT_THROW(decodeLastSuccess(env), StateDecodeError);
}

// ===========================================================================
// Dedupe v1 -> v2：每个条目弃用 fingerprint，新增未知 fingerprints
// ===========================================================================
TEST(FingerprintStoreMigrationTest, DedupeV1ToV2Migration) {
    json entry = {{"requestId", "req"}, {"reportId", "rpt"}, {"snapshotSeq", 3},
                  {"fingerprint", "legacy"}, {"overallResult", "ALL_OK"},
                  {"completedAt", 1}, {"expiresAt", 0}};
    json v1 = {{"schemaVersion", 1}, {"entries", json::array({entry})},
               {"maxEntries", 10}, {"ttlMs", 0}};
    std::string env = makeEnvelope(1, v1.dump());
    DedupeState d = decodeDedupe(env);
    EXPECT_EQ(d.schemaVersion, schema::DEDUPE_VERSION);
    ASSERT_EQ(d.entries.size(), 1u);
    EXPECT_TRUE(d.entries[0].fingerprints.algorithm.empty());  // 未知
    EXPECT_TRUE(d.entries[0].fingerprints.snapshotFingerprintHex.empty());
}

// ===========================================================================
// ActiveJob v1 -> v2：新增未知 fingerprints
// ===========================================================================
TEST(FingerprintStoreMigrationTest, ActiveJobV1ToV2Migration) {
    json v1 = {{"schemaVersion", 1}, {"requestId", "r"}, {"reportId", "rp"},
               {"snapshotSeq", 1}, {"reason", "AutoStart"}, {"phase", "Accepted"},
               {"attempt", 0}, {"nextRetryAt", 0}, {"lastErrorCode", ""},
               {"idempotencyKey", "idem"}};
    std::string env = makeEnvelope(1, v1.dump());
    ActiveJobState d = decodeActiveJob(env);
    EXPECT_EQ(d.schemaVersion, schema::ACTIVE_JOB_VERSION);
    EXPECT_TRUE(d.fingerprints.algorithm.empty());  // 未知
    EXPECT_EQ(d.idempotencyKey, "idem");
}

// ===========================================================================
// 版本不匹配不误判：迁移后的未知指纹不得与 v1 指纹直接比较
// （语义测试：v1 记录迁移后 fingerprints 为空，运行时应判定为“需完整采集”）
// ===========================================================================
TEST(FingerprintStoreMigrationTest, MigratedUnknownNotComparable) {
    json v1 = {{"schemaVersion", 1}, {"reportId", "r"}, {"snapshotSeq", 5},
               {"completedAt", 1}, {"registryVersion", "rv"},
               {"overallResult", "ALL_OK"}, {"fingerprint", "legacy-fp"},
               {"snapshot", snapshotJson(5)}};
    LastSuccessState d = decodeLastSuccess(makeEnvelope(1, v1.dump()));
    // 未知指纹：algorithm 为空，不得参与版本比较
    EXPECT_TRUE(d.fingerprints.algorithm.empty());
    EXPECT_TRUE(d.fingerprints.canonicalization.empty());
    EXPECT_TRUE(d.fingerprints.versionFingerprintHex.empty());
}
