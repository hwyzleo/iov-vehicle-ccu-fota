#pragma once

// =============================================================================
// include/ccu/fota/store/fota_state_recovery.hpp
// CCU-FOTA 重启状态恢复器 (CCU-FOTA-DSN-CR-005)
// =============================================================================
// 在开放 SOME/IP 服务与自动采集前，根据持久化的 active_job 阶段执行保守恢复：
//   无 active job            -> 正常启动
//   Accepted / Collecting    -> 以原 reportId 重新采集（序号在 Assembled 阶段分配）
//   Assembled / SubmitPrepared -> 校验序号后以原幂等标识重提交（at-least-once）
//   SubmitUnknown / RetryWaiting -> 保持原 reportId/序号/幂等标识按退避重试
//   CompletedPendingCleanup  -> 与 last_success 对账，匹配则补齐 dedupe 并清理
//   active_job 损坏          -> 分配新序号与幂等标识保守重采集，不得恢复为成功
//   sequence 阻断/回退       -> 阻断，禁止自动归零
//
// 恢复在开放 SOME/IP 和自动采集前完成；恢复任务与新主动请求仍受单在途任务约束，
// 新请求可合并到恢复任务并返回同一 reportId。
// =============================================================================

#include "ccu/fota/store/fota_state.hpp"
#include "ccu/fota/store/fota_state_store.hpp"

#include <optional>
#include <string>

namespace ccu_fota {
namespace store {

// ---------------------------------------------------------------------------
// RecoveryAction - 恢复后业务层应执行的动作
// ---------------------------------------------------------------------------
enum class RecoveryAction {
    None,           // 无在途任务（或已完成清理），正常启动
    ReCollect,      // 以原 reportId 重新采集（Accepted/Collecting 或已完成但未确认）
    Resubmit,       // 以原幂等标识重提交已组装快照（Assembled/SubmitPrepared）
    Retry,          // 以原 reportId/序号/幂等标识按退避重试（SubmitUnknown/RetryWaiting）
    ReCollectFresh, // active_job 损坏：新 reportId+序号保守重采集
    Blocked,        // 序号阻断/回退：停止自动/主动上报
};

// ---------------------------------------------------------------------------
// RecoveryPlan - 恢复结果
// ---------------------------------------------------------------------------
struct RecoveryPlan {
    RecoveryAction action = RecoveryAction::None;
    std::optional<ActiveJobState> job;  // 恢复的在途任务（ReCollect/Resubmit/Retry 时有效）
    std::string reason;                 // 恢复原因（仅含 key/phase/version 摘要，无 payload）

    bool shouldResumeJob() const {
        return action == RecoveryAction::ReCollect ||
               action == RecoveryAction::Resubmit ||
               action == RecoveryAction::Retry;
    }
};

// ---------------------------------------------------------------------------
// StateRecovery - 恢复器
// ---------------------------------------------------------------------------
class StateRecovery {
public:
    // 对 store 执行恢复。可能修改 store（清理已完成 active_job、补齐 dedupe）。
    // 恢复日志只记 key/phase/format_version/attempt/error_code，禁止 payload。
    static RecoveryPlan recover(FotaStateStore& store);

private:
    // CompletedPendingCleanup 对账：匹配 last_success 则补齐 dedupe 并清理。
    // 返回 true 表示已清理（正常启动），false 表示未确认（需重采集）。
    static bool reconcileCompleted(FotaStateStore& store, const ActiveJobState& job);
};

} // namespace store
} // namespace ccu_fota
