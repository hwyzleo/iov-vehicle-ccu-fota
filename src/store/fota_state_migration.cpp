// =============================================================================
// src/store/fota_state_migration.cpp
// CCU-FOTA 旧 ota.* 状态迁移器实现 (CCU-FOTA-DSN-CR-011 §Store 迁移流程)
// =============================================================================

#include "ccu/fota/store/fota_state_migration.hpp"

#include "ccu/fota/store/fota_cloud_state.hpp"
#include "ccu/fota/store/fota_cloud_state_serializer.hpp"
#include "ccu/fota/store/fota_state_serializer.hpp"  // FTSE envelope 复用

#include <nlohmann/json.hpp>

#include "store_types.h"  // StoreException

#include <chrono>
#include <cstdint>
#include <cstdlib>  // strtoull
#include <cerrno>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ccu_fota {
namespace store {

namespace {

using json = nlohmann::json;
using ccu_fota::store::fota::FotaConsentRecord;
using ccu_fota::store::fota::FotaControlRecord;
using ccu_fota::store::fota::FotaDownloadRecord;
using ccu_fota::store::fota::FotaEventOutboxMeta;
using ccu_fota::store::fota::FotaEventOutboxRecord;
using ccu_fota::store::fota::FotaExecutionRecord;
using ccu_fota::store::fota::FotaInventoryRecord;
using ccu_fota::store::fota::FotaLogJobRecord;
using ccu_fota::store::fota::FotaPolicyRecord;
using ccu_fota::store::fota::FotaVehicleTaskRecord;
using ccu_fota::store::fota::Timestamp;

// ============================================================
// 旧 key 清单（vehicle.ota.v1 时代）
// ============================================================
constexpr const char* OLD_VEHICLE_TASK = "ota.vehicle_task";
constexpr const char* OLD_INVENTORY    = "ota.inventory";
constexpr const char* OLD_CONSENT      = "ota.consent";
constexpr const char* OLD_DOWNLOADS    = "ota.downloads";
constexpr const char* OLD_EXECUTION    = "ota.execution";
constexpr const char* OLD_EVENT_OUTBOX = "ota.event_outbox";
constexpr const char* OLD_CONTROLS     = "ota.controls";
constexpr const char* OLD_POLICY       = "ota.policy";
constexpr const char* OLD_LOG_JOBS     = "ota.log_jobs";

constexpr const char* kMarkerKey = "fota.migration_marker";

// ============================================================
// 旧 envelope 解码（与旧 ota_state_serializer 一致的 FTSE+JSON）
// ============================================================
json decodeLegacyEnvelopeJson(const std::string& bytes, std::uint32_t expectedVersion,
                              const char* tag) {
    EnvelopeHeader h = parseEnvelopeHeader(bytes);
    if (h.schemaVersion > expectedVersion) {
        throw StateDecodeError(std::string(tag) + " unknown newer schemaVersion: " +
                               std::to_string(h.schemaVersion));
    }
    std::string payload = extractPayload(bytes, h);
    if (h.schemaVersion < expectedVersion) {
        throw StateDecodeError(std::string(tag) + " unsupported older schemaVersion: " +
                               std::to_string(h.schemaVersion));
    }
    try {
        return json::parse(payload);
    } catch (const json::exception& e) {
        throw StateDecodeError(std::string(tag) + " json parse failed: " + e.what());
    }
}

std::string getStr(const json& j, const char* key) {
    return j.at(key).get<std::string>();
}
std::string getStrOpt(const json& j, const char* key, const char* dflt) {
    return j.value(key, std::string(dflt));
}

// ============================================================
// 旧 revision 字符串 -> uint64（新契约 task/control/inventory revision 为 uint64）
// 数值字符串直接用；否则用 FNV-1a 稳定哈希（确定性、可重复迁移）。
// ============================================================
std::uint64_t revisionToUint64(const std::string& s) {
    if (!s.empty()) {
        char* end = nullptr;
        errno = 0;
        unsigned long long v = std::strtoull(s.c_str(), &end, 10);
        if (errno == 0 && end != nullptr && *end == '\0') {
            return static_cast<std::uint64_t>(v);
        }
    }
    // FNV-1a 64-bit
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// ============================================================
// 旧记录 JSON 解码（镜像旧 ota_state 结构）
// ============================================================
struct LegacyVehicleTask {
    std::string vehicleTaskId;
    std::string taskRevision;
    std::string targetBaselineId;
    std::string vehicleTaskState;
    Timestamp frozenAtMs = 0;
    std::string frozenSnapshotPbHex;
    std::string localDispositionResult;
    bool superseded = false;
};
LegacyVehicleTask decodeLegacyVehicleTask(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "vehicle_task");
    LegacyVehicleTask r;
    r.vehicleTaskId = getStr(j, "vehicleTaskId");
    r.taskRevision = getStr(j, "taskRevision");
    r.targetBaselineId = getStr(j, "targetBaselineId");
    r.vehicleTaskState = getStr(j, "vehicleTaskState");
    r.frozenAtMs = j.at("frozenAtMs").get<Timestamp>();
    r.frozenSnapshotPbHex = getStrOpt(j, "frozenSnapshotPbHex", "");
    r.localDispositionResult = getStrOpt(j, "localDispositionResult", "");
    r.superseded = j.value("superseded", false);
    return r;
}

struct LegacyInventory {
    std::string mode;
    std::string inventoryRevision;
    std::string algorithm;
    std::string ecuListDigest;
    Timestamp collectedAtMs = 0;
    std::string inventoryPbHex;
    bool fullRequired = false;
};
LegacyInventory decodeLegacyInventory(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "inventory");
    LegacyInventory r;
    r.mode = getStr(j, "mode");
    r.inventoryRevision = getStr(j, "inventoryRevision");
    r.algorithm = getStr(j, "algorithm");
    r.ecuListDigest = getStr(j, "ecuListDigest");
    r.collectedAtMs = j.at("collectedAtMs").get<Timestamp>();
    r.inventoryPbHex = getStrOpt(j, "inventoryPbHex", "");
    r.fullRequired = j.value("fullRequired", false);
    return r;
}

struct LegacyConsent {
    std::string vehicleTaskId;
    std::string effectiveStatus;
    std::string receiptId;
    Timestamp receiptExpiresAtMs = 0;
    std::string consentReceiptPbHex;
    std::string termsId;
    std::string termsVersion;
};
LegacyConsent decodeLegacyConsent(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "consent");
    LegacyConsent r;
    r.vehicleTaskId = getStr(j, "vehicleTaskId");
    r.effectiveStatus = getStr(j, "effectiveStatus");
    r.receiptId = getStr(j, "receiptId");
    r.receiptExpiresAtMs = j.at("receiptExpiresAtMs").get<Timestamp>();
    r.consentReceiptPbHex = getStrOpt(j, "consentReceiptPbHex", "");
    r.termsId = getStrOpt(j, "termsId", "");
    r.termsVersion = getStrOpt(j, "termsVersion", "");
    return r;
}

