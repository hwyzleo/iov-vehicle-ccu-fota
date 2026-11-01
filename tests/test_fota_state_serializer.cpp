// =============================================================================
// tests/test_fota_state_serializer.cpp
// CCU-FOTA 状态序列化器单元测试 (CCU-FOTA-DSN-CR-005)
// 覆盖：envelope 往返、v0->v1 迁移、重复迁移、未知新版本 fail-closed、
//       损坏（截断/错 magic/错长度/非法枚举/超大值）。
// =============================================================================

#include "ccu/fota/store/fota_state_serializer.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <string>

using namespace ccu_fota;
using namespace ccu_fota::store;
using json = nlohmann::json;

namespace {
// 构造指定 schemaVersion 的 envelope（测试旧版本与迁移）。
std::string makeEnvelope(std::uint32_t schemaVersion, const std::string& payloadJson) {
    return encodeEnvelope(schemaVersion, payloadJson);
}

// 辅助：构造一个完整的 VehicleSoftwareSnapshot。
VehicleSoftwareSnapshot makeSnapshot(std::uint64_t seq) {
    VehicleSoftwareSnapshot s;
    s.vin = "LSJAAAAAAAAAAAAAA";
    s.baseline_id = "bl-001";
    s.baseline_source = BaselineSource::FACTORY;
    s.registry_version = "1.0.0";
    s.collected_at = "2026-08-07T10:00:00Z";
    s.overall_result = CollectionStatus::ALL_OK;
    s.snapshot_seq = seq;
    EcuVersionEntry e;
    e.ecu_id = "ECU1";
    e.part_number = "P001";
    e.sw_version = "1.2.3";
    e.source = VersionSource::UDS_0x22;
    e.status = EcuStatus::OK;
    s.ecu_list.push_back(e);
    return s;
}
} // namespace

// ===========================================================================
// Envelope 基础
// ===========================================================================
TEST(FotaStateSerializerTest, EnvelopeHeaderRoundTrip) {
    std::string env = makeEnvelope(1, R"({"k":1})");
    EnvelopeHeader h = parseEnvelopeHeader(env);
    EXPECT_EQ(h.schemaVersion, 1u);
    EXPECT_EQ(h.minReaderVersion, schema::MIN_READER_VERSION);
    EXPECT_EQ(h.payloadLen, 7u);
    EXPECT_EQ(extractPayload(env, h), R"({"k":1})");
}

TEST(FotaStateSerializerTest, EnvelopeTruncatedFails) {
    std::string env = makeEnvelope(1, R"({"k":1})");
    EXPECT_THROW(parseEnvelopeHeader(env.substr(0, 10)), StateDecodeError);
}

TEST(FotaStateSerializerTest, EnvelopeBadMagicFails) {
    std::string env = makeEnvelope(1, R"({"k":1})");
    env[0] = 'X';
    EXPECT_THROW(parseEnvelopeHeader(env), StateDecodeError);
}

TEST(FotaStateSerializerTest, EnvelopeBadLengthFails) {
    std::string env = makeEnvelope(1, R"({"k":1})");
    env.push_back('!');  // 多余字节 -> 长度不匹配
    EXPECT_THROW(parseEnvelopeHeader(env), StateDecodeError);
}

TEST(FotaStateSerializerTest, EnvelopeMinReaderTooHighFails) {
    // 手工构造 minReaderVersion 超出 WRITER_VERSION
    std::string env = makeEnvelope(1, R"({"k":1})");
    // minReaderVersion 在 offset 12 (u32 LE)，改为 WRITER_VERSION+1
    std::uint32_t bad = schema::WRITER_VERSION + 1;
    env[12] = static_cast<char>(bad & 0xFF);
    env[13] = static_cast<char>((bad >> 8) & 0xFF);
    env[14] = static_cast<char>((bad >> 16) & 0xFF);
    env[15] = static_cast<char>((bad >> 24) & 0xFF);
    EXPECT_THROW(parseEnvelopeHeader(env), StateDecodeError);
}

// ===========================================================================
// SequenceState 往返
// ===========================================================================
TEST(FotaStateSerializerTest, SequenceRoundTrip) {
    SequenceState s;
    s.highestAllocated = 42;
    s.updatedAt = 1700000000000;
    std::string env = encodeSequence(s);
    SequenceState d = decodeSequence(env);
    EXPECT_EQ(d.schemaVersion, schema::SEQUENCE_VERSION);
    EXPECT_EQ(d.highestAllocated, 42u);
    EXPECT_EQ(d.updatedAt, 1700000000000);
}

