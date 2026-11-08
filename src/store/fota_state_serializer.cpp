// =============================================================================
// src/store/fota_state_serializer.cpp
// CCU-FOTA 状态序列化器实现 (CCU-FOTA-DSN-CR-005)
// =============================================================================

#include "ccu/fota/store/fota_state_serializer.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>

namespace ccu_fota {
namespace store {

using json = nlohmann::json;

StateDecodeError::StateDecodeError(const std::string& message)
    : std::runtime_error(message) {}

// ===========================================================================
// 小端字节打包 / 解包
// ===========================================================================
namespace {

void putU32Le(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void putU64Le(std::string& out, std::int64_t v) {
    auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
    }
}

std::uint32_t getU32Le(const std::string& b, std::size_t off) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off]))
         | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + 1])) << 8)
         | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + 2])) << 16)
         | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + 3])) << 24);
}

std::int64_t getU64Le(const std::string& b, std::size_t off) {
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i) {
        u |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b[off + i])) << (8 * i);
    }
    return static_cast<std::int64_t>(u);
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// data_models.h 类型的显式 JSON 转换（不依赖 ADL，避免命名空间陷阱）
// ---------------------------------------------------------------------------
json ecuEntryToJson(const EcuVersionEntry& e) {
    json j = json{{"ecu_id", e.ecu_id},
                  {"source", versionSourceToString(e.source)},
                  {"status", ecuStatusToString(e.status)}};
    if (e.part_number) j["part_number"] = *e.part_number;
    // CR-014：独立软件件号纳入 legacy 快照记录（与总成件号分离往返）。
    if (e.software_part_number) j["software_part_number"] = *e.software_part_number;
    if (e.sw_version)  j["sw_version"]  = *e.sw_version;
    if (e.hw_version)  j["hw_version"]  = *e.hw_version;
    if (e.error_code)  j["error_code"]  = *e.error_code;
    return j;
}

EcuVersionEntry ecuEntryFromJson(const json& j) {
    EcuVersionEntry e;
    e.ecu_id = j.at("ecu_id").get<std::string>();
    {
        std::string s = j.at("source").get<std::string>();
        if (s == "UDS_0x22")            e.source = VersionSource::UDS_0x22;
        else if (s == "SOMEIP_GET_VERSION") e.source = VersionSource::SOMEIP_GET_VERSION;
        else throw StateDecodeError("EcuVersionEntry bad source: " + s);
    }
    {
        std::string s = j.at("status").get<std::string>();
        if (s == "OK")            e.status = EcuStatus::OK;
        else if (s == "MISSING")  e.status = EcuStatus::MISSING;
        else if (s == "NRC")      e.status = EcuStatus::NRC;
        else if (s == "TIMEOUT")  e.status = EcuStatus::TIMEOUT;
        else if (s == "UNREACHABLE") e.status = EcuStatus::UNREACHABLE;
        else if (s == "PARSE_ERROR") e.status = EcuStatus::PARSE_ERROR;
        else throw StateDecodeError("EcuVersionEntry bad status: " + s);
    }
    if (j.contains("part_number")) e.part_number = j["part_number"].get<std::string>();
    // 旧记录无该字段 => 保持缺失（不得用 part_number 补齐）。
    if (j.contains("software_part_number"))
        e.software_part_number = j["software_part_number"].get<std::string>();
    if (j.contains("sw_version"))  e.sw_version  = j["sw_version"].get<std::string>();
    if (j.contains("hw_version"))  e.hw_version  = j["hw_version"].get<std::string>();
    if (j.contains("error_code"))  e.error_code  = j["error_code"].get<std::string>();
    return e;
}

json snapshotToJson(const VehicleSoftwareSnapshot& s) {
    json j = json{
        {"vin", s.vin},
        {"vin_source", vinSourceToString(s.vin_source)},
        {"baseline_source", baselineSourceToString(s.baseline_source)},
        {"registry_version", s.registry_version},
        {"collected_at", s.collected_at},
        {"overall_result", collectionStatusToString(s.overall_result)},
        {"snapshot_seq", s.snapshot_seq},
        {"ecu_list", json::array()}};
    if (s.baseline_id) j["baseline_id"] = *s.baseline_id;
    for (const auto& e : s.ecu_list) {
        j["ecu_list"].push_back(ecuEntryToJson(e));
    }
    return j;
}