struct LegacyDownloadEntry {
    std::string packageId;
    std::string packageRevision;
    std::string etag;
    std::int64_t offset = 0;
    Timestamp credentialExpiresAtMs = 0;
    std::string verifyStatus;
    std::string stageResultId;
    std::string stageResultDigest;
    bool ready = false;
};
LegacyDownloadEntry downloadEntryFromJson(const json& j) {
    LegacyDownloadEntry e;
    e.packageId = j.at("packageId").get<std::string>();
    e.packageRevision = j.at("packageRevision").get<std::string>();
    e.etag = j.at("etag").get<std::string>();
    e.offset = j.at("offset").get<std::int64_t>();
    e.credentialExpiresAtMs = j.value("credentialExpiresAtMs", Timestamp(0));
    e.verifyStatus = j.at("verifyStatus").get<std::string>();
    e.stageResultId = j.at("stageResultId").get<std::string>();
    e.stageResultDigest = j.at("stageResultDigest").get<std::string>();
    e.ready = j.value("ready", false);
    return e;
}
struct LegacyDownloads {
    std::vector<LegacyDownloadEntry> entries;
};
LegacyDownloads decodeLegacyDownloads(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "downloads");
    LegacyDownloads r;
    for (const auto& ej : j.at("entries")) r.entries.push_back(downloadEntryFromJson(ej));
    return r;
}