TEST(FotaStateSerializerTest, SequenceMigrationV0ToV1) {
    // v0 payload: 无 schemaVersion / updatedAt
    std::string v0Payload = R"({"highestAllocated":7})";
    std::string env = makeEnvelope(0, v0Payload);
    SequenceState d = decodeSequence(env);
    EXPECT_EQ(d.schemaVersion, schema::SEQUENCE_VERSION);
    EXPECT_EQ(d.highestAllocated, 7u);
    EXPECT_EQ(d.updatedAt, 0);  // 迁移补齐
}

TEST(FotaStateSerializerTest, SequenceMigrationIsIdempotent) {
    std::string v0Payload = R"({"highestAllocated":7})";
    // 迁移一次
    std::string m1 = migrateSequencePayload(v0Payload, 0);
    // 再迁移（已为 v1）应原样返回
    std::string m2 = migrateSequencePayload(m1, schema::SEQUENCE_VERSION);
    EXPECT_EQ(m1, m2);
}

TEST(FotaStateSerializerTest, SequenceUnknownNewerVersionFails) {
    std::string env = makeEnvelope(schema::SEQUENCE_VERSION + 1,
                                   R"({"highestAllocated":1})");
    EXPECT_THROW(decodeSequence(env), StateDecodeError);
}

// ===========================================================================
// LastSuccessState 往返
// ===========================================================================
TEST(FotaStateSerializerTest, LastSuccessRoundTrip) {
    LastSuccessState s;
    s.reportId = "rpt-001";
    s.snapshotSeq = 100;
    s.completedAt = 1700000000000;
    s.registryVersion = "1.0.0";
    s.overallResult = CollectionStatus::PARTIAL;
    s.fingerprints.algorithm = "sha-256";
    s.fingerprints.canonicalization = "cgw-fota-snapshot-v1";
    s.fingerprints.versionFingerprintHex = std::string(64, 'a');
    s.fingerprints.snapshotFingerprintHex = std::string(64, 'b');
    s.fingerprints.dedupeKeyHex = std::string(64, 'c');
    s.snapshot = makeSnapshot(100);
    std::string env = encodeLastSuccess(s);
    LastSuccessState d = decodeLastSuccess(env);
    EXPECT_EQ(d.reportId, "rpt-001");
    EXPECT_EQ(d.snapshotSeq, 100u);
    EXPECT_EQ(d.completedAt, 1700000000000);
    EXPECT_EQ(d.registryVersion, "1.0.0");
    EXPECT_EQ(d.overallResult, CollectionStatus::PARTIAL);
    EXPECT_EQ(d.fingerprints.algorithm, "sha-256");
    EXPECT_EQ(d.fingerprints.canonicalization, "cgw-fota-snapshot-v1");
    EXPECT_EQ(d.fingerprints.versionFingerprintHex, std::string(64, 'a'));
    EXPECT_EQ(d.fingerprints.snapshotFingerprintHex, std::string(64, 'b'));
    EXPECT_EQ(d.fingerprints.dedupeKeyHex, std::string(64, 'c'));
    EXPECT_EQ(d.snapshot.vin, "LSJAAAAAAAAAAAAAA");
    EXPECT_EQ(d.snapshot.snapshot_seq, 100u);
    ASSERT_EQ(d.snapshot.ecu_list.size(), 1u);
    EXPECT_EQ(d.snapshot.ecu_list[0].ecu_id, "ECU1");
    EXPECT_EQ(d.snapshot.ecu_list[0].sw_version.value(), "1.2.3");
}