VehicleSoftwareSnapshot snapshotFromJson(const json& j) {
    VehicleSoftwareSnapshot s;
    s.vin = j.at("vin").get<std::string>();
    {
        std::string v = j.value("vin_source", std::string("UNKNOWN"));
        if (v == "PROVISIONED")      s.vin_source = VinSource::PROVISIONED;
        else if (v == "UNKNOWN")     s.vin_source = VinSource::UNKNOWN;
        else throw StateDecodeError("snapshot bad vin_source: " + v);
    }
    if (j.contains("baseline_id")) s.baseline_id = j["baseline_id"].get<std::string>();
    {
        std::string b = j.at("baseline_source").get<std::string>();
        if (b == "FACTORY")       s.baseline_source = BaselineSource::FACTORY;
        else if (b == "LAST_OTA") s.baseline_source = BaselineSource::LAST_OTA;
        else if (b == "UNKNOWN")  s.baseline_source = BaselineSource::UNKNOWN;
        else throw StateDecodeError("snapshot bad baseline_source: " + b);
    }
    s.registry_version = j.at("registry_version").get<std::string>();
    s.collected_at     = j.at("collected_at").get<std::string>();
    {
        std::string r = j.at("overall_result").get<std::string>();
        if (r == "ALL_OK")       s.overall_result = CollectionStatus::ALL_OK;
        else if (r == "PARTIAL") s.overall_result = CollectionStatus::PARTIAL;
        else if (r == "FAILED")  s.overall_result = CollectionStatus::FAILED;
        else throw StateDecodeError("snapshot bad overall_result: " + r);
    }
    s.snapshot_seq = j.at("snapshot_seq").get<std::uint64_t>();
    for (const auto& ej : j.at("ecu_list")) {
        s.ecu_list.push_back(ecuEntryFromJson(ej));
    }
    return s;
}

CollectionStatus overallResultFromString(const std::string& r) {
    if (r == "ALL_OK")       return CollectionStatus::ALL_OK;
    if (r == "PARTIAL")      return CollectionStatus::PARTIAL;
    if (r == "FAILED")       return CollectionStatus::FAILED;
    throw StateDecodeError("bad overallResult: " + r);
}

// ---------------------------------------------------------------------------
// PersistedFingerprints JSON 转换与校验 (CCU-FOTA-DSN-CR-006 §10.6)
// 比较前必须先校验 algorithm 与 canonicalization；空 fingerprints 视为
// 未知/遗留，允许（不可比较）。
// ---------------------------------------------------------------------------
json persistedFingerprintsToJson(const PersistedFingerprints& fp) {
    return json{{"algorithm", fp.algorithm},
                {"canonicalization", fp.canonicalization},
                {"versionFingerprintHex", fp.versionFingerprintHex},
                {"snapshotFingerprintHex", fp.snapshotFingerprintHex},
                {"dedupeKeyHex", fp.dedupeKeyHex}};
}

PersistedFingerprints persistedFingerprintsFromJson(const json& j) {
    PersistedFingerprints fp;
    fp.algorithm              = j.value("algorithm", std::string());
    fp.canonicalization       = j.value("canonicalization", std::string());
    fp.versionFingerprintHex  = j.value("versionFingerprintHex", std::string());
    fp.snapshotFingerprintHex = j.value("snapshotFingerprintHex", std::string());
    fp.dedupeKeyHex           = j.value("dedupeKeyHex", std::string());
    return fp;
}

