// =============================================================================
// src/store/fota_cloud_state_serializer.cpp
// CCU-FOTA 车云 FOTA 状态序列化器实现 (CCU-FOTA-DSN-CR-011 §Store)
// =============================================================================

#include "ccu/fota/store/fota_cloud_state_serializer.hpp"
#include "ccu/fota/store/fota_state_serializer.hpp"  // FTSE envelope 复用

#include <nlohmann/json.hpp>

#include <ccu/fw/hash/hex.hpp>  // CCU-FOTA-DSN-CR-006: 可逆 hex 编解码经 framework

#include <stdexcept>
#include <string_view>

namespace ccu_fota {
namespace store {
namespace fota {

using json = nlohmann::json;
using ccu_fota::store::encodeEnvelope;
using ccu_fota::store::parseEnvelopeHeader;
using ccu_fota::store::extractPayload;
using ccu_fota::store::StateDecodeError;

// ===========================================================================
// proto-binary <-> hex
// ---------------------------------------------------------------------------
// CCU-FOTA-DSN-CR-006: FOTA 仓内不实现私有 hex 编码器。proto-binary 的可逆
// hex 编解码经 ccu-framework-hash 通用工具（<ccu/fw/hash/hex.hpp>）。
// ===========================================================================
std::string protoBinaryToHex(const std::string& binary) {
    return ccu::fw::hash::bytesToHex(binary);
}

std::string hexToProtoBinary(const std::string& hex) {
    try {
        return ccu::fw::hash::hexToBytes(hex);
    } catch (const ccu::fw::hash::HexException& e) {
        if (e.position() != std::string_view::npos) {
            throw StateDecodeError(std::string("proto hex decode failed: ") +
                                   e.code() + " at index " +
                                   std::to_string(e.position()));
        }
        throw StateDecodeError(std::string("proto hex decode failed: ") + e.code());
    }
}

namespace {

// 通用 envelope+JSON 解码：解析 envelope、校验 schemaVersion、返回 JSON。
// 未知新版本 fail-closed。
json decodeEnvelopeJson(const std::string& bytes, std::uint32_t expectedVersion,
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

} // namespace

// ===========================================================================
// fota.vehicle_task
// ===========================================================================
std::string encodeVehicleTask(const FotaVehicleTaskRecord& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"vehicleTaskId", r.vehicleTaskId},
        {"taskRevision", r.taskRevision},
        {"targetBaselineCode", r.targetBaselineCode},
        {"vehicleTaskState", r.vehicleTaskState},
        {"frozenAtMs", r.frozenAtMs},
        {"taskSnapshotPbHex", r.taskSnapshotPbHex},
        {"localDispositionResult", r.localDispositionResult},
        {"superseded", r.superseded}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

FotaVehicleTaskRecord decodeVehicleTask(const std::string& bytes) {
    json j = decodeEnvelopeJson(bytes, schema::VEHICLE_TASK_VERSION, "vehicle_task");
    FotaVehicleTaskRecord r;
    r.schemaVersion = j.value("schemaVersion", schema::VEHICLE_TASK_VERSION);
    r.vehicleTaskId = getStr(j, "vehicleTaskId");
    r.taskRevision = j.at("taskRevision").get<std::uint64_t>();
    r.targetBaselineCode = getStr(j, "targetBaselineCode");
    r.vehicleTaskState = getStr(j, "vehicleTaskState");
    r.frozenAtMs = j.at("frozenAtMs").get<Timestamp>();
    r.taskSnapshotPbHex = getStrOpt(j, "taskSnapshotPbHex", "");
    r.localDispositionResult = getStrOpt(j, "localDispositionResult", "");
    r.superseded = j.value("superseded", false);
    return r;
}

// ===========================================================================
// fota.inventory
// ===========================================================================
std::string encodeInventory(const FotaInventoryRecord& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"status", r.status},
        {"mode", r.mode},
        {"inventoryRevision", r.inventoryRevision},
        {"acceptedInventoryRevision", r.acceptedInventoryRevision},
        {"requiredMode", r.requiredMode},
        {"algorithm", r.algorithm},
        {"ecuListDigestHex", r.ecuListDigestHex},
        {"fingerprintCanonicalization", r.fingerprintCanonicalization},
        {"fingerprintHex", r.fingerprintHex},
        {"baselineCode", r.baselineCode},
        {"fotaMasterVersion", r.fotaMasterVersion},
        {"collectedAtMs", r.collectedAtMs},
        {"requestPbHex", r.requestPbHex},
        {"attempt", r.attempt},
        {"nextRetryAtMs", r.nextRetryAtMs},
        {"lastErrorCode", r.lastErrorCode},
        {"digestFailures", r.digestFailures},
        {"fullRequired", r.fullRequired}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

// fota.inventory v1 -> v2 迁移（CCU-FOTA-DSN-CR-012 §迁移）。
// v1 无 status/fingerprint：旧实现只在云端拒绝时写入且 fullRequired=true。
// 迁移后 fingerprintHex 为空 => 身份不可用 => 下一次检测保守 FULL 建基线，
// 但保留已分配 revision（不回退、不清零）。
FotaInventoryRecord decodeInventoryV1(const json& j) {
    FotaInventoryRecord r;
    r.schemaVersion = schema::INVENTORY_VERSION;
    r.inventoryRevision = j.value("inventoryRevision", std::uint64_t{0});
    r.algorithm = getStrOpt(j, "algorithm", "sha-256");
    r.ecuListDigestHex = getStrOpt(j, "ecuListDigestHex", "");
    r.baselineCode = getStrOpt(j, "baselineCode", "");
    r.fotaMasterVersion = getStrOpt(j, "fotaMasterVersion", "");
    r.collectedAtMs = j.value("collectedAtMs", Timestamp(0));
    r.requestPbHex = getStrOpt(j, "requestPbHex", "");
    r.fullRequired = j.value("fullRequired", false);
    r.mode = getStrOpt(j, "mode", r.fullRequired ? "FULL" : "DIGEST");
    r.status = r.fullRequired ? "REQUIRED_FULL" : "PREPARED";
    r.requiredMode = r.fullRequired ? "FULL" : "";
    // fingerprint 留空：v1 无内容指纹，不得伪造。
    return r;
}

FotaInventoryRecord decodeInventory(const std::string& bytes) {
    EnvelopeHeader h = parseEnvelopeHeader(bytes);
    if (h.schemaVersion > schema::INVENTORY_VERSION) {
        throw StateDecodeError("inventory unknown newer schemaVersion: " +
                               std::to_string(h.schemaVersion));
    }
    std::string payload = extractPayload(bytes, h);
    json j;
    try {
        j = json::parse(payload);
    } catch (const json::exception& e) {
        throw StateDecodeError(std::string("inventory json parse failed: ") + e.what());
    }
    if (h.schemaVersion <= 1) return decodeInventoryV1(j);

    FotaInventoryRecord r;
    r.schemaVersion = schema::INVENTORY_VERSION;
    r.status = getStrOpt(j, "status", "PREPARED");
    r.mode = getStr(j, "mode");
    r.inventoryRevision = j.at("inventoryRevision").get<std::uint64_t>();
    r.acceptedInventoryRevision = j.value("acceptedInventoryRevision", std::uint64_t{0});
    r.requiredMode = getStrOpt(j, "requiredMode", "");
    r.algorithm = getStrOpt(j, "algorithm", "sha-256");
    r.ecuListDigestHex = getStrOpt(j, "ecuListDigestHex", "");
    r.fingerprintCanonicalization = getStrOpt(j, "fingerprintCanonicalization", "");
    r.fingerprintHex = getStrOpt(j, "fingerprintHex", "");
    r.baselineCode = getStrOpt(j, "baselineCode", "");
    r.fotaMasterVersion = getStrOpt(j, "fotaMasterVersion", "");
    r.collectedAtMs = j.value("collectedAtMs", Timestamp(0));
    r.requestPbHex = getStrOpt(j, "requestPbHex", "");
    r.attempt = j.value("attempt", std::uint32_t{0});
    r.nextRetryAtMs = j.value("nextRetryAtMs", Timestamp(0));
    r.lastErrorCode = getStrOpt(j, "lastErrorCode", "");
    r.digestFailures = j.value("digestFailures", std::uint32_t{0});
    r.fullRequired = j.value("fullRequired", r.requiredMode == "FULL");
    return r;
}

// ===========================================================================
// fota.consent
// ===========================================================================
std::string encodeConsent(const FotaConsentRecord& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"vehicleTaskId", r.vehicleTaskId},
        {"effectiveStatus", r.effectiveStatus},
        {"receiptId", r.receiptId},
        {"receiptExpiresAtMs", r.receiptExpiresAtMs},
        {"termsId", r.termsId},
        {"termsVersion", r.termsVersion},
        {"termsDigestHex", r.termsDigestHex},
        {"termsAlgorithm", r.termsAlgorithm},
        {"consentTimeMs", r.consentTimeMs},
        {"channel", r.channel}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

FotaConsentRecord decodeConsent(const std::string& bytes) {
    json j = decodeEnvelopeJson(bytes, schema::CONSENT_VERSION, "consent");
    FotaConsentRecord r;
    r.schemaVersion = j.value("schemaVersion", schema::CONSENT_VERSION);
    r.vehicleTaskId = getStr(j, "vehicleTaskId");
    r.effectiveStatus = getStr(j, "effectiveStatus");
    r.receiptId = getStr(j, "receiptId");
    r.receiptExpiresAtMs = j.at("receiptExpiresAtMs").get<Timestamp>();
    r.termsId = getStrOpt(j, "termsId", "");
    r.termsVersion = getStrOpt(j, "termsVersion", "");
    r.termsDigestHex = getStrOpt(j, "termsDigestHex", "");
    r.termsAlgorithm = getStrOpt(j, "termsAlgorithm", "");
    r.consentTimeMs = j.value("consentTimeMs", Timestamp(0));
    r.channel = getStrOpt(j, "channel", "");
    return r;
}

// ===========================================================================
// fota.downloads:<package_id>
// ===========================================================================
std::string encodeDownload(const FotaDownloadRecord& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"packageId", r.packageId},
        {"packageRevision", r.packageRevision},
        {"etag", r.etag},
        {"currentOffsetBytes", r.currentOffsetBytes},
        {"offsetScope", r.offsetScope},
        {"credentialExpiresAtMs", r.credentialExpiresAtMs},
        {"verifyStatus", r.verifyStatus},
        {"stageResultId", r.stageResultId},
        {"stageResultDigestHex", r.stageResultDigestHex},
        {"ready", r.ready}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

FotaDownloadRecord decodeDownload(const std::string& bytes) {
    json j = decodeEnvelopeJson(bytes, schema::DOWNLOADS_VERSION, "download");
    FotaDownloadRecord r;
    r.schemaVersion = j.value("schemaVersion", schema::DOWNLOADS_VERSION);
    r.packageId = getStr(j, "packageId");
    r.packageRevision = getStr(j, "packageRevision");
    r.etag = getStr(j, "etag");
    r.currentOffsetBytes = j.at("currentOffsetBytes").get<std::uint64_t>();
    r.offsetScope = getStrOpt(j, "offsetScope", "");
    r.credentialExpiresAtMs = j.value("credentialExpiresAtMs", Timestamp(0));
    r.verifyStatus = getStr(j, "verifyStatus");
    r.stageResultId = getStrOpt(j, "stageResultId", "");
    r.stageResultDigestHex = getStrOpt(j, "stageResultDigestHex", "");
    r.ready = j.value("ready", false);
    return r;
}

// ===========================================================================
// fota.execution
// ===========================================================================
std::string encodeExecution(const FotaExecutionRecord& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"vehicleTaskId", r.vehicleTaskId},
        {"executionId", r.executionId},
        {"attemptNo", r.attemptNo},
        {"permitId", r.permitId},
        {"permitToken", r.permitToken},
        {"controlRevision", r.controlRevision},
        {"validUntilMs", r.validUntilMs},
        {"executionState", r.executionState},
        {"installPlanVersion", r.installPlanVersion},
        {"checkpointPbHex", r.checkpointPbHex},
        {"offlinePolicyPbHex", r.offlinePolicyPbHex},
        {"timeoutPolicyPbHex", r.timeoutPolicyPbHex}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

FotaExecutionRecord decodeExecution(const std::string& bytes) {
    json j = decodeEnvelopeJson(bytes, schema::EXECUTION_VERSION, "execution");
    FotaExecutionRecord r;
    r.schemaVersion = j.value("schemaVersion", schema::EXECUTION_VERSION);
    r.vehicleTaskId = getStr(j, "vehicleTaskId");
    r.executionId = getStr(j, "executionId");
    r.attemptNo = j.at("attemptNo").get<std::uint32_t>();
    r.permitId = getStr(j, "permitId");
    r.permitToken = getStr(j, "permitToken");
    r.controlRevision = j.at("controlRevision").get<std::uint64_t>();
    r.validUntilMs = j.at("validUntilMs").get<Timestamp>();
    r.executionState = getStr(j, "executionState");
    r.installPlanVersion = getStrOpt(j, "installPlanVersion", "");
    r.checkpointPbHex = getStrOpt(j, "checkpointPbHex", "");
    r.offlinePolicyPbHex = getStrOpt(j, "offlinePolicyPbHex", "");
    r.timeoutPolicyPbHex = getStrOpt(j, "timeoutPolicyPbHex", "");
    return r;
}

// ===========================================================================
// fota.event_outbox_meta
// ===========================================================================
std::string encodeEventOutboxMeta(const FotaEventOutboxMeta& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"nextSequenceNo", r.nextSequenceNo},
        {"acceptedSequenceNo", r.acceptedSequenceNo}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

FotaEventOutboxMeta decodeEventOutboxMeta(const std::string& bytes) {
    json j = decodeEnvelopeJson(bytes, schema::EVENT_OUTBOX_META_VERSION,
                                "event_outbox_meta");
    FotaEventOutboxMeta r;
    r.schemaVersion = j.value("schemaVersion", schema::EVENT_OUTBOX_META_VERSION);
    r.nextSequenceNo = j.at("nextSequenceNo").get<std::uint64_t>();
    r.acceptedSequenceNo = j.at("acceptedSequenceNo").get<std::uint64_t>();
    return r;
}

// ===========================================================================
// fota.event_outbox:<seq>
// ===========================================================================
std::string encodeEventOutbox(const FotaEventOutboxRecord& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"sequenceNo", r.sequenceNo},
        {"eventId", r.eventId},
        {"eventDigestHex", r.eventDigestHex},
        {"stage", r.stage},
        {"eventStatus", r.eventStatus},
        {"progress", r.progress},
        {"occurredAtMs", r.occurredAtMs},
        {"payloadSummary", r.payloadSummary},
        {"sendStatus", r.sendStatus},
        {"eventPbHex", r.eventPbHex}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

FotaEventOutboxRecord decodeEventOutbox(const std::string& bytes) {
    json j = decodeEnvelopeJson(bytes, schema::EVENT_OUTBOX_VERSION, "event_outbox");
    FotaEventOutboxRecord r;
    r.schemaVersion = j.value("schemaVersion", schema::EVENT_OUTBOX_VERSION);
    r.sequenceNo = j.at("sequenceNo").get<std::uint64_t>();
    r.eventId = getStr(j, "eventId");
    r.eventDigestHex = getStrOpt(j, "eventDigestHex", "");
    r.stage = getStrOpt(j, "stage", "");
    r.eventStatus = getStrOpt(j, "eventStatus", "");
    r.progress = j.value("progress", std::uint32_t(0));
    r.occurredAtMs = j.value("occurredAtMs", Timestamp(0));
    r.payloadSummary = getStrOpt(j, "payloadSummary", "");
    r.sendStatus = getStr(j, "sendStatus");
    r.eventPbHex = getStrOpt(j, "eventPbHex", "");
    return r;
}

// ===========================================================================
// fota.controls:<revision>
// ===========================================================================
std::string encodeControl(const FotaControlRecord& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"controlId", r.controlId},
        {"controlRevision", r.controlRevision},
        {"action", r.action},
        {"scope", r.scope},
        {"applyMode", r.applyMode},
        {"issuedAtMs", r.issuedAtMs},
        {"expiresAtMs", r.expiresAtMs},
        {"reason", r.reason},
        {"ackStatus", r.ackStatus},
        {"ackSequenceNo", r.ackSequenceNo},
        {"appliedAtMs", r.appliedAtMs}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

FotaControlRecord decodeControl(const std::string& bytes) {
    json j = decodeEnvelopeJson(bytes, schema::CONTROLS_VERSION, "control");
    FotaControlRecord r;
    r.schemaVersion = j.value("schemaVersion", schema::CONTROLS_VERSION);
    r.controlId = getStrOpt(j, "controlId", "");
    r.controlRevision = j.at("controlRevision").get<std::uint64_t>();
    r.action = getStrOpt(j, "action", "");
    r.scope = getStrOpt(j, "scope", "");
    r.applyMode = getStrOpt(j, "applyMode", "");
    r.issuedAtMs = j.value("issuedAtMs", Timestamp(0));
    r.expiresAtMs = j.value("expiresAtMs", Timestamp(0));
    r.reason = getStrOpt(j, "reason", "");
    r.ackStatus = getStrOpt(j, "ackStatus", "");
    r.ackSequenceNo = j.value("ackSequenceNo", std::uint64_t(0));
    r.appliedAtMs = j.value("appliedAtMs", Timestamp(0));
    return r;
}

// ===========================================================================
// fota.policy
// ===========================================================================
std::string encodePolicy(const FotaPolicyRecord& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"localPolicyVersion", r.localPolicyVersion},
        {"basePreferenceVersion", r.basePreferenceVersion},
        {"preferenceVersion", r.preferenceVersion},
        {"effectivePolicyPbHex", r.effectivePolicyPbHex},
        {"conflict", r.conflict},
        {"effectiveAtMs", r.effectiveAtMs}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

FotaPolicyRecord decodePolicy(const std::string& bytes) {
    json j = decodeEnvelopeJson(bytes, schema::POLICY_VERSION, "policy");
    FotaPolicyRecord r;
    r.schemaVersion = j.value("schemaVersion", schema::POLICY_VERSION);
    r.localPolicyVersion = getStrOpt(j, "localPolicyVersion", "");
    r.basePreferenceVersion = getStr(j, "basePreferenceVersion");
    r.preferenceVersion = getStr(j, "preferenceVersion");
    r.effectivePolicyPbHex = getStrOpt(j, "effectivePolicyPbHex", "");
    r.conflict = j.value("conflict", false);
    r.effectiveAtMs = j.value("effectiveAtMs", Timestamp(0));
    return r;
}

// ===========================================================================
// fota.log_jobs:<log_request_id>
// ===========================================================================
std::string encodeLogJob(const FotaLogJobRecord& r) {
    json j = json{
        {"schemaVersion", r.schemaVersion},
        {"logRequestId", r.logRequestId},
        {"objectKey", r.objectKey},
        {"digestHex", r.digestHex},
        {"algorithm", r.algorithm},
        {"sizeBytes", r.sizeBytes},
        {"status", r.status},
        {"completedAtMs", r.completedAtMs}};
    return encodeEnvelope(r.schemaVersion, j.dump());
}

FotaLogJobRecord decodeLogJob(const std::string& bytes) {
    json j = decodeEnvelopeJson(bytes, schema::LOG_JOBS_VERSION, "log_job");
    FotaLogJobRecord r;
    r.schemaVersion = j.value("schemaVersion", schema::LOG_JOBS_VERSION);
    r.logRequestId = getStr(j, "logRequestId");
    r.objectKey = getStr(j, "objectKey");
    r.digestHex = getStrOpt(j, "digestHex", "");
    r.algorithm = getStrOpt(j, "algorithm", "");
    r.sizeBytes = j.at("sizeBytes").get<std::uint64_t>();
    r.status = getStr(j, "status");
    r.completedAtMs = j.value("completedAtMs", Timestamp(0));
    return r;
}

} // namespace fota
} // namespace store
} // namespace ccu_fota
