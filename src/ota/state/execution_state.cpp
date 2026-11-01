// =============================================================================
// src/ota/state/execution_state.cpp
// CCU-FOTA Execution 状态机实现 (CCU-FOTA-DSN-CR-009 §13.2)
// =============================================================================

#include "ccu/fota/ota/state/execution_state.hpp"

#include <cstring>

namespace ccu_fota {
namespace ota {

bool isValidExecutionTransition(ExecutionState from, ExecutionState to) {
    if (from == to) return true; // 幂等自转换
    if (isTerminalExecutionState(from)) return false; // 终态不可离开
    // 任意非终态可被 ABORT 取消。
    if (to == ExecutionState::Canceled) return true;
    switch (from) {
        case ExecutionState::PermitPersisted:
            return to == ExecutionState::Ready;
        case ExecutionState::Ready:
            return to == ExecutionState::InstallStarted;
        case ExecutionState::InstallStarted:
            return to == ExecutionState::Install;
        case ExecutionState::Install:
            return to == ExecutionState::Reboot ||
                   to == ExecutionState::Rollback ||
                   to == ExecutionState::ResultPending;
        case ExecutionState::Reboot:
            return to == ExecutionState::PostCheck;
        case ExecutionState::PostCheck:
            return to == ExecutionState::ResultPending ||
                   to == ExecutionState::Rollback;
        case ExecutionState::Rollback:
            return to == ExecutionState::ResultPending;
        case ExecutionState::ResultPending:
            return to == ExecutionState::Succeeded ||
                   to == ExecutionState::Failed ||
                   to == ExecutionState::RolledBack;
        case ExecutionState::Succeeded:
        case ExecutionState::Failed:
        case ExecutionState::RolledBack:
        case ExecutionState::Canceled:
            return false; // 终态
    }
    return false;
}

bool isTerminalExecutionState(ExecutionState s) {
    return s == ExecutionState::Succeeded ||
           s == ExecutionState::Failed ||
           s == ExecutionState::RolledBack ||
           s == ExecutionState::Canceled;
}

const char* executionStateToString(ExecutionState s) {
    switch (s) {
        case ExecutionState::PermitPersisted: return "PERMIT_PERSISTED";
        case ExecutionState::Ready:           return "READY";
        case ExecutionState::InstallStarted:  return "INSTALL_STARTED";
        case ExecutionState::Install:         return "INSTALL";
        case ExecutionState::Reboot:          return "REBOOT";
        case ExecutionState::PostCheck:       return "POST_CHECK";
        case ExecutionState::Rollback:        return "ROLLBACK";
        case ExecutionState::ResultPending:   return "RESULT_PENDING";
        case ExecutionState::Succeeded:       return "SUCCEEDED";
        case ExecutionState::Failed:          return "FAILED";
        case ExecutionState::RolledBack:      return "ROLLED_BACK";
        case ExecutionState::Canceled:        return "CANCELED";
    }
    return "UNKNOWN";
}

bool executionStateFromString(const char* s, ExecutionState& out) {
    if (!s) return false;
    static const char* kNames[] = {
        "PERMIT_PERSISTED", "READY", "INSTALL_STARTED", "INSTALL",
        "REBOOT", "POST_CHECK", "ROLLBACK", "RESULT_PENDING",
        "SUCCEEDED", "FAILED", "ROLLED_BACK", "CANCELED"
    };
    for (std::size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        if (std::strcmp(s, kNames[i]) == 0) {
            out = static_cast<ExecutionState>(i);
            return true;
        }
    }
    return false;
}

} // namespace ota
} // namespace ccu_fota
