// =============================================================================
// src/store/fota_state_recovery.cpp
// CCU-FOTA 重启状态恢复器实现 (CCU-FOTA-DSN-CR-005)
// =============================================================================

#include "ccu/fota/store/fota_state_recovery.hpp"

#include "constants.h"
#include "fota_log_adapter.h"
#include "fota_log_context.h"

#include <chrono>

namespace ccu_fota {
namespace store {

namespace {
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string phaseLabel(JobPhase p) { return jobPhaseToString(p); }
} // namespace

// ===========================================================================
// recover
// ===========================================================================
RecoveryPlan StateRecovery::recover(FotaStateStore& store) {
    RecoveryPlan plan;

    FotaLogAdapter::store().info(
        fota_events::STORE_RECOVERY_STARTED,
        "State recovery started");

    // 1. 序号阻断 -> 阻断
    if (store.isSequenceBlocked()) {
        plan.action = RecoveryAction::Blocked;
        plan.reason = "sequence blocked";
        FotaLogAdapter::store().error(
            fota_events::STORE_RECOVERY_BLOCKED,
            "Recovery blocked: sequence unavailable",
            {flog::f_str("key", keys::SEQUENCE)});
        return plan;
    }

    // 2. 跨 key 一致性：last_success.snapshotSeq > sequence.highestAllocated -> 回退阻断
    auto seq = store.loadSequence();
    auto lastSuccess = store.loadLastSuccess();
    if (seq.has_value() && lastSuccess.has_value()) {
        if (lastSuccess->snapshotSeq > seq->highestAllocated) {
            plan.action = RecoveryAction::Blocked;
            plan.reason = "sequence rollback detected";
            FotaLogAdapter::store().error(
                fota_events::STORE_RECOVERY_BLOCKED,
                "Recovery blocked: sequence rollback",
                {flog::f_str("key", keys::SEQUENCE),
                 flog::f_int("highest_allocated", static_cast<std::int64_t>(seq->highestAllocated)),
                 flog::f_int("last_success_seq", static_cast<std::int64_t>(lastSuccess->snapshotSeq))});
            return plan;
        }
    }

    // 3. 加载在途任务
    auto job = store.loadActiveJob();
    if (!job.has_value()) {
        if (store.isActiveJobCorrupted()) {
            // active_job 损坏：不得恢复为成功，新序号+幂等标识保守重采集
            plan.action = RecoveryAction::ReCollectFresh;
            plan.reason = "active_job corrupted";
            FotaLogAdapter::store().warn(
                fota_events::STORE_RECOVERY_BLOCKED,
                "active_job corrupted, conservative re-collect",
                {flog::f_str("key", keys::ACTIVE_JOB)});
            FotaLogAdapter::store().info(
                fota_events::STORE_RECOVERY_COMPLETED,
                "Recovery plan: fresh re-collect",
                {flog::f_str("action", "ReCollectFresh")});
            return plan;
        }
        // 无在途任务 -> 正常启动
        plan.action = RecoveryAction::None;
        plan.reason = "no active job";
        FotaLogAdapter::store().info(
            fota_events::STORE_RECOVERY_COMPLETED,
            "Recovery plan: normal start",
            {flog::f_str("action", "None")});
        return plan;
    }

    // 4. 按 phase 恢复
    plan.job = job;
    switch (job->phase) {
        case JobPhase::Accepted:
        case JobPhase::Collecting:
            // 以原 reportId 重新采集；序号在 Assembled 阶段分配
            plan.action = RecoveryAction::ReCollect;
            plan.reason = "re-collect with original reportId";
            break;

        case JobPhase::Assembled:
        case JobPhase::SubmitPrepared:
            // 校验序号后以原幂等标识重提交（at-least-once，禁止换 ID 伪装新任务）
            if (job->snapshotSeq == 0) {
                // 序号未分配（不一致）-> 重新采集
                plan.action = RecoveryAction::ReCollect;
                plan.reason = "assembled but seq unallocated, re-collect";
            } else {
                plan.action = RecoveryAction::Resubmit;
                plan.reason = "resubmit with original idempotencyKey";
            }
            break;

        case JobPhase::SubmitUnknown:
        case JobPhase::RetryWaiting:
            // 保持原 reportId/序号/幂等标识按退避重试；未知结果不得标记成功
            plan.action = RecoveryAction::Retry;
            plan.reason = "retry with original ids, result unknown";
            break;

        case JobPhase::CompletedPendingCleanup:
            // 与 last_success 对账：匹配则补齐 dedupe 并清理；否则保守重采集
            if (reconcileCompleted(store, *job)) {
                plan.action = RecoveryAction::None;
                plan.reason = "completed and cleaned up";
                plan.job.reset();  // 已清理，无在途任务
            } else {
                plan.action = RecoveryAction::ReCollect;
                plan.reason = "last_success mismatch, re-collect";
            }
            break;
    }

    FotaLogAdapter::store().info(
        fota_events::STORE_RECOVERY_COMPLETED,
        "Recovery plan determined",
        {flog::f_str("action", [plan]() {
            switch (plan.action) {
                case RecoveryAction::None:          return "None";
                case RecoveryAction::ReCollect:     return "ReCollect";
                case RecoveryAction::Resubmit:      return "Resubmit";
                case RecoveryAction::Retry:         return "Retry";
                case RecoveryAction::ReCollectFresh:return "ReCollectFresh";
                case RecoveryAction::Blocked:       return "Blocked";
            }
            return "Unknown";
        }()),
         flog::f_str("phase", plan.job ? phaseLabel(plan.job->phase) : "none"),
         flog::f_str("reason", plan.reason)});

    return plan;
}

// ===========================================================================
// reconcileCompleted - CompletedPendingCleanup 对账
// ===========================================================================
bool StateRecovery::reconcileCompleted(FotaStateStore& store, const ActiveJobState& job) {
    auto lastSuccess = store.loadLastSuccess();
    if (!lastSuccess.has_value()) {
        // last_success 缺失或损坏 -> 未确认成功
        return false;
    }
    // 匹配 reportId 和 snapshotSeq
    if (lastSuccess->reportId != job.reportId ||
        lastSuccess->snapshotSeq != job.snapshotSeq) {
        return false;
    }

    // 确认成功：补齐 dedupe（若尚未包含该序号）
    DedupeState dedupe = store.loadDedupe();
    bool found = false;
    for (const auto& e : dedupe.entries) {
        if (e.snapshotSeq == lastSuccess->snapshotSeq &&
            e.reportId == lastSuccess->reportId) {
            found = true;
            break;
        }
    }
    if (!found) {
        DedupeEntry entry;
        entry.requestId = job.requestId;
        entry.reportId = lastSuccess->reportId;
        entry.snapshotSeq = lastSuccess->snapshotSeq;
        entry.fingerprints = lastSuccess->fingerprints;
        entry.overallResult = collectionStatusToString(lastSuccess->overallResult);
        entry.completedAt = lastSuccess->completedAt;
        entry.expiresAt = (dedupe.ttlMs > 0)
            ? lastSuccess->completedAt + dedupe.ttlMs
            : 0;
        dedupe.entries.push_back(entry);
        // 按条目数淘汰
        while (dedupe.entries.size() > dedupe.maxEntries && dedupe.maxEntries > 0) {
            dedupe.entries.erase(dedupe.entries.begin());
        }
        store.saveDedupe(dedupe);
    }

    // 清理 active_job
    store.removeActiveJob();
    return true;
}

} // namespace store
} // namespace ccu_fota
