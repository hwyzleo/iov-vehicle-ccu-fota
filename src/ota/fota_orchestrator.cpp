// =============================================================================
// src/ota/fota_orchestrator.cpp
// CCU-FOTA 车云 FOTA 编排器实现 (CCU-FOTA-DSN-CR-009 §13.4/§13.5 / CR-011)
// =============================================================================

#include "ccu/fota/ota/fota_orchestrator.hpp"
#include "ccu/fota/ota/fota_digest.hpp"
#include "ccu/fota/ota/inventory_error_codes.hpp"
#include "ccu/fota/ota/state/state_proto.hpp"
#include "ccu/fota/store/fota_cloud_state_serializer.hpp"

#include "fota_log_adapter.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace ccu_fota {
namespace ota {

using namespace ccu_fota::store::fota;

namespace {
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

FotaOrchestrator::FotaOrchestrator(FotaCloudProxy& cloud, InventoryProvider& inv,
                                   ConsentProvider& consent, PackageDownloader& dl,
                                   VehicleConditionProvider& cond, InstallExecutor& exec,
                                   LogCollector& log, store::fota::FotaCloudStateStore& store,
                                   FotaOrchestratorConfig cfg,
                                   ClockSyncProvider* clockSync,
                                   RevisionAllocator revisionAllocator,
                                   InstalledVersionRegistrar* registrar)
    : cloud_(cloud), inv_(inv), consent_(consent), dl_(dl), cond_(cond),
      exec_(exec), log_(log), store_(store), cfg_(std::move(cfg)),
      clockSync_(clockSync), registrar_(registrar) {
    // CR-012：清单身份决策器（唯一分配 revision 与决定 FULL/DIGEST 的入口）。
    InventoryIdentityConfig icfg;
    icfg.retryBackoff = cfg_.inventoryIdentityRetryBackoff;
    icfg.digestMaxConsecutiveFailures = cfg_.inventoryDigestMaxConsecutiveFailures;
    icfg.changeDetectionEnabled = cfg_.inventoryChangeDetectionEnabled;
    planner_ = std::make_unique<InventoryIdentityPlanner>(
        store_, std::move(revisionAllocator), icfg);
}

CallContext FotaOrchestrator::makeCtx(const std::string& idempotencyKey) const {
    CallContext ctx;
    ctx.traceId = "trace-" + idempotencyKey;
    ctx.requestId = "req-" + idempotencyKey;
    ctx.idempotencyKey = idempotencyKey;
    ctx.timeout = cfg_.cloudCallTimeout;
    ctx.deviceId = cfg_.deviceId;
    ctx.vin = cfg_.vin;
    ctx.vehicleTaskId = frozenTask_.vehicle_task_id();
    ctx.executionId = executionId_;
    return ctx;
}

std::int64_t FotaOrchestrator::nowMs() const { return ::ccu_fota::ota::nowMs(); }

// ---------------------------------------------------------------------------
// 持久化辅助
// ---------------------------------------------------------------------------
void FotaOrchestrator::persistVehicleTask(const char* action) {
    FotaVehicleTaskRecord r;
    r.vehicleTaskId = frozenTask_.vehicle_task_id();
    r.taskRevision = frozenTask_.task_revision();
    r.targetBaselineCode = frozenTask_.target_baseline_code();
    r.vehicleTaskState = vehicleTaskStateToString(vtState_);
    r.frozenAtMs = nowMs();
    std::string bin;
    frozenTask_.SerializeToString(&bin);
    r.taskSnapshotPbHex = protoBinaryToHex(bin);
    r.localDispositionResult = action;
    store_.saveVehicleTask(r);
}

void FotaOrchestrator::persistExecution() {
    FotaExecutionRecord r;
    r.vehicleTaskId = frozenTask_.vehicle_task_id();
    r.executionId = executionId_;
    r.attemptNo = attemptNo_;
    r.permitId = permit_.permit_id();
    r.permitToken = permit_.permit_token();
    r.controlRevision = permit_.control_revision();
    r.validUntilMs = permit_.valid_until_ms();
    r.executionState = executionStateToString(exState_);
    r.installPlanVersion = permit_.install_plan_version();
    std::string polBin;
    if (permit_.has_offline_policy()) {
        permit_.offline_policy().SerializeToString(&polBin);
        r.offlinePolicyPbHex = protoBinaryToHex(polBin);
    }
    if (permit_.has_timeout_policy()) {
        polBin.clear();
        permit_.timeout_policy().SerializeToString(&polBin);
        r.timeoutPolicyPbHex = protoBinaryToHex(polBin);
    }
    r.installedVersionRegistrationId = installedVersionRegistrationId_;
    store_.saveExecution(r);

    // 事件水位（outbox meta）
    FotaEventOutboxMeta meta;
    meta.nextSequenceNo = nextSeq_;
    meta.acceptedSequenceNo = acceptedSeq_;
    store_.saveEventOutboxMeta(meta);
}

void FotaOrchestrator::loadFromStore() {
    if (auto vt = store_.loadVehicleTask()) {
        vtState_ = VehicleTaskState::None;
        vehicleTaskStateFromString(vt->vehicleTaskState.c_str(), vtState_);
        if (!vt->taskSnapshotPbHex.empty()) {
            std::string bin = hexToProtoBinary(vt->taskSnapshotPbHex);
            frozenTask_.ParseFromString(bin);
        } else {
            // 迁移/异常后无任务快照 -> 回到 None 重新检测（新契约恢复路径）
            vtState_ = VehicleTaskState::None;
            exState_ = ExecutionState::PermitPersisted;
            executionId_.clear();
            nextSeq_ = 1;
            acceptedSeq_ = 0;
            return;
        }
    }
    if (auto ex = store_.loadExecution()) {
        executionStateFromString(ex->executionState.c_str(), exState_);
        executionId_ = ex->executionId;
        attemptNo_ = ex->attemptNo;
        installedVersionRegistrationId_ = ex->installedVersionRegistrationId;
        // CR-015 §D5：恢复活动 Execution 时必须由本进程重新 prepare 执行器上下文
        // （frozenTask 已随 fota.vehicle_task 持久化），不得直接使用空执行器状态。
        executorPrepared_ = false;
        permit_.set_execution_id(ex->executionId);
        permit_.set_attempt_no(ex->attemptNo);
        permit_.set_permit_id(ex->permitId);
        permit_.set_permit_token(ex->permitToken);
        permit_.set_control_revision(ex->controlRevision);
        if (ex->validUntilMs != 0) permit_.set_valid_until_ms(ex->validUntilMs);
        if (!ex->installPlanVersion.empty()) permit_.set_install_plan_version(ex->installPlanVersion);
        if (!ex->offlinePolicyPbHex.empty()) {
            std::string bin = hexToProtoBinary(ex->offlinePolicyPbHex);
            permit_.mutable_offline_policy()->ParseFromString(bin);
        }
        if (!ex->timeoutPolicyPbHex.empty()) {
            std::string bin = hexToProtoBinary(ex->timeoutPolicyPbHex);
            permit_.mutable_timeout_policy()->ParseFromString(bin);
        }
    }
    if (auto meta = store_.loadEventOutboxMeta()) {
        nextSeq_ = meta->nextSequenceNo;
        acceptedSeq_ = meta->acceptedSequenceNo;
    }
}

void FotaOrchestrator::reconcileOnStart() { loadFromStore(); }

// ---------------------------------------------------------------------------
// CR-012 D8：终态 -> 检测态。
// 终态上报已在上一步 durable 完成；这里回到 None 继续周期任务检测，并清理
// 任务/执行记录（保留事件水位与已接受清单身份）。下载上下文不在此清理，
// packageCacheAction 语义仍由云端任务响应决定。
// ---------------------------------------------------------------------------
void FotaOrchestrator::resetTerminalToDetection() {
    // 保留一次终态对账身份（下一次 checkTask 携带 local_vehicle_task_status）。
    if (!frozenTask_.vehicle_task_id().empty()) {
        lastTerminalTaskId_ = frozenTask_.vehicle_task_id();
        lastTerminalTaskRevision_ = frozenTask_.task_revision();
        lastTerminalState_ = vtState_;
        terminalReconPending_ = true;
    }
    vtState_ = VehicleTaskState::None;
    exState_ = ExecutionState::PermitPersisted;
    frozenTask_.Clear();
    permit_.Clear();
    executionId_.clear();
    attemptNo_ = 0;
    installedVersionRegistrationId_.clear();
    executorPrepared_ = false;
    lastReceivedControlRev_ = 0;
    lastAppliedControlRev_ = 0;
    pendingControlIds_.clear();
    try {
        store_.removeVehicleTask();
        store_.removeExecution();
    } catch (const std::exception&) {
        // 清理失败不阻断检测：下次 start 的 loadFromStore 仍会保守恢复。
    }
}

// ---------------------------------------------------------------------------
// EventSink: 先 durable 写 outbox 单条 + 推进水位 meta，再分配 sequenceNo
// ---------------------------------------------------------------------------
bool FotaOrchestrator::emit(const ::vehicle::fota::v1::ExecutionEvent& event) {
    auto meta = store_.loadEventOutboxMeta().value_or(FotaEventOutboxMeta{});
    if (meta.nextSequenceNo - 1 >= cfg_.eventOutboxMax) return false;

    // CR-015 §D4：eventDigest 由编排器统一计算（不信任执行器占位值），覆盖稳定
    // 业务字段，不含本地 sequenceNo 分配状态；重发保持原摘要。
    ::vehicle::fota::v1::ExecutionEvent evt = event;
    if (evt.event_id().empty()) {
        evt.set_event_id("EVT-seq-" + std::to_string(meta.nextSequenceNo));
    }
    evt.mutable_event_digest()->set_algorithm("sha-256");
    evt.mutable_event_digest()->set_value_hex(digest::executionEventDigestHex(evt));

    FotaEventOutboxRecord e;
    e.sequenceNo = meta.nextSequenceNo;
    e.eventId = evt.event_id();
    e.eventDigestHex = evt.event_digest().value_hex();
    e.stage = evt.stage();
    e.eventStatus = evt.event_status();
    e.progress = evt.progress();
    e.occurredAtMs = evt.occurred_at_ms();
    e.payloadSummary = evt.stage();  // 新契约无 payload_summary，用 stage 作摘要
    e.sendStatus = "PENDING";
    std::string bin;
    evt.SerializeToString(&bin);
    e.eventPbHex = protoBinaryToHex(bin);

    store_.saveEvent(e);
    meta.nextSequenceNo = e.sequenceNo + 1;
    store_.saveEventOutboxMeta(meta);
    nextSeq_ = meta.nextSequenceNo;
    return true;
}

// ---------------------------------------------------------------------------
// 时钟门禁：上行 Envelope 的 timestamp_ms/expire_at_ms 由 CLOCK_REALTIME 推导，
// 时钟未与可信时间源同步时 expire_at_ms 落在过去，云侧网关按契约判定
// MESSAGE_EXPIRED 并丢入上行 DLQ（消息永久丢失且无业务回执）。因此时钟未就绪时
// 不发出任何上行，只保留 durable 状态等待下次重试（fail-closed）。
// ---------------------------------------------------------------------------
bool FotaOrchestrator::clockReady(StepOutcome& outcome) {
    if (!cfg_.requireClockSync || clockSync_ == nullptr) {
        return true;
    }
    ClockSyncStatus st = clockSync_->status();
    if (st.synchronized) {
        return true;
    }
    outcome.action = "awaitClockSync";
    outcome.awaitingClockSync = true;
    outcome.error = std::string("system clock not synchronized: ") + st.detail +
                    ", systemTimeMs=" + std::to_string(st.systemTimeMs);
    return false;
}

// ---------------------------------------------------------------------------
// CCU-FOTA-DSN-CR-013：安装结果登记（DIAG 全权）
//
// 登记能力是安装前置：不可用时禁止开始新安装（清单读取与上报不受影响）。
// 登记是成功执行收口的必要条件：失败则保持 RESULT_PENDING，用同一
// executionId+attemptNo+idempotencyKey 重试（DIAG 侧幂等）。
// ---------------------------------------------------------------------------
bool FotaOrchestrator::registrationCapable(StepOutcome& outcome, const char* stage) {
    if (registrar_ != nullptr && registrar_->available()) {
        return true;
    }
    FotaLogAdapter::orchestrator().warn(
        fota_events::DIAG_REGISTRATION_UNAVAILABLE,
        "Installed version registration unavailable, new install blocked",
        {flog::f_str("stage", stage),
         flog::f_str("error_code", inventory_error::kRegistrationUnavailable)});
    outcome.error =
        "installed version registration unavailable (install blocked)";
    outcome.vehicleTaskState = vtState_;
    outcome.executionState = exState_;
    return false;
}

InstalledVersionsReport FotaOrchestrator::buildInstalledVersionsReport() const {
    InstalledVersionsReport report;
    report.vehicleTaskId = frozenTask_.vehicle_task_id();
    report.taskRevision = frozenTask_.task_revision();
    report.executionId = executionId_;
    report.attemptNo = attemptNo_;
    report.idempotencyKey = executionId_ + ":" + std::to_string(attemptNo_);
    report.appliedAtMs = nowMs();
    // items 只来自冻结任务包清单（installedVersion = targetVersion），不从本地
    // 清单或代码常量推算（CR-013 D2/D3）。
    for (const auto& pkg : frozenTask_.package_list()) {
        InstalledVersionItem item;
        item.ecuId = pkg.ecu_id();
        item.packageId = pkg.package_id();
        item.packageRevision = pkg.package_revision();
        item.installedVersion = pkg.target_version();
        report.items.push_back(std::move(item));
    }
    return report;
}

bool FotaOrchestrator::registerInstalledVersions(StepOutcome& outcome) {
    // 已登记（重启重放）：跳过登记，直接收口（CR-013 §失败、重试与恢复）。
    if (!installedVersionRegistrationId_.empty()) {
        return true;
    }

    // 能力不可用：不关闭本次成功执行，保持 RESULT_PENDING 等待恢复。
    if (registrar_ == nullptr || !registrar_->available()) {
        exState_ = ExecutionState::ResultPending;
        persistExecution();
        FotaLogAdapter::orchestrator().warn(
            fota_events::DIAG_REGISTRATION_UNAVAILABLE,
            "Installed version registration unavailable, execution kept pending",
            {flog::f_str("execution_id", executionId_),
             flog::f_int("attempt_no", static_cast<int64_t>(attemptNo_)),
             flog::f_str("error_code",
                         inventory_error::kRegistrationUnavailable)});
        outcome.error = "installed version registration unavailable";
        return false;
    }

    InstalledVersionsReport report = buildInstalledVersionsReport();
    if (report.items.empty()) {
        // 成功执行却没有可登记条目：属于任务/映射异常，不得静默收口。
        exState_ = ExecutionState::ResultPending;
        persistExecution();
        FotaLogAdapter::orchestrator().error(
            fota_events::DIAG_REGISTRATION_REJECTED,
            "Installed version registration has no items, execution kept pending",
            {flog::f_str("execution_id", executionId_),
             flog::f_str("error_code", inventory_error::kRegistrationFailed),
             flog::f_str("reason", "empty items")});
        outcome.error = "installed version registration has no items";
        return false;
    }

    auto ctx = makeCtx("registry-" + executionId_);
    RegistrationResult result =
        registrar_->registerInstalledVersions(report, ctx);

    if (result.completed()) {
        if (result.registrationId.empty()) {
            exState_ = ExecutionState::ResultPending;
            persistExecution();
            FotaLogAdapter::orchestrator().warn(
                fota_events::DIAG_REGISTRATION_REJECTED,
                "Registration completed without registrationId, execution kept pending",
                {flog::f_str("execution_id", executionId_),
                 flog::f_str("error_code", inventory_error::kRegistrationFailed)});
            outcome.error = "registration missing registrationId";
            return false;
        }
        // durable 保存登记标识后才能在内存生效（否则重启会重复登记，虽幂等）。
        installedVersionRegistrationId_ = result.registrationId;
        try {
            persistExecution();
        } catch (const std::exception& e) {
            installedVersionRegistrationId_.clear();
            exState_ = ExecutionState::ResultPending;
            outcome.error = std::string("registration durable save failed: ") + e.what();
            return false;
        }
        FotaLogAdapter::orchestrator().info(
            fota_events::DIAG_REGISTRATION_ACCEPTED,
            "Installed versions registered, execution can be closed",
            {flog::f_str("execution_id", executionId_),
             flog::f_int("attempt_no", static_cast<int64_t>(attemptNo_)),
             flog::f_int("applied_count",
                         static_cast<int64_t>(result.appliedCount)),
             flog::f_str("disposition",
                         registrationDispositionToString(result.disposition))});
        return true;
    }

    if (result.rejected()) {
        // 永久性拒绝（未知 ecuId / 非法版本 / 同键不同内容）：不无限重试。
        // 记录 ERROR 级事件后按“登记已尽力”收口，由运维修正后触发新一轮任务。
        std::string reasons;
        for (const auto& it : result.rejectedItems) {
            if (!reasons.empty()) reasons += ",";
            reasons += it.ecuId + ":" + it.reasonCode;
        }
        FotaLogAdapter::orchestrator().error(
            fota_events::DIAG_REGISTRATION_REJECTED,
            "Installed version registration permanently rejected, closing execution",
            {flog::f_str("execution_id", executionId_),
             flog::f_int("attempt_no", static_cast<int64_t>(attemptNo_)),
             flog::f_str("error_code", result.errorCode),
             flog::f_str("reason", reasons.empty() ? result.errorDetail : reasons)});
        return true;
    }

    // RETRYABLE_FAILED / UNAVAILABLE：保持 RESULT_PENDING，同身份重试。
    exState_ = ExecutionState::ResultPending;
    persistExecution();
    FotaLogAdapter::orchestrator().warn(
        fota_events::DIAG_REGISTRATION_REJECTED,
        "Installed version registration failed, execution kept pending for retry",
        {flog::f_str("execution_id", executionId_),
         flog::f_int("attempt_no", static_cast<int64_t>(attemptNo_)),
         flog::f_str("disposition",
                     registrationDispositionToString(result.disposition)),
         flog::f_str("error_code", result.errorCode),
         flog::f_str("reason", result.errorDetail)});
    outcome.error = "installed version registration failed: " + result.errorCode;
    return false;
}

// ---------------------------------------------------------------------------
// step: 状态驱动分步推进
// ---------------------------------------------------------------------------
StepOutcome FotaOrchestrator::step() {
    StepOutcome o;
    o.vehicleTaskState = vtState_;
    o.executionState = exState_;
    if (isTerminalVehicleTaskState(vtState_)) {
        // CR-012 D8：终态只结束本次任务，不得终止后续任务探测。
        // 终态同上一步已上报并 durable；本步回到检测态并清理任务/执行记录。
        resetTerminalToDetection();
        o.action = "resetToDetection";
        o.vehicleTaskState = vtState_;
        o.executionState = exState_;
        return o;
    }
    // 任何上行之前先过时钟门禁（覆盖启动期首次版本/清单上报与后续全部阶段）。
    if (!clockReady(o)) {
        return o;
    }
    switch (vtState_) {
        case VehicleTaskState::None:
        case VehicleTaskState::Discovered:
            return doCheckTask();
        case VehicleTaskState::ConsentPending:
            return doConsent();
        case VehicleTaskState::DownloadPending:
        case VehicleTaskState::Downloading:
            return doDownload();
        case VehicleTaskState::Ready:
        case VehicleTaskState::WaitingWindow:
        case VehicleTaskState::PermitPending:
            return doPermit();
        case VehicleTaskState::Executing:
            return doExecute();
        case VehicleTaskState::RetryPending:
            vtState_ = VehicleTaskState::PermitPending;
            return doPermit();
        case VehicleTaskState::RollbackPending:
            exState_ = ExecutionState::Rollback;
            vtState_ = VehicleTaskState::Executing;
            return doExecute();
        default:
            o.action = "noop";
            return o;
    }
}

// ---------------------------------------------------------------------------
// 1. 任务检测
// ---------------------------------------------------------------------------
StepOutcome FotaOrchestrator::doCheckTask() {
    StepOutcome o;
    o.action = "checkTask";

    // CR-012：同一身份重试退避中 -> 本轮不采集/不上报（不计新版本）。
    if (planner_ && !planner_->retryReady(nowMs())) {
        o.action = "inventoryRetryWait";
        o.vehicleTaskState = vtState_;
        return o;
    }

    // 采集（Defer：采集阶段不得分配 revision；失败/空清单不上报）。
    auto inv = inv_.collectInventory();
    if (!inv.collected || inv.ecuList.empty()) {
        // CR-014 D3：优先透出采集端口给出的可定位错误码（如
        // FOTA-INVENTORY-REQUIRED-FACT-MISSING），否则回退为既有空清单错误码。
        const std::string errorCode = inv.collectionErrorCode.empty()
                                          ? std::string("FOTA-INVENTORY-COLLECT-EMPTY")
                                          : inv.collectionErrorCode;
        FotaLogAdapter::orchestrator().warn(
            fota_events::INVENTORY_COLLECT_EMPTY,
            "Inventory collect failed or empty, uplink skipped",
            {flog::f_str("error_code", errorCode),
             flog::f_str("error_detail", inv.collectionErrorDetail)});
        o.error = "inventory collect failed or empty";
        return o;
    }

    // 清单身份决策 + PREPARED 检查点（durable，必须先于云端调用）。
    InventoryIdentity ident;
    FotaInventoryRecord prepared;
    try {
        ident = planner_->resolve(inv);
        prepared = planner_->markPrepared(inv, ident);
    } catch (const std::exception& e) {
        FotaLogAdapter::orchestrator().error(
            fota_events::INVENTORY_IDENTITY_FAILED,
            "Inventory identity unavailable, uplink blocked",
            {flog::f_str("error_code", "FOTA-INVENTORY-IDENTITY-UNAVAILABLE"),
             flog::f_str("reason", inventoryChangeReasonToString(ident.reason))});
        o.error = std::string("inventory identity unavailable: ") + e.what();
        return o;
    }

    ::vehicle::fota::v1::TaskCheckRequest req;
    req.set_baseline_code(inv.baselineCode);
    req.set_fota_master_version(inv.fotaMasterVersion);
    if (terminalReconPending_ && !lastTerminalTaskId_.empty()) {
        // CR-012 D8：终态后首次检测携带上一次任务身份与终态，仅供云端对账。
        req.set_local_vehicle_task_id(lastTerminalTaskId_);
        req.set_local_task_revision(lastTerminalTaskRevision_);
        req.set_local_vehicle_task_status(toProto(lastTerminalState_));
    } else if (!frozenTask_.vehicle_task_id().empty()) {
        req.set_local_vehicle_task_id(frozenTask_.vehicle_task_id());
        req.set_local_task_revision(frozenTask_.task_revision());
        req.set_local_vehicle_task_status(toProto(vtState_));
    }
    req.set_inventory_mode(ident.mode);
    req.set_inventory_revision(prepared.inventoryRevision);
    req.mutable_ecu_list_digest()->set_algorithm(prepared.algorithm);
    req.mutable_ecu_list_digest()->set_value_hex(prepared.ecuListDigestHex);
    // CR-012 D5：FULL 携带列表，DIGEST 只发送身份（revision/algorithm/digest）。
    if (ident.mode == ::vehicle::fota::v1::INVENTORY_MODE_FULL) {
        for (const auto& ecu : inv.ecuList) *req.add_ecu_list() = ecu;
    }

    std::string reqBin;
    req.SerializeToString(&reqBin);
    try {
        planner_->saveRequestPayload(prepared, protoBinaryToHex(reqBin));
    } catch (const std::exception& e) {
        o.error = std::string("inventory prepared checkpoint failed: ") + e.what();
        return o;
    }

    FotaLogAdapter::orchestrator().info(
        fota_events::INVENTORY_FINGERPRINT_COMPARED,
        "Inventory content fingerprint compared",
        {flog::f_str("fingerprint_canonicalization", inv.fingerprintCanonicalization),
         flog::f_bool("changed", ident.reason == InventoryChangeReason::FirstBaseline ||
                                  ident.reason == InventoryChangeReason::ContentChanged),
         flog::f_str("change_reason", inventoryChangeReasonToString(ident.reason))});

    FotaLogAdapter::orchestrator().info(
        ident.freshRevision ? fota_events::INVENTORY_REVISION_ALLOCATED
                            : fota_events::INVENTORY_REVISION_REUSED,
        "Inventory identity resolved for task check",
        {flog::f_int("inventory_revision", static_cast<int64_t>(prepared.inventoryRevision)),
         flog::f_str("inventory_mode", prepared.mode),
         flog::f_str("algorithm", prepared.algorithm),
         flog::f_str("digest", prepared.ecuListDigestHex),
         flog::f_str("fingerprint_canonicalization", inv.fingerprintCanonicalization),
         flog::f_str("change_reason", inventoryChangeReasonToString(ident.reason)),
         flog::f_int("attempt", static_cast<int64_t>(prepared.attempt))});

    ::vehicle::fota::v1::TaskCheckResponse resp;
    try {
        // 稳定业务身份：首次 checkTask 时 cloud 尚未下发 VehicleTask，vehicle_task_id
        // 为空，不得拼出 "checkTask-" 这样的退化常量身份（多车共享同一 request_id）。
        // 传输层 message_id 由 FotaMessageCodec 独立生成，与此处的 request_id 无关。
        const std::string localTaskId = frozenTask_.vehicle_task_id();
        resp = cloud_.checkTask(
            req, makeCtx("checkTask-" + (localTaskId.empty() ? "initial" : localTaskId)));
    } catch (const FotaCloudException& e) {
        // 结果未知：保持 PREPARED，同身份退避重试（不得新分配 revision）。
        try {
            planner_->onTransportFailure(ident, "FOTA-INVENTORY-TRANSPORT");
        } catch (const std::exception&) {
            // durable 写失败不阻断传输错误上报；下轮按 PREPARED 重试。
        }
        o.error = e.what();
        o.transportFailed = true;
        return o;
    }

    // 终态对账身份已随请求上送（只对账一次；失败时保留由下次重试继续携带）。
    terminalReconPending_ = false;
    lastTerminalTaskId_.clear();
    lastTerminalTaskRevision_ = 0;
    lastTerminalState_ = VehicleTaskState::None;

    if (resp.inventory_disposition() != ::vehicle::fota::v1::INVENTORY_DISPOSITION_ACCEPTED) {
        // FULL_REQUIRED/REVISION_CONFLICT/DIGEST_MISMATCH/ALGORITHM_UNSUPPORTED
        // -> durable 记录要求与失配原因，按决策矩阵决定下一轮 mode/revision。
        try {
            planner_->onRejected(ident, resp);
        } catch (const std::exception& e) {
            o.error = std::string("inventory disposition persist failed: ") + e.what();
            return o;
        }
        FotaLogAdapter::orchestrator().warn(
            fota_events::INVENTORY_DISPOSITION,
            "Inventory not accepted by cloud",
            {flog::f_str("inventory_disposition",
                         ::vehicle::fota::v1::InventoryDisposition_Name(
                             resp.inventory_disposition())),
             flog::f_int("inventory_revision", static_cast<int64_t>(prepared.inventoryRevision)),
             flog::f_str("inventory_mode", prepared.mode)});
        o.error = "inventory not accepted: " +
                  ::vehicle::fota::v1::InventoryDisposition_Name(resp.inventory_disposition());
        return o;
    }

    try {
        planner_->onAccepted(ident, resp);
    } catch (const std::exception& e) {
        // 未落盘则下轮仍按 PREPARED 复用同一身份（不会新分配），可安全重试。
        o.error = std::string("inventory accepted state persist failed: ") + e.what();
        return o;
    }
    FotaLogAdapter::orchestrator().info(
        fota_events::INVENTORY_DISPOSITION,
        "Inventory accepted by cloud",
        {flog::f_str("inventory_disposition", "INVENTORY_DISPOSITION_ACCEPTED"),
         flog::f_int("inventory_revision", static_cast<int64_t>(prepared.inventoryRevision)),
         flog::f_int("accepted_inventory_revision",
                     static_cast<int64_t>(resp.has_accepted_inventory_revision()
                                              ? resp.accepted_inventory_revision()
                                              : prepared.inventoryRevision)),
         flog::f_str("inventory_mode", prepared.mode)});

    // 无任务不是错误（CR-012）：本轮检测完成，下轮内容未变则走 DIGEST。
    if (!resp.has_task()) {
        o.action = "checkTaskIdle";
        o.vehicleTaskState = vtState_;
        return o;
    }

    // 冻结任务快照（VehicleTaskSnapshot）——只在 inventoryDisposition=ACCEPTED 后。
    frozenTask_ = resp.task();
    vtState_ = VehicleTaskState::Discovered;
    persistVehicleTask("checkTask");

    // 授权要求
    vtState_ = frozenTask_.consent_required()
                   ? VehicleTaskState::ConsentPending
                   : VehicleTaskState::DownloadPending;
    persistVehicleTask("consentRequired");
    o.vehicleTaskState = vtState_;
    return o;
}

// ---------------------------------------------------------------------------
// 2. 授权
// ---------------------------------------------------------------------------
StepOutcome FotaOrchestrator::doConsent() {
    StepOutcome o;
    o.action = "consent";
    ConsentTermsId terms;
    terms.termsVersion = frozenTask_.has_terms_version() ? frozenTask_.terms_version() : "v1";
    terms.termsId = "T-1";
    terms.termsDigestHex = std::string(64, 'a');
    terms.algorithm = "sha-256";

    auto choice = consent_.requestConsent(frozenTask_.vehicle_task_id(), terms);
    ::vehicle::fota::v1::ConsentReport report;
    report.set_consent_status(choice.userChoice);
    report.set_terms_version(terms.termsVersion);
    report.set_terms_id(terms.termsId);
    report.mutable_terms_digest()->set_algorithm(terms.algorithm);
    report.mutable_terms_digest()->set_value_hex(terms.termsDigestHex);
    report.set_consent_time_ms(nowMs());
    report.set_channel("vehicle");

    ::vehicle::fota::v1::ConsentResponse resp;
    try {
        resp = cloud_.reportConsent(report, makeCtx("consent-" + frozenTask_.vehicle_task_id()));
    } catch (const FotaCloudException& e) {
        o.error = e.what();
        o.transportFailed = true;
        return o;
    }

    // 仅以 effectiveConsentStatus 推进（不以 accepted 推断）
    FotaConsentRecord cr;
    cr.vehicleTaskId = frozenTask_.vehicle_task_id();
    cr.effectiveStatus = ::vehicle::fota::v1::ConsentStatus_Name(resp.effective_consent_status());
    cr.receiptId = resp.has_consent_receipt_id() ? resp.consent_receipt_id() : "";
    cr.receiptExpiresAtMs = resp.has_receipt_expires_at_ms() ? resp.receipt_expires_at_ms() : 0;
    cr.termsId = terms.termsId;
    cr.termsVersion = terms.termsVersion;
    cr.termsDigestHex = terms.termsDigestHex;
    cr.termsAlgorithm = terms.algorithm;
    cr.consentTimeMs = nowMs();
    cr.channel = "vehicle";
    store_.saveConsent(cr);

    if (resp.effective_consent_status() == ::vehicle::fota::v1::CONSENT_STATUS_ACCEPTED) {
        vtState_ = VehicleTaskState::DownloadPending;
    } else {
        vtState_ = VehicleTaskState::Ended;
    }
    persistVehicleTask("consent");
    o.vehicleTaskState = vtState_;
    return o;
}

// ---------------------------------------------------------------------------
// 3. 下载与校验（per-package durable 上下文）
// ---------------------------------------------------------------------------
StepOutcome FotaOrchestrator::doDownload() {
    StepOutcome o;
    o.action = "download";

    bool allReady = true;
    for (const auto& pkg : frozenTask_.package_list()) {
        auto loaded = store_.loadDownload(pkg.package_id());
        FotaDownloadRecord entry;
        if (loaded.has_value()) {
            entry = *loaded;
        } else {
            entry.packageId = pkg.package_id();
            entry.packageRevision = pkg.package_revision();
            entry.offsetScope = "STORED_OBJECT";
            entry.verifyStatus = "PENDING";
            entry.ready = false;
        }
        if (entry.ready) continue;

        // 申请下载凭证
        ::vehicle::fota::v1::DownloadGrantRequest greq;
        greq.set_package_id(pkg.package_id());
        greq.set_task_revision(frozenTask_.task_revision());
        greq.set_package_revision(entry.packageRevision);
        if (entry.currentOffsetBytes > 0) {
            greq.set_current_etag(entry.etag);
            greq.set_current_offset_bytes(entry.currentOffsetBytes);
            greq.set_offset_scope(entry.offsetScope.empty() ? "STORED_OBJECT" : entry.offsetScope);
        }
        greq.set_network_type("LTE");

        ::vehicle::fota::v1::DownloadGrantResponse grant;
        try {
            grant = cloud_.requestDownload(greq, makeCtx("dl-" + pkg.package_id()));
        } catch (const FotaCloudException& e) {
            o.error = e.what();
            o.transportFailed = true;
            vtState_ = VehicleTaskState::Downloading;
            store_.saveDownload(entry);
            return o;
        }

        // ETag/packageRevision 变化 -> RESET_OFFSET（清零偏移并替换）
        if (grant.has_etag() && !grant.etag().empty() && grant.etag() != entry.etag) {
            entry.currentOffsetBytes = 0;
            entry.etag = grant.etag();
            if (!grant.package_revision().empty()) entry.packageRevision = grant.package_revision();
        } else if (grant.has_etag()) {
            entry.etag = grant.etag();
        }

        auto outcome = dl_.downloadAndVerify(pkg, grant, entry.currentOffsetBytes);
        entry.currentOffsetBytes = outcome.finalOffsetBytes;

        // 上报阶段结果（独立 StageResultReport）
        if (outcome.hasStageResult) {
            auto srep = outcome.stageResult;
            if (srep.stage_result_id().empty()) {
                srep.set_stage_result_id("SR-" + pkg.package_id() + "-" +
                                         std::to_string(entry.currentOffsetBytes));
            }
            if (srep.verified_package_revision().empty()) {
                srep.set_verified_package_revision(entry.packageRevision);
            }
            if (srep.verified_at_ms() == 0) srep.set_verified_at_ms(nowMs());
            try {
                auto sresp = cloud_.reportStageResult(
                    srep, makeCtx("sr-" + pkg.package_id()));
                if (sresp.accepted() && outcome.allStagesSucceeded) {
                    entry.verifyStatus = "SUCCEEDED";
                    entry.ready = true;
                    entry.stageResultId = srep.stage_result_id();
                    entry.stageResultDigestHex = srep.stage_result_digest().value_hex();
                } else {
                    entry.verifyStatus = "FAILED";
                    entry.ready = false;
                    allReady = false;
                }
            } catch (const FotaCloudException& e) {
                o.error = e.what();
                o.transportFailed = true;
                vtState_ = VehicleTaskState::Downloading;
                store_.saveDownload(entry);
                return o;
            }
        }

        if (!outcome.allStagesSucceeded) {
            entry.verifyStatus = "FAILED";
            entry.ready = false;
            allReady = false;
            o.error = outcome.errorDetail.empty() ? "download/verify failed" : outcome.errorDetail;
        }
        store_.saveDownload(entry);
    }

    if (allReady) {
        vtState_ = VehicleTaskState::Ready;
    } else {
        vtState_ = VehicleTaskState::Downloading;
    }
    persistVehicleTask("download");
    o.vehicleTaskState = vtState_;
    return o;
}

// ---------------------------------------------------------------------------
// 4. 安装许可
// ---------------------------------------------------------------------------
StepOutcome FotaOrchestrator::doPermit() {
    StepOutcome o;
    if (vtState_ == VehicleTaskState::Ready) {
        vtState_ = VehicleTaskState::WaitingWindow;
        persistVehicleTask("waitingWindow");
        o.action = "waitingWindow";
        o.vehicleTaskState = vtState_;
        return o;
    }
    o.action = "permit";

    // CCU-FOTA-DSN-CR-013 D7：登记能力是安装前置。未分配/禁用/不可用时阻止
    // 新的安装（但清单读取与上报不受影响）；已启动的成功执行不在此路径。
    if (!registrationCapable(o, "permit")) {
        return o;
    }

    // 复检窗口与本地门禁
    std::int64_t t = nowMs();
    if (t < frozenTask_.start_time_ms() || t >= frozenTask_.end_time_ms()) {
        o.error = "install window not open";
        return o;  // 停留 WaitingWindow
    }
    if (!cond_.allGuardsPassed()) {
        o.error = "local guards not passed";
        return o;
    }

    auto condEv = cond_.evaluateConditions();
    ::vehicle::fota::v1::InstallPermitRequest preq;
    preq.set_task_revision(frozenTask_.task_revision());
    preq.set_install_plan_version(frozenTask_.install_plan_version());
    preq.mutable_package_manifest_digest()->set_algorithm("sha-256");
    preq.mutable_package_manifest_digest()->set_value_hex(
        digest::packageManifestDigestHex(frozenTask_));
    preq.set_consent_receipt_id(
        store_.loadConsent().value_or(FotaConsentRecord{}).receiptId);
    preq.mutable_local_readiness_digest()->set_algorithm("sha-256");
    preq.mutable_local_readiness_digest()->set_value_hex(condEv.localReadinessDigest);
    preq.set_local_guard_passed(condEv.allGuardsPassed);
    for (const auto& f : condEv.failedConditions) preq.add_failed_conditions(f);
    preq.set_condition_set_version(condEv.conditionSetVersion);
    *preq.mutable_condition_snapshot() = condEv.snapshot;

    ::vehicle::fota::v1::InstallPermitResponse resp;
    try {
        resp = cloud_.requestInstall(preq, makeCtx("permit-" + frozenTask_.vehicle_task_id()));
    } catch (const FotaCloudException& e) {
        o.error = e.what();
        o.transportFailed = true;
        return o;
    }

    if (!resp.allowed()) {
        o.error = "install denied: " +
                  (resp.has_deny_reason() ? resp.deny_reason() : "no reason");
        return o;  // 停留 PermitPending/WaitingWindow，等待重试
    }

    // durable 保存 Execution/permit/controlRevision/validUntil/离线/超时策略后再执行
    permit_ = resp;
    executionId_ = resp.has_execution_id() ? resp.execution_id() : "";
    attemptNo_ = resp.has_attempt_no() ? resp.attempt_no() : 0;
    exState_ = ExecutionState::PermitPersisted;
    vtState_ = VehicleTaskState::Executing;
    // 新 Execution 必须由本进程重新 prepare 执行器上下文（CR-015 §D5）。
    executorPrepared_ = false;
    persistVehicleTask("permit");
    persistExecution();

    // 进入 INSTALL_STARTED 前复检 validUntil 与本地门禁
    if (resp.has_valid_until_ms() && nowMs() >= resp.valid_until_ms()) {
        o.error = "pre-install recheck failed (validUntil)";
        return o;
    }
    if (!cond_.allGuardsPassed()) {
        o.error = "pre-install recheck failed (guards)";
        return o;
    }

    o.vehicleTaskState = vtState_;
    o.executionState = exState_;
    return o;
}

// ---------------------------------------------------------------------------
// 5. 执行事件、控制与最终结果
// ---------------------------------------------------------------------------
StepOutcome FotaOrchestrator::doExecute() {
    StepOutcome o;
    o.action = "execute";

    // 首次（本进程首次进入执行）：prepare + start；恢复（安装已开始/结果待确认）：
    // 先用持久化 frozenTask 重新 prepare，再按既有 checkpoint resume。
    // 不得在重启后从空任务上下文生成空最终结果（CR-015 §D5）。
    const bool freshStart = exState_ == ExecutionState::PermitPersisted ||
                            exState_ == ExecutionState::Ready;
    if (!executorPrepared_) {
        // CCU-FOTA-DSN-CR-013 D7：仅新安装开始前复检登记能力（不可用则不得起刷）。
        // 已开始的执行即使能力失效，仍按既有规则上报并在成功时等待登记。
        if (freshStart && !registrationCapable(o, "execute")) {
            return o;
        }
        auto prep = exec_.prepare(frozenTask_);
        if (!prep.ready) {
            o.error = "executor prepare failed: " + prep.reason;
            return o;
        }
        if (freshStart) {
            auto st = exec_.start(permit_, *this);
            if (!st.started) {
                o.error = "executor start failed: " + st.reason;
                return o;
            }
            exState_ = ExecutionState::InstallStarted;
            persistExecution();
        } else {
            auto ckpt = exec_.checkpoint();
            auto rs = exec_.resume(ckpt, *this);
            if (!rs.resumed) {
                o.error = "executor resume failed: " + rs.reason;
                return o;
            }
            FotaLogAdapter::orchestrator().info(
                fota_events::OTA_EXECUTION_RESTORED,
                "Execution context restored and resumed from checkpoint",
                {flog::f_str("execution_id", executionId_),
                 flog::f_int("attempt_no", static_cast<std::int64_t>(attemptNo_)),
                 flog::f_str("execution_state",
                             executionStateToString(exState_))});
        }
        executorPrepared_ = true;
    }

    // 发送 outbox 中 PENDING 事件，推进水位；BUFFERED/REJECTED/CONFLICT 下次补发
    auto meta = store_.loadEventOutboxMeta().value_or(FotaEventOutboxMeta{});
    bool sendError = false;
    for (std::uint64_t seq = 1; seq < meta.nextSequenceNo; ++seq) {
        auto e = store_.loadEvent(seq);
        if (!e.has_value()) continue;
        if (e->sendStatus == "ACKED") continue;
        // 旧迁移事件无 payload -> 跳过发送（由对账/最终结果收敛）
        if (e->eventPbHex.empty()) {
            e->sendStatus = "SENT";
            store_.saveEvent(*e);
            continue;
        }
        ::vehicle::fota::v1::ExecutionEvent evt;
        std::string bin = hexToProtoBinary(e->eventPbHex);
        evt.ParseFromString(bin);
        evt.set_sequence_no(e->sequenceNo);
        try {
            auto eresp = cloud_.reportEvent(evt, makeCtx("evt-" + std::to_string(e->sequenceNo)));
            if (eresp.event_disposition() == ::vehicle::fota::v1::EVENT_DISPOSITION_ACCEPTED) {
                e->sendStatus = "ACKED";
                acceptedSeq_ = std::max(acceptedSeq_, e->sequenceNo);
            } else {
                sendError = true;  // BUFFERED/REJECTED/CONFLICT/DUPLICATE -> 补发
            }
        } catch (const FotaCloudException& ex) {
            o.error = ex.what();
            o.transportFailed = true;
            sendError = true;
        }
        store_.saveEvent(*e);
    }
    if (meta.acceptedSequenceNo < acceptedSeq_) {
        meta.acceptedSequenceNo = acceptedSeq_;
        store_.saveEventOutboxMeta(meta);
    }
    persistExecution();

    if (sendError) {
        o.vehicleTaskState = vtState_;
        o.executionState = exState_;
        return o;  // 等待下次 step 补发
    }

    // 全部事件 ACKED 且 executor 完成 -> 最终结果
    return doFinalize();
}

StepOutcome FotaOrchestrator::doFinalize() {
    StepOutcome o;
    o.action = "finalResult";

    auto finalData = exec_.readFinalResult();

    // CR-015 §D5：总体 Result 由执行器最终 ECU 结果与回滚事实决定，不得固定为
    // SUCCEEDED。全部成功（且未回滚）才 SUCCEEDED；任一失败 → FAILED；
    // 回滚成功 → ROLLED_BACK。空 ECU 结果列表不得声称成功。
    bool anyEcuFailed = false;
    bool anyEcuRollback = false;
    for (const auto& er : finalData.ecuResults) {
        if (er.result() != ::vehicle::fota::v1::RESULT_SUCCEEDED) anyEcuFailed = true;
        if (er.has_rollback_result()) anyEcuRollback = true;
    }
    const bool rollbackFacts = finalData.rollbackOccurred || anyEcuRollback ||
                               exState_ == ExecutionState::Rollback;
    const bool allSucceeded = !finalData.ecuResults.empty() && !anyEcuFailed &&
                              !rollbackFacts;
    const auto overallResult =
        rollbackFacts ? ::vehicle::fota::v1::RESULT_ROLLED_BACK
                      : (allSucceeded ? ::vehicle::fota::v1::RESULT_SUCCEEDED
                                      : ::vehicle::fota::v1::RESULT_FAILED);

    ::vehicle::fota::v1::FinalResultReport fr;
    fr.set_final_sequence_no(acceptedSeq_);
    fr.set_last_received_control_revision(lastReceivedControlRev_);
    fr.set_last_applied_control_revision(lastAppliedControlRev_);
    for (const auto& id : pendingControlIds_) fr.add_pending_control_ids(id);
    fr.set_result(overallResult);
    if (!finalData.actualBaselineCode.empty()) {
        fr.set_actual_baseline_code(finalData.actualBaselineCode);
    }
    fr.set_baseline_status(finalData.baselineStatus);
    for (const auto& er : finalData.ecuResults) *fr.add_ecu_results() = er;
    fr.set_started_at_ms(0);
    fr.set_completed_at_ms(nowMs());
    fr.set_rollback_occurred(finalData.rollbackOccurred || anyEcuRollback);
    // 失败摘要：只含安全字段（ECU/stage/稳定错误码）。
    for (const auto& er : finalData.ecuResults) {
        if (er.result() == ::vehicle::fota::v1::RESULT_SUCCEEDED) continue;
        auto* es = fr.add_error_summary();
        es->set_ecu_id(er.ecu_id());
        if (er.has_failure_stage()) es->set_stage(er.failure_stage());
        if (!er.package_id().empty()) es->set_error_code("FOTA-INSTALL-FAILED");
    }
    // CR-015 §D4：结果摘要使用确定性规范化输入（不含序号/时间）。
    fr.mutable_result_digest()->set_algorithm("sha-256");
    fr.mutable_result_digest()->set_value_hex(digest::finalResultDigestHex(fr));

    ::vehicle::fota::v1::FinalResultResponse resp;
    try {
        resp = cloud_.reportFinalResult(fr, makeCtx("final-" + executionId_));
    } catch (const FotaCloudException& e) {
        o.error = e.what();
        o.transportFailed = true;
        return o;
    }

    if (!resp.result_accepted()) {
        o.error = "final result not accepted";
        return o;  // 等待补事件或对账
    }

    // CCU-FOTA-DSN-CR-013 D5 / CR-015 §D6：仅“总体成功且未被回滚”的执行需要登记。
    // 失败/回滚成功不登记，按原终态规则收口。登记未完成时不得关闭 Execution。
    const bool retryPendingTask =
        resp.vehicle_task_status() ==
        ::vehicle::fota::v1::VEHICLE_TASK_STATUS_RETRY_PENDING;
    if (overallResult == ::vehicle::fota::v1::RESULT_SUCCEEDED && !retryPendingTask) {
        if (!registerInstalledVersions(o)) {
            o.vehicleTaskState = vtState_;
            o.executionState = exState_;
            return o;  // 保持 RESULT_PENDING，下次 step 用同一身份重试
        }
    }

    // 日志上传
    doLogUpload();

    // 收口 Execution
    exState_ = retryPendingTask
                   ? ExecutionState::Failed
                   : (overallResult == ::vehicle::fota::v1::RESULT_ROLLED_BACK
                          ? ExecutionState::RolledBack
                          : (overallResult ==
                                     ::vehicle::fota::v1::RESULT_SUCCEEDED
                                 ? ExecutionState::Succeeded
                                 : ExecutionState::Failed));
    vtState_ = fromProtoVehicleTask(resp.vehicle_task_status());
    if (vtState_ == VehicleTaskState::None) {
        vtState_ = VehicleTaskState::Completed;
    }
    persistVehicleTask("finalResult");
    persistExecution();
    FotaLogAdapter::orchestrator().info(
        fota_events::OTA_FINAL_RESULT, "Final result assembled and accepted",
        {flog::f_str("execution_id", executionId_),
         flog::f_int("attempt_no", static_cast<std::int64_t>(attemptNo_)),
         flog::f_str("result",
                     overallResult == ::vehicle::fota::v1::RESULT_SUCCEEDED
                         ? "SUCCEEDED"
                         : (overallResult ==
                                    ::vehicle::fota::v1::RESULT_ROLLED_BACK
                                ? "ROLLED_BACK"
                                : "FAILED")),
         flog::f_int("ecu_result_count",
                     static_cast<std::int64_t>(finalData.ecuResults.size())),
         flog::f_str("registration_id", installedVersionRegistrationId_)});
    o.vehicleTaskState = vtState_;
    o.executionState = exState_;
    o.terminal = isTerminalVehicleTaskState(vtState_);
    return o;
}

StepOutcome FotaOrchestrator::doLogUpload() {
    StepOutcome o;
    o.action = "logUpload";
    ::vehicle::fota::v1::LogGrantRequest lreq;
    std::string logId = "LOG-" + std::to_string(nowMs());
    lreq.set_log_request_id(logId);
    lreq.add_collection_scope("ccu-fota");
    lreq.mutable_log_time_range()->set_start_at_ms(0);
    lreq.mutable_log_time_range()->set_end_at_ms(nowMs());
    lreq.set_redaction_profile_version("v1");
    lreq.set_log_type("fota");
    lreq.set_file_name("fota-" + logId + ".log");
    lreq.set_mime_type("text/plain");
    lreq.set_privacy_level("pii-masked");

    ::vehicle::fota::v1::LogGrantResponse gresp;
    try {
        gresp = cloud_.requestLogUpload(lreq, makeCtx("log-" + frozenTask_.vehicle_task_id()));
    } catch (const FotaCloudException& e) {
        o.error = e.what();
        return o;
    }

    LogCollectScope scope;
    for (const auto& c : lreq.collection_scope()) scope.collectionScope.push_back(c);
    scope.timeRange = lreq.log_time_range();
    scope.redactionProfileVersion = lreq.redaction_profile_version();
    scope.logType = lreq.log_type();

    auto pkg = log_.collect(scope);
    if (!pkg.generated) {
        o.error = "log collect failed: " + pkg.errorCode;
        return o;
    }

    ::vehicle::fota::v1::LogUploadResult lres;
    lres.set_object_key(gresp.object_key());
    lres.set_upload_result(::vehicle::fota::v1::RESULT_SUCCEEDED);
    lres.mutable_actual_file_digest()->set_algorithm(pkg.algorithm.empty() ? "sha-256" : pkg.algorithm);
    lres.mutable_actual_file_digest()->set_value_hex(pkg.digestHex);
    lres.set_uploaded_at_ms(nowMs());
    try {
        cloud_.reportLogUpload(lres, makeCtx("logres-" + logId));
    } catch (const FotaCloudException& e) {
        o.error = e.what();
        return o;
    }

    FotaLogJobRecord jr;
    jr.logRequestId = logId;
    jr.objectKey = gresp.object_key();
    jr.digestHex = pkg.digestHex;
    jr.algorithm = pkg.algorithm.empty() ? "sha-256" : pkg.algorithm;
    jr.sizeBytes = pkg.sizeBytes;
    jr.status = "UPLOADED";
    jr.completedAtMs = nowMs();
    store_.saveLogJob(jr);
    return o;
}

// ---------------------------------------------------------------------------
// 控制指令
// ---------------------------------------------------------------------------
ControlOutcome FotaOrchestrator::applyControl(const ::vehicle::fota::v1::ControlCommand& cmd) {
    ControlOutcome o;
    // durable 保存控制指令（按 controlRevision 去重）
    if (store_.loadControl(cmd.control_revision()).has_value()) {
        o.status = ::vehicle::fota::v1::CONTROL_ACK_STATUS_RECEIVED;
        o.reason = "duplicate controlRevision";
        return o;
    }
    FotaControlRecord ce;
    ce.controlId = cmd.control_id();
    ce.controlRevision = cmd.control_revision();
    ce.action = ::vehicle::fota::v1::ControlAction_Name(cmd.action());
    ce.scope = ::vehicle::fota::v1::ControlScope_Name(cmd.scope());
    ce.applyMode = ::vehicle::fota::v1::ApplyMode_Name(cmd.apply_mode());
    ce.issuedAtMs = cmd.issued_at_ms();
    ce.expiresAtMs = cmd.expires_at_ms();
    ce.reason = cmd.has_reason_code() ? cmd.reason_code() : "";
    store_.saveControl(ce);

    lastReceivedControlRev_ = cmd.control_revision();
    pendingControlIds_.push_back(cmd.control_id());

    auto app = exec_.apply(cmd);
    o.status = app.status;
    o.reason = app.reason;
    if (app.status == ::vehicle::fota::v1::CONTROL_ACK_STATUS_APPLIED) {
        ce.ackStatus = ::vehicle::fota::v1::ControlAckStatus_Name(app.status);
        ce.ackSequenceNo = ackSeq_++;
        ce.appliedAtMs = nowMs();
        store_.saveControl(ce);
        lastAppliedControlRev_ = cmd.control_revision();
        pendingControlIds_.erase(
            std::remove(pendingControlIds_.begin(), pendingControlIds_.end(), cmd.control_id()),
            pendingControlIds_.end());

        // 上报控制回执（独立 ControlAckReport）
        ::vehicle::fota::v1::ControlAckReport ack;
        ack.mutable_ack()->set_control_ack_id("CA-" + std::to_string(cmd.control_revision()));
        ack.mutable_ack()->set_ack_sequence_no(ce.ackSequenceNo);
        ack.mutable_ack()->set_control_id(cmd.control_id());
        ack.mutable_ack()->set_control_revision(cmd.control_revision());
        ack.mutable_ack()->set_status(app.status);
        ack.mutable_ack()->set_processed_at_ms(nowMs());
        if (!app.reason.empty()) ack.mutable_ack()->set_reason_code(app.reason);
        try {
            cloud_.acknowledgeControl(ack, makeCtx("ctrlack-" + std::to_string(cmd.control_revision())));
        } catch (const FotaCloudException&) {
            // 回执失败保留 PENDING，由 outbox/对账补发（MVP 记录）
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// 策略同步
// ---------------------------------------------------------------------------
bool FotaOrchestrator::syncPolicy() {
    auto cur = store_.loadPolicy().value_or(FotaPolicyRecord{});
    ::vehicle::fota::v1::PolicyRequest preq;
    preq.set_local_policy_version(cur.preferenceVersion);
    if (!cur.basePreferenceVersion.empty()) {
        preq.set_base_preference_version(cur.basePreferenceVersion);
    }
    preq.set_updated_at_ms(nowMs());
    ::vehicle::fota::v1::PolicyResponse resp;
    try {
        resp = cloud_.syncPolicy(preq, makeCtx("policy-sync"));
    } catch (const FotaCloudException&) {
        return false;
    }
    FotaPolicyRecord nr;
    nr.localPolicyVersion = resp.policy_version();
    nr.basePreferenceVersion = cur.preferenceVersion;
    nr.preferenceVersion = resp.policy_version();
    std::string bin;
    resp.effective_policy().SerializeToString(&bin);
    nr.effectivePolicyPbHex = protoBinaryToHex(bin);
    nr.conflict = !(resp.has_preference_accepted() && resp.preference_accepted());
    nr.effectiveAtMs = resp.effective_at_ms();
    store_.savePolicy(nr);
    return !nr.conflict;
}

} // namespace ota
} // namespace ccu_fota
