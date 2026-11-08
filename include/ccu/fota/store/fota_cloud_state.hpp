#pragma once

// =============================================================================
// include/ccu/fota/store/fota_cloud_state.hpp
// CCU-FOTA 车云 FOTA 持久化状态模型 (CCU-FOTA-DSN-CR-011 §Store)
// =============================================================================
// 定义通过 ccu-framework-store 持久化的车云 FOTA 状态记录。key 从旧 ota.* 迁移
// 至 fota.*；多条目类别（downloads/event_outbox/controls/log_jobs）按 item 拆为
// 独立 key（框架 Store 禁止 '/' 作 key 分隔符，故用 ':' 分隔）：
//   fota.vehicle_task              - 当前任务、taskRevision、基线、状态、任务快照
//   fota.inventory                 - FULL 清单 revision/摘要、baseline、master 版本
//   fota.consent                   - 条款身份、receipt、有效期、权威状态
//   fota.downloads:<package_id>    - 各包 packageRevision/ETag/offset/凭证/校验/stageResult
//   fota.execution                 - execution/attempt/permit/control/冻结策略/checkpoint
//   fota.event_outbox_meta         - 事件水位（next/accepted sequence）
//   fota.event_outbox:<seq>        - 单条事件 ID/摘要/payload/发送状态
//   fota.controls:<revision>       - 单条控制指令/回执序号/状态/原因
//   fota.policy                    - 本地偏好版本/云端版本/有效策略
//   fota.log_jobs:<log_request_id> - 单条日志任务对象键/摘要/上传状态
//
// 复杂 proto 消息（VehicleTaskSnapshot/TaskCheckRequest/InstallCheckpoint/
// OfflinePolicy/TimeoutPolicy/EffectivePolicy/ExecutionEvent）以 proto-binary hex
// 内嵌于 JSON。每条记录含 schemaVersion 与业务摘要；未知新版本 fail-closed。
// 每 item 独立原子写；一致性由严格写入顺序、稳定 taskRevision/executionId/
// sequenceNo/controlRevision/idempotencyKey 与启动对账保证。
// =============================================================================

#include <cstdint>
#include <string>