bool isValidHex64(const std::string& h) {
    if (h.size() != 64) return false;
    for (char c : h) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// 任一 hex 非空时必须 algorithm="sha-256" 且 canonicalization 为已知快照域；
// 所有非空 hex 必须为 64 小写 hex。空 fingerprints 视为未知/遗留，允许。
// CR-014：快照域升至 cgw-fota-snapshot-v2，v1 记录仍可读取；未知域 fail-closed。
// 保留域名而比较仍以 canonicalization 相等为前提（不跨版本比较）。
void validatePersistedFingerprints(const PersistedFingerprints& fp,
                                   const char* stage) {
    bool anyHex = !fp.versionFingerprintHex.empty() ||
                  !fp.snapshotFingerprintHex.empty() ||
                  !fp.dedupeKeyHex.empty();
    if (anyHex) {
        if (fp.algorithm != "sha-256") {
            throw StateDecodeError(std::string(stage) +
                                   " bad fingerprint algorithm: " + fp.algorithm);
        }
        if (fp.canonicalization != "cgw-fota-snapshot-v1" &&
            fp.canonicalization != "cgw-fota-snapshot-v2") {
            throw StateDecodeError(std::string(stage) +
                                   " bad fingerprint canonicalization: " +
                                   fp.canonicalization);
        }
    }
    if (!fp.versionFingerprintHex.empty() && !isValidHex64(fp.versionFingerprintHex)) {
        throw StateDecodeError(std::string(stage) + " bad versionFingerprintHex");
    }
    if (!fp.snapshotFingerprintHex.empty() && !isValidHex64(fp.snapshotFingerprintHex)) {
        throw StateDecodeError(std::string(stage) + " bad snapshotFingerprintHex");
    }
    if (!fp.dedupeKeyHex.empty() && !isValidHex64(fp.dedupeKeyHex)) {
        throw StateDecodeError(std::string(stage) + " bad dedupeKeyHex");
    }
}

json dedupeEntryToJson(const DedupeEntry& e) {
    return json{{"requestId", e.requestId},
                {"reportId", e.reportId},
                {"snapshotSeq", e.snapshotSeq},
                {"fingerprints", persistedFingerprintsToJson(e.fingerprints)},
                {"overallResult", e.overallResult},
                {"completedAt", e.completedAt},
                {"expiresAt", e.expiresAt}};
}

DedupeEntry dedupeEntryFromJson(const json& j) {
    DedupeEntry e;
    e.requestId     = j.at("requestId").get<std::string>();
    e.reportId      = j.at("reportId").get<std::string>();
    e.snapshotSeq   = j.at("snapshotSeq").get<std::uint64_t>();
    e.fingerprints  = persistedFingerprintsFromJson(j.value("fingerprints", json::object()));
    validatePersistedFingerprints(e.fingerprints, "dedupe entry");
    e.overallResult = j.at("overallResult").get<std::string>();
    e.completedAt   = j.at("completedAt").get<Timestamp>();
    e.expiresAt     = j.at("expiresAt").get<Timestamp>();
    return e;
}

} // namespace

// ===========================================================================
// Envelope 编解码
// ===========================================================================
std::string encodeEnvelope(std::uint32_t schemaVersion, const std::string& payloadJson) {
    std::string out;
    out.reserve(envelope::HEADER_SIZE + payloadJson.size());
    out.append(envelope::MAGIC, 4);
    putU32Le(out, schemaVersion);
    putU32Le(out, schema::WRITER_VERSION);
    putU32Le(out, schema::MIN_READER_VERSION);
    putU64Le(out, nowMs());
    putU32Le(out, static_cast<std::uint32_t>(payloadJson.size()));
    out.append(payloadJson);
    return out;
}

EnvelopeHeader parseEnvelopeHeader(const std::string& bytes) {
    if (bytes.size() < envelope::HEADER_SIZE) {
        throw StateDecodeError("envelope truncated: size=" + std::to_string(bytes.size()));
    }
    if (std::memcmp(bytes.data(), envelope::MAGIC, 4) != 0) {
        throw StateDecodeError("envelope bad magic");
    }
    EnvelopeHeader h;
    h.schemaVersion   = getU32Le(bytes, 4);
    h.writerVersion   = getU32Le(bytes, 8);
    h.minReaderVersion = getU32Le(bytes, 12);
    h.writtenAt       = getU64Le(bytes, 16);
    h.payloadLen      = getU32Le(bytes, 24);
    if (bytes.size() != envelope::HEADER_SIZE + h.payloadLen) {
        throw StateDecodeError("envelope length mismatch: declared=" +
                               std::to_string(h.payloadLen) + " actual=" +
                               std::to_string(bytes.size() - envelope::HEADER_SIZE));
    }
    // minReaderVersion 超出本二进制能力 -> fail-closed（不可降级解释）
    if (h.minReaderVersion > schema::WRITER_VERSION) {
        throw StateDecodeError("envelope minReaderVersion too high: " +
                               std::to_string(h.minReaderVersion));
    }
    return h;
}

std::string extractPayload(const std::string& bytes, const EnvelopeHeader& hdr) {
    return bytes.substr(envelope::HEADER_SIZE, hdr.payloadLen);
}

// ===========================================================================
// SequenceState
// ===========================================================================
std::string encodeSequence(const SequenceState& s) {
    json j = json{{"schemaVersion", s.schemaVersion},
                  {"highestAllocated", s.highestAllocated},
                  {"updatedAt", s.updatedAt}};
    return encodeEnvelope(s.schemaVersion, j.dump());
}