struct LegacyExecution {
    std::string vehicleTaskId;
    std::string executionId;
    std::uint32_t attemptNo = 0;
    std::string permitId;
    std::string permitToken;
    std::string controlRevision;
    Timestamp validUntilMs = 0;
    std::string executionState;
    std::string installPlanVersion;
    std::string offlinePolicyPbHex;
    std::string timeoutPolicyPbHex;
    std::uint64_t acceptedSequenceNo = 0;
    std::uint64_t nextSequenceNo = 1;
};
LegacyExecution decodeLegacyExecution(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "execution");
    LegacyExecution r;
    r.vehicleTaskId = getStr(j, "vehicleTaskId");
    r.executionId = getStr(j, "executionId");
    r.attemptNo = j.at("attemptNo").get<std::uint32_t>();
    r.permitId = getStr(j, "permitId");
    r.permitToken = getStr(j, "permitToken");
    r.controlRevision = getStr(j, "controlRevision");
    r.validUntilMs = j.at("validUntilMs").get<Timestamp>();
    r.executionState = getStr(j, "executionState");
    r.installPlanVersion = getStrOpt(j, "installPlanVersion", "");
    r.offlinePolicyPbHex = getStrOpt(j, "offlinePolicyPbHex", "");
    r.timeoutPolicyPbHex = getStrOpt(j, "timeoutPolicyPbHex", "");
    r.acceptedSequenceNo = j.at("acceptedSequenceNo").get<std::uint64_t>();
    r.nextSequenceNo = j.at("nextSequenceNo").get<std::uint64_t>();
    return r;
}

struct LegacyEventOutboxEntry {
    std::uint64_t sequenceNo = 0;
    std::string eventId;
    std::string eventDigest;
    std::string stage;
    std::uint32_t progressPercent = 0;
    std::string result;
    Timestamp timestampMs = 0;
    std::string payloadSummary;
    std::string sendStatus;
    std::string eventPbHex;
};
LegacyEventOutboxEntry eventEntryFromJson(const json& j) {
    LegacyEventOutboxEntry e;
    e.sequenceNo = j.at("sequenceNo").get<std::uint64_t>();
    e.eventId = j.at("eventId").get<std::string>();
    e.eventDigest = j.at("eventDigest").get<std::string>();
    e.stage = j.at("stage").get<std::string>();
    e.progressPercent = j.value("progressPercent", std::uint32_t(0));
    e.result = j.at("result").get<std::string>();
    e.timestampMs = j.at("timestampMs").get<Timestamp>();
    e.payloadSummary = j.at("payloadSummary").get<std::string>();
    e.sendStatus = j.at("sendStatus").get<std::string>();
    e.eventPbHex = j.at("eventPbHex").get<std::string>();
    return e;
}
struct LegacyEventOutbox {
    std::uint64_t nextSequenceNo = 1;
    std::uint64_t acceptedSequenceNo = 0;
    std::vector<LegacyEventOutboxEntry> entries;
};
LegacyEventOutbox decodeLegacyEventOutbox(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "event_outbox");
    LegacyEventOutbox r;
    r.nextSequenceNo = j.at("nextSequenceNo").get<std::uint64_t>();
    r.acceptedSequenceNo = j.at("acceptedSequenceNo").get<std::uint64_t>();
    for (const auto& ej : j.at("entries")) r.entries.push_back(eventEntryFromJson(ej));
    return r;
}