namespace ccu_fota {
namespace store {
namespace fota {

using Timestamp = std::int64_t;

// ============================================================
// Store keys（框架 Store 禁止 '/'，多条目类别用 ':' 分隔 item 标识）
// ============================================================
namespace keys {
constexpr const char* VEHICLE_TASK     = "fota.vehicle_task";
constexpr const char* INVENTORY        = "fota.inventory";
constexpr const char* CONSENT          = "fota.consent";
constexpr const char* DOWNLOADS_PREFIX = "fota.downloads:";
constexpr const char* EXECUTION        = "fota.execution";
constexpr const char* EVENT_OUTBOX_META   = "fota.event_outbox_meta";
constexpr const char* EVENT_OUTBOX_PREFIX = "fota.event_outbox:";
constexpr const char* CONTROLS_PREFIX  = "fota.controls:";
constexpr const char* POLICY           = "fota.policy";
constexpr const char* LOG_JOBS_PREFIX  = "fota.log_jobs:";
constexpr const char* MIGRATION_MARKER = "fota.migration_marker";

inline std::string downloadKey(const std::string& packageId) { return DOWNLOADS_PREFIX + packageId; }
inline std::string eventKey(std::uint64_t seq) { return EVENT_OUTBOX_PREFIX + std::to_string(seq); }
inline std::string controlKey(std::uint64_t revision) { return CONTROLS_PREFIX + std::to_string(revision); }
inline std::string logJobKey(const std::string& logRequestId) { return LOG_JOBS_PREFIX + logRequestId; }
} // namespace keys

// ============================================================
// Schema versions（业务级，envelope 内）
// ============================================================
namespace schema {
constexpr std::uint32_t VEHICLE_TASK_VERSION = 1;
// CR-012: v2 引入清单身份字段（status/acceptedRevision/fingerprint/retry）。
constexpr std::uint32_t INVENTORY_VERSION    = 2;
constexpr std::uint32_t CONSENT_VERSION      = 1;
constexpr std::uint32_t DOWNLOADS_VERSION    = 1;
constexpr std::uint32_t EXECUTION_VERSION    = 1;
constexpr std::uint32_t EVENT_OUTBOX_META_VERSION = 1;
constexpr std::uint32_t EVENT_OUTBOX_VERSION = 1;
constexpr std::uint32_t CONTROLS_VERSION     = 1;
constexpr std::uint32_t POLICY_VERSION       = 1;
constexpr std::uint32_t LOG_JOBS_VERSION     = 1;

constexpr std::uint32_t MIN_READER_VERSION = 0;
constexpr std::uint32_t WRITER_VERSION     = 1;
} // namespace schema

// ============================================================
// fota.vehicle_task
// ============================================================
struct FotaVehicleTaskRecord {
    std::uint32_t schemaVersion = schema::VEHICLE_TASK_VERSION;
    std::string   vehicleTaskId;
    std::uint64_t taskRevision = 0;
    std::string   targetBaselineCode;
    std::string   vehicleTaskState;        // VehicleTaskState 名
    Timestamp     frozenAtMs = 0;
    std::string   taskSnapshotPbHex;       // VehicleTaskSnapshot proto binary hex
    std::string   localDispositionResult;  // 本地旧任务处置结果
    bool          superseded = false;
};

// ============================================================
// fota.inventory - 清单身份（CCU-FOTA-DSN-CR-012 §持久化模型）
// ============================================================
// inventoryRevision 是“清单内容版本”，不是采集次数：同一内容必须复用同一
// revision。status 描述身份生命周期：
//   PREPARED            已分配/复用身份并将在（或已经）上行，云端尚未回执
//   ACCEPTED            云端已接受（acceptedInventoryRevision 为云端回执值）
//   REQUIRED_FULL       云端要求下一次上报完整清单（revision 保留）
//   CONFLICT            云端已将该 revision 绑定到不同内容（revision 弃用）
//   ALGORITHM_UNSUPPORTED 云端不支持当前算法（换算法后 FULL，revision 保留）
struct FotaInventoryRecord {
    std::uint32_t schemaVersion = schema::INVENTORY_VERSION;
    std::string   status = "PREPARED";
    std::string   mode;                    // FULL / DIGEST
    std::uint64_t inventoryRevision = 0;
    std::uint64_t acceptedInventoryRevision = 0;  // 0 = 尚无云端回执
    std::string   requiredMode;             // "" / FULL / DIGEST（来自云端回执）
    std::string   algorithm;
    std::string   ecuListDigestHex;
    std::string   fingerprintCanonicalization;   // cgw-fota-version-v1
    std::string   fingerprintHex;                // 空 = 身份不可用（旧格式）
    std::string   baselineCode;
    std::string   fotaMasterVersion;
    Timestamp     collectedAtMs = 0;
    std::string   requestPbHex;            // TaskCheckRequest proto binary hex
    std::uint32_t attempt = 0;             // 同一身份的尝试次数（重试不新增版本）
    Timestamp     nextRetryAtMs = 0;       // 同身份重试退避截止时间
    std::string   lastErrorCode;
    std::uint32_t digestFailures = 0;      // 连续 DIGEST 失配次数
    bool          fullRequired = false;    // 兼容 v1，等价 requiredMode=="FULL"
};

// ============================================================
// fota.consent
// ============================================================
struct FotaConsentRecord {
    std::uint32_t schemaVersion = schema::CONSENT_VERSION;
    std::string   vehicleTaskId;
    std::string   effectiveStatus;         // ConsentStatus 名
    std::string   receiptId;
    Timestamp     receiptExpiresAtMs = 0;
    std::string   termsId;
    std::string   termsVersion;
    std::string   termsDigestHex;
    std::string   termsAlgorithm;
    Timestamp     consentTimeMs = 0;
    std::string   channel;
};

// ============================================================
// fota.downloads:<package_id>（每包一条）
// ============================================================
struct FotaDownloadRecord {
    std::uint32_t schemaVersion = schema::DOWNLOADS_VERSION;
    std::string   packageId;
    std::string   packageRevision;
    std::string   etag;
    std::uint64_t currentOffsetBytes = 0;  // STORED_OBJECT 字节偏移
    std::string   offsetScope;             // "STORED_OBJECT"
    Timestamp     credentialExpiresAtMs = 0;
    std::string   verifyStatus;            // PENDING / SUCCEEDED / FAILED
    std::string   stageResultId;
    std::string   stageResultDigestHex;
    bool          ready = false;
};

// ============================================================
// fota.execution
// ============================================================
struct FotaExecutionRecord {
    std::uint32_t schemaVersion = schema::EXECUTION_VERSION;
    std::string   vehicleTaskId;
    std::string   executionId;
    std::uint32_t attemptNo = 0;
    std::string   permitId;
    std::string   permitToken;
    std::uint64_t controlRevision = 0;
    Timestamp     validUntilMs = 0;        // 仅约束进入 INSTALL_STARTED
    std::string   executionState;          // ExecutionState 名
    std::string   installPlanVersion;
    std::string   checkpointPbHex;         // InstallCheckpoint proto binary hex
    std::string   offlinePolicyPbHex;      // OfflinePolicy proto binary hex
    std::string   timeoutPolicyPbHex;      // TimeoutPolicy proto binary hex
};

// ============================================================
// fota.event_outbox_meta（事件水位；条目见 fota.event_outbox:<seq>）
// ============================================================
struct FotaEventOutboxMeta {
    std::uint32_t schemaVersion = schema::EVENT_OUTBOX_META_VERSION;
    std::uint64_t nextSequenceNo = 1;
    std::uint64_t acceptedSequenceNo = 0;
};

// ============================================================
// fota.event_outbox:<seq>（每事件一条）
// ============================================================
struct FotaEventOutboxRecord {
    std::uint32_t schemaVersion = schema::EVENT_OUTBOX_VERSION;
    std::uint64_t sequenceNo = 0;
    std::string   eventId;
    std::string   eventDigestHex;
    std::string   stage;
    std::string   eventStatus;
    std::uint32_t progress = 0;
    Timestamp     occurredAtMs = 0;
    std::string   payloadSummary;
    std::string   sendStatus;              // PENDING / SENT / ACKED
    std::string   eventPbHex;              // ExecutionEvent proto binary hex（重发用）
};

// ============================================================
// fota.controls:<revision>（每控制一条）
// ============================================================
struct FotaControlRecord {
    std::uint32_t schemaVersion = schema::CONTROLS_VERSION;
    std::string   controlId;
    std::uint64_t controlRevision = 0;
    std::string   action;                  // ControlAction 名
    std::string   scope;                   // ControlScope 名
    std::string   applyMode;               // ApplyMode 名
    Timestamp     issuedAtMs = 0;
    Timestamp     expiresAtMs = 0;
    std::string   reason;
    std::string   ackStatus;               // ControlAckStatus 名
    std::uint64_t ackSequenceNo = 0;
    Timestamp     appliedAtMs = 0;
};

// ============================================================
// fota.policy
// ============================================================
struct FotaPolicyRecord {
    std::uint32_t schemaVersion = schema::POLICY_VERSION;
    std::string   localPolicyVersion;
    std::string   basePreferenceVersion;
    std::string   preferenceVersion;
    std::string   effectivePolicyPbHex;    // EffectivePolicy proto binary hex
    bool          conflict = false;
    Timestamp     effectiveAtMs = 0;
};

// ============================================================
// fota.log_jobs:<log_request_id>（每日志任务一条）
// ============================================================
struct FotaLogJobRecord {
    std::uint32_t schemaVersion = schema::LOG_JOBS_VERSION;
    std::string   logRequestId;
    std::string   objectKey;
    std::string   digestHex;
    std::string   algorithm;
    std::uint64_t sizeBytes = 0;
    std::string   status;                  // PENDING / UPLOADED / FAILED
    Timestamp     completedAtMs = 0;
};

} // namespace fota
} // namespace store
} // namespace ccu_fota
