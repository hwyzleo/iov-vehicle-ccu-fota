// =============================================================================
// src/ota/state/vehicle_task_state.cpp
// CCU-FOTA VehicleTask 状态机实现 (CCU-FOTA-DSN-CR-009 §13.2)
// =============================================================================

#include "ccu/fota/ota/state/vehicle_task_state.hpp"

#include <cstring>

namespace ccu_fota {
namespace ota {

namespace {
// 文档状态图转换表（from -> to）。
// 额外允许：任意非终态 -> Ended（cancel/supersede/abort 全局逃生）。
// 自转换（同态）始终合法（幂等）。
bool documentedTransition(VehicleTaskState from, VehicleTaskState to) {
    switch (from) {
        case VehicleTaskState::None:
            return to == VehicleTaskState::Discovered;
        case VehicleTaskState::Discovered:
            return to == VehicleTaskState::ConsentPending ||
                   to == VehicleTaskState::DownloadPending ||
                   to == VehicleTaskState::Paused;
        case VehicleTaskState::ConsentPending:
            return to == VehicleTaskState::DownloadPending ||
                   to == VehicleTaskState::Ended;
        case VehicleTaskState::DownloadPending:
            return to == VehicleTaskState::Downloading;
        case VehicleTaskState::Downloading:
            return to == VehicleTaskState::Ready;
        case VehicleTaskState::Ready:
            return to == VehicleTaskState::WaitingWindow ||
                   to == VehicleTaskState::Paused;
        case VehicleTaskState::WaitingWindow:
            return to == VehicleTaskState::PermitPending;
        case VehicleTaskState::PermitPending:
            return to == VehicleTaskState::Executing;
        case VehicleTaskState::Executing:
            return to == VehicleTaskState::RetryPending ||
                   to == VehicleTaskState::RollbackPending ||
                   to == VehicleTaskState::Completed ||
                   to == VehicleTaskState::Paused;
        case VehicleTaskState::RetryPending:
            return to == VehicleTaskState::PermitPending;
        case VehicleTaskState::RollbackPending:
            return to == VehicleTaskState::Executing;
        case VehicleTaskState::Paused:
            return to == VehicleTaskState::Discovered ||
                   to == VehicleTaskState::Ended;
        case VehicleTaskState::Completed:
        case VehicleTaskState::Ended:
            return false; // 终态，无文档转换
    }
    return false;
}
} // namespace

bool isValidVehicleTaskTransition(VehicleTaskState from, VehicleTaskState to) {
    if (from == to) return true; // 幂等自转换
    if (isTerminalVehicleTaskState(from)) return false; // 终态不可离开
    if (to == VehicleTaskState::Ended) return true;     // cancel/supersede/abort 全局逃生
    return documentedTransition(from, to);
}

bool isTerminalVehicleTaskState(VehicleTaskState s) {
    return s == VehicleTaskState::Completed || s == VehicleTaskState::Ended;
}

const char* vehicleTaskStateToString(VehicleTaskState s) {
    switch (s) {
        case VehicleTaskState::None:            return "NONE";
        case VehicleTaskState::Discovered:      return "DISCOVERED";
        case VehicleTaskState::ConsentPending:  return "CONSENT_PENDING";
        case VehicleTaskState::DownloadPending: return "DOWNLOAD_PENDING";
        case VehicleTaskState::Downloading:     return "DOWNLOADING";
        case VehicleTaskState::Ready:           return "READY";
        case VehicleTaskState::WaitingWindow:   return "WAITING_WINDOW";
        case VehicleTaskState::PermitPending:   return "PERMIT_PENDING";
        case VehicleTaskState::Executing:       return "EXECUTING";
        case VehicleTaskState::RetryPending:    return "RETRY_PENDING";
        case VehicleTaskState::RollbackPending: return "ROLLBACK_PENDING";
        case VehicleTaskState::Paused:          return "PAUSED";
        case VehicleTaskState::Completed:       return "COMPLETED";
        case VehicleTaskState::Ended:           return "ENDED";
    }
    return "UNKNOWN";
}

bool vehicleTaskStateFromString(const char* s, VehicleTaskState& out) {
    if (!s) return false;
    // 使用稳定字符串（与 proto VehicleTaskStatus 值名一致，去掉前缀）。
    static const char* kNames[] = {
        "NONE", "DISCOVERED", "CONSENT_PENDING", "DOWNLOAD_PENDING",
        "DOWNLOADING", "READY", "WAITING_WINDOW", "PERMIT_PENDING",
        "EXECUTING", "RETRY_PENDING", "ROLLBACK_PENDING", "PAUSED",
        "COMPLETED", "ENDED"
    };
    for (std::size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        if (std::strcmp(s, kNames[i]) == 0) {
            out = static_cast<VehicleTaskState>(i);
            return true;
        }
    }
    return false;
}

} // namespace ota
} // namespace ccu_fota