struct LegacyControlEntry {
    std::string controlRevision;
    std::string commandType;
    std::string applyMode;
    Timestamp expiresAtMs = 0;
    std::string reason;
    std::string ackStatus;
    std::uint64_t ackSequenceNo = 0;
    Timestamp appliedAtMs = 0;
};
LegacyControlEntry controlEntryFromJson(const json& j) {
    LegacyControlEntry e;
    e.controlRevision = j.at("controlRevision").get<std::string>();
    e.commandType = j.at("commandType").get<std::string>();
    e.applyMode = j.at("applyMode").get<std::string>();
    e.expiresAtMs = j.at("expiresAtMs").get<Timestamp>();
    e.reason = j.at("reason").get<std::string>();
    e.ackStatus = j.at("ackStatus").get<std::string>();
    e.ackSequenceNo = j.at("ackSequenceNo").get<std::uint64_t>();
    e.appliedAtMs = j.at("appliedAtMs").get<Timestamp>();
    return e;
}
struct LegacyControls {
    std::vector<LegacyControlEntry> entries;
};
LegacyControls decodeLegacyControls(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "controls");
    LegacyControls r;
    for (const auto& ej : j.at("entries")) r.entries.push_back(controlEntryFromJson(ej));
    return r;
}

struct LegacyPolicy {
    std::string basePreferenceVersion;
    std::string preferenceVersion;
    bool conflict = false;
};
LegacyPolicy decodeLegacyPolicy(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "policy");
    LegacyPolicy r;
    r.basePreferenceVersion = getStr(j, "basePreferenceVersion");
    r.preferenceVersion = getStr(j, "preferenceVersion");
    r.conflict = j.value("conflict", false);
    return r;
}

struct LegacyLogJobEntry {
    std::string logRequestId;
    std::string objectKey;
    std::string digestHex;
    std::int64_t sizeBytes = 0;
    std::string status;
    Timestamp completedAtMs = 0;
};
LegacyLogJobEntry logJobEntryFromJson(const json& j) {
    LegacyLogJobEntry e;
    e.logRequestId = j.at("logRequestId").get<std::string>();
    e.objectKey = j.at("objectKey").get<std::string>();
    e.digestHex = j.at("digestHex").get<std::string>();
    e.sizeBytes = j.at("sizeBytes").get<std::int64_t>();
    e.status = j.at("status").get<std::string>();
    e.completedAtMs = j.at("completedAtMs").get<Timestamp>();
    return e;
}
struct LegacyLogJobs {
    std::vector<LegacyLogJobEntry> entries;
};
LegacyLogJobs decodeLegacyLogJobs(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "log_jobs");
    LegacyLogJobs r;
    for (const auto& ej : j.at("entries")) r.entries.push_back(logJobEntryFromJson(ej));
    return r;
}

// ============================================================
// migration marker（FTSE envelope + JSON）
// ============================================================
struct MigrationMarker {
    std::uint32_t schemaVersion = 1;
    bool completed = false;
    std::uint32_t migratedKeyCount = 0;
    Timestamp migratedAtMs = 0;
};
std::string encodeMarker(const MigrationMarker& m) {
    json j = json{{"schemaVersion", m.schemaVersion},
                  {"completed", m.completed},
                  {"migratedKeyCount", m.migratedKeyCount},
                  {"migratedAtMs", m.migratedAtMs}};
    return encodeEnvelope(m.schemaVersion, j.dump());
}
MigrationMarker decodeMarker(const std::string& bytes) {
    json j = decodeLegacyEnvelopeJson(bytes, 1, "migration_marker");
    MigrationMarker m;
    m.schemaVersion = j.value("schemaVersion", 1u);
    m.completed = j.value("completed", false);
    m.migratedKeyCount = j.value("migratedKeyCount", 0u);
    m.migratedAtMs = j.value("migratedAtMs", Timestamp(0));
    return m;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// 旧 commandType / applyMode 名称映射到新契约名称。
std::string mapControlAction(const std::string& old) {
    if (old == "RESUME") return "CONTINUE";
    if (old == "PAUSE") return "PAUSE";
    if (old == "ABORT") return "ABORT";
    if (old == "ROLLBACK") return "ROLLBACK";
    if (old == "RESYNC") return "RESYNC";
    return old;
}
std::string mapApplyMode(const std::string& old) {
    if (old == "IMMEDIATE") return "IMMEDIATE_IF_SAFE";
    return old;
}

} // namespace