std::string migrateSequencePayload(const std::string& payloadJson,
                                   std::uint32_t fromVersion) {
    if (fromVersion >= schema::SEQUENCE_VERSION) return payloadJson;
    // v0 -> v1: 旧格式无 schemaVersion / updatedAt，补齐。
    json j = json::parse(payloadJson);
    if (!j.contains("schemaVersion")) j["schemaVersion"] = schema::SEQUENCE_VERSION;
    if (!j.contains("updatedAt"))     j["updatedAt"] = 0;
    return j.dump();
}

SequenceState decodeSequence(const std::string& bytes) {
    EnvelopeHeader h = parseEnvelopeHeader(bytes);
    if (h.schemaVersion > schema::SEQUENCE_VERSION) {
        throw StateDecodeError("sequence unknown newer schemaVersion: " +
                               std::to_string(h.schemaVersion));
    }
    std::string payload = extractPayload(bytes, h);
    try {
        std::uint32_t v = h.schemaVersion;
        while (v < schema::SEQUENCE_VERSION) {
            payload = migrateSequencePayload(payload, v);
            ++v;
        }
        json j = json::parse(payload);
        SequenceState s;
        s.schemaVersion = j.value("schemaVersion", schema::SEQUENCE_VERSION);
        if (s.schemaVersion != schema::SEQUENCE_VERSION) {
            throw StateDecodeError("sequence schemaVersion mismatch after migration");
        }
        s.highestAllocated = j.at("highestAllocated").get<std::uint64_t>();
        s.updatedAt = j.value("updatedAt", static_cast<Timestamp>(0));
        return s;
    } catch (const StateDecodeError&) {
        throw;
    } catch (const json::exception& e) {
        throw StateDecodeError(std::string("sequence decode failed: ") + e.what());
    }
}

// ===========================================================================
// LastSuccessState
// ===========================================================================
std::string encodeLastSuccess(const LastSuccessState& s) {
    json j = json{{"schemaVersion", s.schemaVersion},
                  {"reportId", s.reportId},
                  {"snapshotSeq", s.snapshotSeq},
                  {"completedAt", s.completedAt},
                  {"registryVersion", s.registryVersion},
                  {"overallResult", collectionStatusToString(s.overallResult)},
                  {"fingerprints", persistedFingerprintsToJson(s.fingerprints)},
                  {"snapshot", snapshotToJson(s.snapshot)}};
    return encodeEnvelope(s.schemaVersion, j.dump());
}

std::string migrateLastSuccessPayload(const std::string& payloadJson,
                                      std::uint32_t fromVersion) {
    if (fromVersion >= schema::LAST_SUCCESS_VERSION) return payloadJson;
    json j = json::parse(payloadJson);
    if (fromVersion == 0) {
        // v0 -> v1: 旧格式无 schemaVersion，补齐。
        if (!j.contains("schemaVersion")) j["schemaVersion"] = static_cast<std::uint32_t>(1);
        if (!j.contains("fingerprint"))   j["fingerprint"] = "";
    } else if (fromVersion == 1) {
        // v1 -> v2 (CCU-FOTA-DSN-CR-006): 弃用旧 fingerprint 占位串，新增 fingerprints{}
        // （未知/不兼容，触发首次完整采集建立 v1 指纹）。
        j.erase("fingerprint");
        if (!j.contains("fingerprints")) {
            j["fingerprints"] = json{{"algorithm", ""},
                                     {"canonicalization", ""},
                                     {"versionFingerprintHex", ""},
                                     {"snapshotFingerprintHex", ""},
                                     {"dedupeKeyHex", ""}};
        }
        j["schemaVersion"] = schema::LAST_SUCCESS_VERSION;
    }
    return j.dump();
}