TEST(FotaStateSerializerTest, LastSuccessMigrationV0ToV2) {
    // v0: 无 fingerprint
    json snap = {{"vin", "V"}, {"baseline_source", "UNKNOWN"},
                 {"registry_version", "rv"}, {"collected_at", "t"},
                 {"overall_result", "ALL_OK"}, {"snapshot_seq", 5},
                 {"ecu_list", json::array()}};
    json v0 = {{"reportId", "r"}, {"snapshotSeq", 5}, {"completedAt", 1},
               {"registryVersion", "rv"}, {"overallResult", "ALL_OK"},
               {"snapshot", snap}};
    std::string env = makeEnvelope(0, v0.dump());
    LastSuccessState d = decodeLastSuccess(env);
    EXPECT_EQ(d.schemaVersion, schema::LAST_SUCCESS_VERSION);
    EXPECT_TRUE(d.fingerprints.algorithm.empty());          // 迁移补齐为未知
    EXPECT_TRUE(d.fingerprints.versionFingerprintHex.empty());
    EXPECT_EQ(d.snapshotSeq, 5u);
}

TEST(FotaStateSerializerTest, LastSuccessSnapshotSeqMismatchFails) {
    // envelope payload 中 snapshotSeq 与 snapshot.snapshot_seq 不一致
    json snap = {{"vin", "V"}, {"baseline_source", "UNKNOWN"},
                 {"registry_version", "rv"}, {"collected_at", "t"},
                 {"overall_result", "ALL_OK"}, {"snapshot_seq", 99},
                 {"ecu_list", json::array()}};
    json p = {{"schemaVersion", 1}, {"reportId", "r"}, {"snapshotSeq", 5},
              {"completedAt", 1}, {"registryVersion", "rv"},
              {"overallResult", "ALL_OK"}, {"fingerprint", ""}, {"snapshot", snap}};
    std::string env = makeEnvelope(1, p.dump());
    EXPECT_THROW(decodeLastSuccess(env), StateDecodeError);
}

TEST(FotaStateSerializerTest, LastSuccessUnknownNewerVersionFails) {
    std::string env = makeEnvelope(schema::LAST_SUCCESS_VERSION + 1,
                                   R"({"reportId":"r"})");
    EXPECT_THROW(decodeLastSuccess(env), StateDecodeError);
}

// ===========================================================================
// DedupeState 往返
// ===========================================================================
TEST(FotaStateSerializerTest, DedupeRoundTrip) {
    DedupeState s;
    s.maxEntries = 50;
    s.ttlMs = 300000;
    DedupeEntry e;
    e.requestId = "req-1";
    e.reportId = "rpt-1";
    e.snapshotSeq = 7;
    e.fingerprints.algorithm = "sha-256";
    e.fingerprints.canonicalization = "cgw-fota-snapshot-v1";
    e.fingerprints.snapshotFingerprintHex = std::string(64, 'd');
    e.fingerprints.dedupeKeyHex = std::string(64, 'e');
    e.overallResult = "ALL_OK";
    e.completedAt = 1700000000000;
    e.expiresAt = 1700000300000;
    s.entries.push_back(e);
    std::string env = encodeDedupe(s);
    DedupeState d = decodeDedupe(env);
    EXPECT_EQ(d.maxEntries, 50u);
    EXPECT_EQ(d.ttlMs, 300000);
    ASSERT_EQ(d.entries.size(), 1u);
    EXPECT_EQ(d.entries[0].requestId, "req-1");
    EXPECT_EQ(d.entries[0].snapshotSeq, 7u);
    EXPECT_EQ(d.entries[0].expiresAt, 1700000300000);
    EXPECT_EQ(d.entries[0].fingerprints.algorithm, "sha-256");
    EXPECT_EQ(d.entries[0].fingerprints.snapshotFingerprintHex, std::string(64, 'd'));
}

TEST(FotaStateSerializerTest, DedupeMigrationV0ToV2) {
    // v0: 用 maxSize 而非 maxEntries，无 ttlMs
    json v0 = {{"entries", json::array()},
               {"maxSize", 30}};
    std::string env = makeEnvelope(0, v0.dump());
    DedupeState d = decodeDedupe(env);
    EXPECT_EQ(d.schemaVersion, schema::DEDUPE_VERSION);
    EXPECT_EQ(d.maxEntries, 30u);  // 重命名
    EXPECT_EQ(d.ttlMs, 0);          // 补齐
}

TEST(FotaStateSerializerTest, DedupeUnknownNewerVersionFails) {
    std::string env = makeEnvelope(schema::DEDUPE_VERSION + 1, R"({})");
    EXPECT_THROW(decodeDedupe(env), StateDecodeError);
}