// ===========================================================================
// migrateOtaToFota
// ===========================================================================
FotaMigrationResult migrateOtaToFota(ccu::fw::store::Store& store) {
    using ccu::fw::store::Store;
    FotaMigrationResult out;

    const std::vector<std::string> oldKeys = {
        OLD_VEHICLE_TASK, OLD_INVENTORY, OLD_CONSENT, OLD_DOWNLOADS,
        OLD_EXECUTION, OLD_EVENT_OUTBOX, OLD_CONTROLS, OLD_POLICY, OLD_LOG_JOBS};

    // 1. 检测旧 key。
    bool anyOld = false;
    for (const auto& k : oldKeys) {
        if (store.has(k)) { anyOld = true; break; }
    }
    if (!anyOld) return out;  // {ran=false, completed=true}

    out.ran = true;

    // 若 marker 已 completed：仅做遗留清理（中断于清理阶段）并返回。
    bool markerDone = false;
    if (store.has(kMarkerKey)) {
        auto m = decodeMarker(store.load<std::string>(kMarkerKey));
        markerDone = m.completed;
    }
    if (markerDone) {
        std::size_t cleaned = 0;
        for (const auto& k : oldKeys) {
            if (store.remove(k)) ++cleaned;
        }
        out.completed = true;
        out.migratedKeys = cleaned;
        out.detail = "marker already completed; cleaned leftover ota.* keys";
        return out;
    }

    // 2. 迁移（读取旧 -> 写入新；任何失败抛异常，旧 key 保留）。
    std::size_t written = 0;

    if (store.has(OLD_VEHICLE_TASK)) {
        auto old = decodeLegacyVehicleTask(store.load<std::string>(OLD_VEHICLE_TASK));
        FotaVehicleTaskRecord r;
        r.vehicleTaskId = old.vehicleTaskId;
        r.taskRevision = revisionToUint64(old.taskRevision);
        r.targetBaselineCode = old.targetBaselineId;
        r.vehicleTaskState = old.vehicleTaskState;
        r.frozenAtMs = old.frozenAtMs;
        // 旧 FrozenTaskSnapshot proto 不随新契约保留（opaque 载荷不迁移）
        r.taskSnapshotPbHex.clear();
        r.localDispositionResult = old.localDispositionResult;
        r.superseded = old.superseded;
        store.save<std::string>(fota::keys::VEHICLE_TASK,
                                fota::encodeVehicleTask(r));
        ++written;
    }

    if (store.has(OLD_INVENTORY)) {
        auto old = decodeLegacyInventory(store.load<std::string>(OLD_INVENTORY));
        FotaInventoryRecord r;
        r.mode = old.mode;
        r.inventoryRevision = revisionToUint64(old.inventoryRevision);
        r.algorithm = old.algorithm;
        r.ecuListDigestHex = old.ecuListDigest;
        r.baselineCode.clear();  // baseline 在旧 InventoryInfo proto 内，不随新契约迁移
        r.fotaMasterVersion.clear();
        r.collectedAtMs = old.collectedAtMs;
        r.fullRequired = old.fullRequired;
        store.save<std::string>(fota::keys::INVENTORY, fota::encodeInventory(r));
        ++written;
    }

    if (store.has(OLD_CONSENT)) {
        auto old = decodeLegacyConsent(store.load<std::string>(OLD_CONSENT));
        FotaConsentRecord r;
        r.vehicleTaskId = old.vehicleTaskId;
        r.effectiveStatus = old.effectiveStatus;
        r.receiptId = old.receiptId;
        r.receiptExpiresAtMs = old.receiptExpiresAtMs;
        r.termsId = old.termsId;
        r.termsVersion = old.termsVersion;
        r.consentTimeMs = 0;
        r.channel.clear();
        store.save<std::string>(fota::keys::CONSENT, fota::encodeConsent(r));
        ++written;
    }

    if (store.has(OLD_DOWNLOADS)) {
        auto old = decodeLegacyDownloads(store.load<std::string>(OLD_DOWNLOADS));
        for (const auto& e : old.entries) {
            FotaDownloadRecord r;
            r.packageId = e.packageId;
            r.packageRevision = e.packageRevision;
            r.etag = e.etag;
            r.currentOffsetBytes = e.offset > 0 ? static_cast<std::uint64_t>(e.offset) : 0;
            r.offsetScope = "STORED_OBJECT";
            r.credentialExpiresAtMs = e.credentialExpiresAtMs;
            r.verifyStatus = e.verifyStatus;
            r.stageResultId = e.stageResultId;
            r.stageResultDigestHex = e.stageResultDigest;
            r.ready = e.ready;
            store.save<std::string>(fota::keys::downloadKey(e.packageId),
                                    fota::encodeDownload(r));
            ++written;
        }
    }

    // execution 水位 -> fota.event_outbox_meta；结构字段 -> fota.execution
    FotaEventOutboxMeta meta;  // 默认 next=1, accepted=0
    if (store.has(OLD_EXECUTION)) {
        auto old = decodeLegacyExecution(store.load<std::string>(OLD_EXECUTION));
        FotaExecutionRecord r;
        r.vehicleTaskId = old.vehicleTaskId;
        r.executionId = old.executionId;
        r.attemptNo = old.attemptNo;
        r.permitId = old.permitId;
        r.permitToken = old.permitToken;
        r.controlRevision = revisionToUint64(old.controlRevision);
        r.validUntilMs = old.validUntilMs;
        r.executionState = old.executionState;
        r.installPlanVersion = old.installPlanVersion;
        // 旧 TaskPolicy proto 不随新契约保留（opaque 载荷不迁移）
        r.checkpointPbHex.clear();
        r.offlinePolicyPbHex.clear();
        r.timeoutPolicyPbHex.clear();
        store.save<std::string>(fota::keys::EXECUTION, fota::encodeExecution(r));
        ++written;
        meta.nextSequenceNo = old.nextSequenceNo;
        meta.acceptedSequenceNo = old.acceptedSequenceNo;
    }

    if (store.has(OLD_EVENT_OUTBOX)) {
        auto old = decodeLegacyEventOutbox(store.load<std::string>(OLD_EVENT_OUTBOX));
        meta.nextSequenceNo = old.nextSequenceNo;
        meta.acceptedSequenceNo = old.acceptedSequenceNo;
        for (const auto& e : old.entries) {
            FotaEventOutboxRecord r;
            r.sequenceNo = e.sequenceNo;
            r.eventId = e.eventId;
            r.eventDigestHex = e.eventDigest;
            r.stage = e.stage;
            r.eventStatus = e.result;
            r.progress = e.progressPercent;
            r.occurredAtMs = e.timestampMs;
            r.payloadSummary = e.payloadSummary;
            r.sendStatus = e.sendStatus;
            r.eventPbHex.clear();  // 旧 ExecutionEvent proto 不迁移（opaque）
            store.save<std::string>(fota::keys::eventKey(e.sequenceNo),
                                    fota::encodeEventOutbox(r));
            ++written;
        }
    }
    store.save<std::string>(fota::keys::EVENT_OUTBOX_META, fota::encodeEventOutboxMeta(meta));
    ++written;

    if (store.has(OLD_CONTROLS)) {
        auto old = decodeLegacyControls(store.load<std::string>(OLD_CONTROLS));
        for (const auto& e : old.entries) {
            FotaControlRecord r;
            r.controlRevision = revisionToUint64(e.controlRevision);
            r.controlId = "";
            r.action = mapControlAction(e.commandType);
            r.scope = "EXECUTION";
            r.applyMode = mapApplyMode(e.applyMode);
            r.issuedAtMs = 0;
            r.expiresAtMs = e.expiresAtMs;
            r.reason = e.reason;
            r.ackStatus = e.ackStatus;
            r.ackSequenceNo = e.ackSequenceNo;
            r.appliedAtMs = e.appliedAtMs;
            store.save<std::string>(fota::keys::controlKey(r.controlRevision),
                                    fota::encodeControl(r));
            ++written;
        }
    }

    if (store.has(OLD_POLICY)) {
        auto old = decodeLegacyPolicy(store.load<std::string>(OLD_POLICY));
        FotaPolicyRecord r;
        r.localPolicyVersion.clear();
        r.basePreferenceVersion = old.basePreferenceVersion;
        r.preferenceVersion = old.preferenceVersion;
        r.effectivePolicyPbHex.clear();  // 旧 EffectivePolicy proto 不迁移（opaque）
        r.conflict = old.conflict;
        r.effectiveAtMs = 0;
        store.save<std::string>(fota::keys::POLICY, fota::encodePolicy(r));
        ++written;
    }

    if (store.has(OLD_LOG_JOBS)) {
        auto old = decodeLegacyLogJobs(store.load<std::string>(OLD_LOG_JOBS));
        for (const auto& e : old.entries) {
            FotaLogJobRecord r;
            r.logRequestId = e.logRequestId;
            r.objectKey = e.objectKey;
            r.digestHex = e.digestHex;
            r.algorithm = "";
            r.sizeBytes = e.sizeBytes > 0 ? static_cast<std::uint64_t>(e.sizeBytes) : 0;
            r.status = e.status;
            r.completedAtMs = e.completedAtMs;
            store.save<std::string>(fota::keys::logJobKey(e.logRequestId),
                                    fota::encodeLogJob(r));
            ++written;
        }
    }

    // 5. 跨 key 一致性检查：重读所有新 key 并校验可解码（解码失败抛异常 -> fail-closed）。
    {
        if (store.has(fota::keys::VEHICLE_TASK)) fota::decodeVehicleTask(store.load<std::string>(fota::keys::VEHICLE_TASK));
        if (store.has(fota::keys::INVENTORY)) fota::decodeInventory(store.load<std::string>(fota::keys::INVENTORY));
        if (store.has(fota::keys::CONSENT)) fota::decodeConsent(store.load<std::string>(fota::keys::CONSENT));
        if (store.has(fota::keys::EXECUTION)) fota::decodeExecution(store.load<std::string>(fota::keys::EXECUTION));
        if (store.has(fota::keys::EVENT_OUTBOX_META)) fota::decodeEventOutboxMeta(store.load<std::string>(fota::keys::EVENT_OUTBOX_META));
        if (store.has(fota::keys::POLICY)) fota::decodePolicy(store.load<std::string>(fota::keys::POLICY));
    }

    // 6. 写完成 marker。
    MigrationMarker m;
    m.completed = true;
    m.migratedKeyCount = static_cast<std::uint32_t>(written);
    m.migratedAtMs = nowMs();
    store.save<std::string>(kMarkerKey, encodeMarker(m));

    // 6. 清理旧 key（marker 完成之后）。
    std::size_t cleaned = 0;
    for (const auto& k : oldKeys) {
        if (store.remove(k)) ++cleaned;
    }

    out.completed = true;
    out.migratedKeys = cleaned;
    out.detail = "migrated " + std::to_string(written) + " fota.* key(s), cleaned " +
                 std::to_string(cleaned) + " ota.* key(s)";
    return out;
}

} // namespace store
} // namespace ccu_fota