LastSuccessState decodeLastSuccess(const std::string& bytes) {
    EnvelopeHeader h = parseEnvelopeHeader(bytes);
    if (h.schemaVersion > schema::LAST_SUCCESS_VERSION) {
        throw StateDecodeError("last_success unknown newer schemaVersion: " +
                               std::to_string(h.schemaVersion));
    }
    std::string payload = extractPayload(bytes, h);
    try {
        std::uint32_t v = h.schemaVersion;
        while (v < schema::LAST_SUCCESS_VERSION) {
            payload = migrateLastSuccessPayload(payload, v);
            ++v;
        }
        json j = json::parse(payload);
        LastSuccessState s;
        s.schemaVersion = j.value("schemaVersion", schema::LAST_SUCCESS_VERSION);
        if (s.schemaVersion != schema::LAST_SUCCESS_VERSION) {
            throw StateDecodeError("last_success schemaVersion mismatch after migration");
        }
        s.reportId        = j.at("reportId").get<std::string>();
        s.snapshotSeq     = j.at("snapshotSeq").get<std::uint64_t>();
        s.completedAt     = j.at("completedAt").get<Timestamp>();
        s.registryVersion = j.at("registryVersion").get<std::string>();
        s.overallResult   = overallResultFromString(j.at("overallResult").get<std::string>());
        s.fingerprints    = persistedFingerprintsFromJson(j.value("fingerprints", json::object()));
        validatePersistedFingerprints(s.fingerprints, "last_success");
        s.snapshot        = snapshotFromJson(j.at("snapshot"));
        // 不变量：序号一致
        if (s.snapshot.snapshot_seq != s.snapshotSeq) {
            throw StateDecodeError("last_success snapshotSeq mismatch");
        }
        return s;
    } catch (const StateDecodeError&) {
        throw;
    } catch (const json::exception& e) {
        throw StateDecodeError(std::string("last_success decode failed: ") + e.what());
    }
}

// ===========================================================================
// DedupeState
// ===========================================================================
std::string encodeDedupe(const DedupeState& s) {
    json j = json{{"schemaVersion", s.schemaVersion},
                  {"entries", json::array()},
                  {"maxEntries", s.maxEntries},
                  {"ttlMs", s.ttlMs}};
    for (const auto& e : s.entries) {
        j["entries"].push_back(dedupeEntryToJson(e));
    }
    return encodeEnvelope(s.schemaVersion, j.dump());
}

std::string migrateDedupePayload(const std::string& payloadJson,
                                 std::uint32_t fromVersion) {
    if (fromVersion >= schema::DEDUPE_VERSION) return payloadJson;
    json j = json::parse(payloadJson);
    if (fromVersion == 0) {
        // v0 -> v1: 旧格式用 maxSize，重命名为 maxEntries，并补 ttlMs。
        if (!j.contains("schemaVersion")) j["schemaVersion"] = static_cast<std::uint32_t>(1);
        if (j.contains("maxSize") && !j.contains("maxEntries")) {
            j["maxEntries"] = j["maxSize"];
            j.erase("maxSize");
        }
        if (!j.contains("ttlMs")) j["ttlMs"] = 0;
    } else if (fromVersion == 1) {
        // v1 -> v2 (CCU-FOTA-DSN-CR-006): 每个条目弃用 fingerprint，新增 fingerprints{}。
        if (!j.contains("schemaVersion")) j["schemaVersion"] = schema::DEDUPE_VERSION;
        if (j.contains("entries") && j["entries"].is_array()) {
            for (auto& ej : j["entries"]) {
                ej.erase("fingerprint");
                if (!ej.contains("fingerprints")) {
                    ej["fingerprints"] = json{{"algorithm", ""},
                                              {"canonicalization", ""},
                                              {"versionFingerprintHex", ""},
                                              {"snapshotFingerprintHex", ""},
                                              {"dedupeKeyHex", ""}};
                }
            }
        }
        j["schemaVersion"] = schema::DEDUPE_VERSION;
    }
    return j.dump();
}

DedupeState decodeDedupe(const std::string& bytes) {
    EnvelopeHeader h = parseEnvelopeHeader(bytes);
    if (h.schemaVersion > schema::DEDUPE_VERSION) {
        throw StateDecodeError("dedupe unknown newer schemaVersion: " +
                               std::to_string(h.schemaVersion));
    }
    std::string payload = extractPayload(bytes, h);
    try {
        std::uint32_t v = h.schemaVersion;
        while (v < schema::DEDUPE_VERSION) {
            payload = migrateDedupePayload(payload, v);
            ++v;
        }
        json j = json::parse(payload);
        DedupeState s;
        s.schemaVersion = j.value("schemaVersion", schema::DEDUPE_VERSION);
        if (s.schemaVersion != schema::DEDUPE_VERSION) {
            throw StateDecodeError("dedupe schemaVersion mismatch after migration");
        }
        for (const auto& ej : j.at("entries")) {
            s.entries.push_back(dedupeEntryFromJson(ej));
        }
        s.maxEntries = j.value("maxEntries", static_cast<std::uint32_t>(0));
        s.ttlMs     = j.value("ttlMs", static_cast<std::int64_t>(0));
        return s;
    } catch (const StateDecodeError&) {
        throw;
    } catch (const json::exception& e) {
        throw StateDecodeError(std::string("dedupe decode failed: ") + e.what());
    }
}

