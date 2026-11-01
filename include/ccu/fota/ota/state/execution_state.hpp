#pragma once

// =============================================================================
// include/ccu/fota/ota/state/execution_state.hpp
// CCU-FOTA Execution 状态机 (CCU-FOTA-DSN-CR-009 §状态模型, US-014/US-015)
// =============================================================================
// Execution：单次安装尝试。同一 VehicleTask 同时最多一个活动 Execution；
// 重试创建新 executionId/attemptNo。
// 流程：PERMIT_PERSISTED -> READY -> INSTALL_STARTED -> INSTALL/REBOOT/POST_CHECK/
//   ROLLBACK -> RESULT_PENDING -> SUCCEEDED/FAILED/ROLLED_BACK/CANCELED。
// =============================================================================

#include <cstdint>

namespace ccu_fota {
namespace ota {

enum class ExecutionState : std::uint8_t {
    PermitPersisted,  // 许可已持久化
    Ready,            // 执行器就绪
    InstallStarted,   // 安装已开始（validUntil 不再约束）
    Install,          // 安装阶段
    Reboot,           // 重启阶段
    PostCheck,        // 后检查阶段
    Rollback,         // 回滚阶段
    ResultPending,    // 等待最终结果确认
    Succeeded,        // 成功（终态）
    Failed,           // 失败（终态）
    RolledBack,       // 已回滚（终态）
    Canceled,         // 已取消（终态）
};

// 状态转换是否合法（依据 §13.2 Execution 状态）。
bool isValidExecutionTransition(ExecutionState from, ExecutionState to);

// 是否终态。
bool isTerminalExecutionState(ExecutionState s);

// 状态名（稳定字符串）。
const char* executionStateToString(ExecutionState s);

// 由字符串解析状态；失败返回 false。
bool executionStateFromString(const char* s, ExecutionState& out);

} // namespace ota
} // namespace ccu_fota