// ===========================================================================
// ActiveJobState 往返
// ===========================================================================
TEST(FotaStateSerializerTest, ActiveJobRoundTrip) {
    ActiveJobState s;
    s.requestId = "req-1";
    s.reportId = "rpt-1";
    s.snapshotSeq = 9;
    s.reason = TriggerReason::CloudRequest;
    s.phase = JobPhase::SubmitPrepared;
    s.attempt = 2;
    s.nextRetryAt = 1700000000000;
    s.lastErrorCode = "CCU-FOTA-1004";
    s.idempotencyKey = "idem-abc";
    std::string env = encodeActiveJob(s);
    ActiveJobState d = decodeActiveJob(env);
    EXPECT_EQ(d.requestId, "req-1");
    EXPECT_EQ(d.reportId, "rpt-1");
    EXPECT_EQ(d.snapshotSeq, 9u);
    EXPECT_EQ(d.reason, TriggerReason::CloudRequest);
    EXPECT_EQ(d.phase, JobPhase::SubmitPrepared);
    EXPECT_EQ(d.attempt, 2u);
    EXPECT_EQ(d.nextRetryAt, 1700000000000);
    EXPECT_EQ(d.lastErrorCode, "CCU-FOTA-1004");
    EXPECT_EQ(d.idempotencyKey, "idem-abc");
}

TEST(FotaStateSerializerTest, ActiveJobMigrationV0ToV2) {
    // v0: 无 idempotencyKey
    json v0 = {{"requestId", "r"}, {"reportId", "rp"}, {"snapshotSeq", 1},
               {"reason", "AutoStart"}, {"phase", "Accepted"},
               {"attempt", 0}, {"nextRetryAt", 0}, {"lastErrorCode", ""}};
    std::string env = makeEnvelope(0, v0.dump());
    ActiveJobState d = decodeActiveJob(env);
    EXPECT_EQ(d.schemaVersion, schema::ACTIVE_JOB_VERSION);
    EXPECT_EQ(d.idempotencyKey, "");  // 迁移补齐
    EXPECT_TRUE(d.fingerprints.algorithm.empty());  // 迁移补齐为未知
    EXPECT_EQ(d.phase, JobPhase::Accepted);
}

TEST(FotaStateSerializerTest, ActiveJobInvalidPhaseFails) {
    json p = {{"schemaVersion", 1}, {"requestId", "r"}, {"reportId", "rp"},
              {"snapshotSeq", 1}, {"reason", "AutoStart"}, {"phase", "BogusPhase"},
              {"attempt", 0}, {"nextRetryAt", 0}, {"lastErrorCode", ""},
              {"idempotencyKey", ""}};
    std::string env = makeEnvelope(1, p.dump());
    EXPECT_THROW(decodeActiveJob(env), StateDecodeError);
}

TEST(FotaStateSerializerTest, ActiveJobInvalidReasonFails) {
    json p = {{"schemaVersion", 1}, {"requestId", "r"}, {"reportId", "rp"},
              {"snapshotSeq", 1}, {"reason", "BogusReason"}, {"phase", "Accepted"},
              {"attempt", 0}, {"nextRetryAt", 0}, {"lastErrorCode", ""},
              {"idempotencyKey", ""}};
    std::string env = makeEnvelope(1, p.dump());
    EXPECT_THROW(decodeActiveJob(env), StateDecodeError);
}

TEST(FotaStateSerializerTest, ActiveJobUnknownNewerVersionFails) {
    std::string env = makeEnvelope(schema::ACTIVE_JOB_VERSION + 1,
                                   R"({"requestId":"r"})");
    EXPECT_THROW(decodeActiveJob(env), StateDecodeError);
}

// ===========================================================================
// 损坏：随机翻转、超大 payload
// ===========================================================================
TEST(FotaStateSerializerTest, SequenceBitFlipInPayloadFails) {
    SequenceState s;
    s.highestAllocated = 42;
    std::string env = encodeSequence(s);
    // 翻转 payload 区域一字节（HEADER_SIZE 之后）
    env[envelope::HEADER_SIZE] ^= 0xFF;
    EXPECT_THROW(decodeSequence(env), StateDecodeError);
}

TEST(FotaStateSerializerTest, EmptyBytesFails) {
    std::string empty;
    EXPECT_THROW(parseEnvelopeHeader(empty), StateDecodeError);
}