// ===========================================================================
// ActiveJobState
// ===========================================================================
std::string encodeActiveJob(const ActiveJobState& s) {
    json j = json{{"schemaVersion", s.schemaVersion},
                  {"requestId", s.requestId},
                  {"reportId", s.reportId},
                  {"snapshotSeq", s.snapshotSeq},
                  {"reason", triggerReasonToString(s.reason)},
                  {"phase", jobPhaseToString(s.phase)},
                  {"attempt", s.attempt},
                  {"nextRetryAt", s.nextRetryAt},
                  {"lastErrorCode", s.lastErrorCode},
                  {"idempotencyKey", s.idempotencyKey},
                  {"fingerprints", persistedFingerprintsToJson(s.fingerprints)}};
    return encodeEnvelope(s.schemaVersion, j.dump());
}

std::string migrateActiveJobPayload(const std::string& payloadJson,
                                    std::uint32_t fromVersion) {
    if (fromVersion >= schema::ACTIVE_JOB_VERSION) return payloadJson;
    json j = json::parse(payloadJson);
    if (fromVersion == 0) {
        // v0 -> v1: 旧格式无 idempotencyKey，补齐空串。
        if (!j.contains("schemaVersion")) j["schemaVersion"] = static_cast<std::uint32_t>(1);
        if (!j.contains("idempotencyKey")) j["idempotencyKey"] = "";
    } else if (fromVersion == 1) {
        // v1 -> v2 (CCU-FOTA-DSN-CR-006): 新增 fingerprints{}（未知）。
        if (!j.contains("schemaVersion")) j["schemaVersion"] = schema::ACTIVE_JOB_VERSION;
        if (!j.contains("fingerprints")) {
            j["fingerprints"] = json{{"algorithm", ""},
                                     {"canonicalization", ""},
                                     {"versionFingerprintHex", ""},
                                     {"snapshotFingerprintHex", ""},
                                     {"dedupeKeyHex", ""}};
        }
        j["schemaVersion"] = schema::ACTIVE_JOB_VERSION;
    }
    return j.dump();
}

ActiveJobState decodeActiveJob(const std::string& bytes) {
    EnvelopeHeader h = parseEnvelopeHeader(bytes);
    if (h.schemaVersion > schema::ACTIVE_JOB_VERSION) {
        throw StateDecodeError("active_job unknown newer schemaVersion: " +
                               std::to_string(h.schemaVersion));
    }
    std::string payload = extractPayload(bytes, h);
    try {
        std::uint32_t v = h.schemaVersion;
        while (v < schema::ACTIVE_JOB_VERSION) {
            payload = migrateActiveJobPayload(payload, v);
            ++v;
        }
        json j = json::parse(payload);
        ActiveJobState s;
        s.schemaVersion = j.value("schemaVersion", schema::ACTIVE_JOB_VERSION);
        if (s.schemaVersion != schema::ACTIVE_JOB_VERSION) {
            throw StateDecodeError("active_job schemaVersion mismatch after migration");
        }
        s.requestId      = j.at("requestId").get<std::string>();
        s.reportId       = j.at("reportId").get<std::string>();
        s.snapshotSeq    = j.at("snapshotSeq").get<std::uint64_t>();
        {
            std::string r = j.at("reason").get<std::string>();
            if (!triggerReasonFromString(r, s.reason)) {
                throw StateDecodeError("active_job bad reason: " + r);
            }
        }
        {
            std::string p = j.at("phase").get<std::string>();
            if (!jobPhaseFromString(p, s.phase)) {
                throw StateDecodeError("active_job bad phase: " + p);
            }
        }
        s.attempt        = j.at("attempt").get<std::uint32_t>();
        s.nextRetryAt    = j.at("nextRetryAt").get<Timestamp>();
        s.lastErrorCode  = j.at("lastErrorCode").get<std::string>();
        s.idempotencyKey = j.value("idempotencyKey", std::string());
        s.fingerprints   = persistedFingerprintsFromJson(j.value("fingerprints", json::object()));
        validatePersistedFingerprints(s.fingerprints, "active_job");
        return s;
    } catch (const StateDecodeError&) {
        throw;
    } catch (const json::exception& e) {
        throw StateDecodeError(std::string("active_job decode failed: ") + e.what());
    }
}

} // namespace store
} // namespace ccu_fota
