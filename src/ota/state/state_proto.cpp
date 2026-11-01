// =============================================================================
// src/ota/state/state_proto.cpp
// CCU-FOTA 状态机与 Protobuf 枚举映射 (CCU-FOTA-DSN-CR-011)
// =============================================================================

#include "ccu/fota/ota/state/state_proto.hpp"

namespace ccu_fota {
namespace ota {

::vehicle::fota::v1::VehicleTaskStatus toProto(VehicleTaskState s) {
    switch (s) {
        case VehicleTaskState::None:            return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_UNSPECIFIED;
        case VehicleTaskState::Discovered:      return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_DISCOVERED;
        case VehicleTaskState::ConsentPending:  return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_CONSENT_PENDING;
        case VehicleTaskState::DownloadPending: return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_DOWNLOAD_PENDING;
        case VehicleTaskState::Downloading:     return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_DOWNLOADING;
        case VehicleTaskState::Ready:           return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_READY;
        case VehicleTaskState::WaitingWindow:   return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_WAITING_WINDOW;
        case VehicleTaskState::PermitPending:   return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_PERMIT_PENDING;
        case VehicleTaskState::Executing:       return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_EXECUTING;
        case VehicleTaskState::RetryPending:    return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_RETRY_PENDING;
        case VehicleTaskState::RollbackPending: return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_ROLLBACK_PENDING;
        case VehicleTaskState::Paused:          return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_PAUSED;
        case VehicleTaskState::Completed:       return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_COMPLETED;
        case VehicleTaskState::Ended:           return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_ENDED;
    }
    return ::vehicle::fota::v1::VEHICLE_TASK_STATUS_UNSPECIFIED;
}

VehicleTaskState fromProtoVehicleTask(::vehicle::fota::v1::VehicleTaskStatus s) {
    switch (s) {
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_UNSPECIFIED:   return VehicleTaskState::None;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_DISCOVERED:      return VehicleTaskState::Discovered;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_CONSENT_PENDING: return VehicleTaskState::ConsentPending;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_DOWNLOAD_PENDING:return VehicleTaskState::DownloadPending;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_DOWNLOADING:     return VehicleTaskState::Downloading;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_READY:           return VehicleTaskState::Ready;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_WAITING_WINDOW:  return VehicleTaskState::WaitingWindow;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_PERMIT_PENDING:  return VehicleTaskState::PermitPending;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_EXECUTING:       return VehicleTaskState::Executing;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_RETRY_PENDING:   return VehicleTaskState::RetryPending;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_ROLLBACK_PENDING:return VehicleTaskState::RollbackPending;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_PAUSED:          return VehicleTaskState::Paused;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_COMPLETED:       return VehicleTaskState::Completed;
        case ::vehicle::fota::v1::VEHICLE_TASK_STATUS_ENDED:           return VehicleTaskState::Ended;
        default: return VehicleTaskState::None;
    }
}

::vehicle::fota::v1::ExecutionStatus toProto(ExecutionState s) {
    switch (s) {
        case ExecutionState::PermitPersisted: return ::vehicle::fota::v1::EXECUTION_STATUS_PERMIT_PERSISTED;
        case ExecutionState::Ready:           return ::vehicle::fota::v1::EXECUTION_STATUS_READY;
        case ExecutionState::InstallStarted:  return ::vehicle::fota::v1::EXECUTION_STATUS_INSTALL_STARTED;
        case ExecutionState::Install:         return ::vehicle::fota::v1::EXECUTION_STATUS_INSTALLING;
        case ExecutionState::Reboot:          return ::vehicle::fota::v1::EXECUTION_STATUS_REBOOTING;
        case ExecutionState::PostCheck:       return ::vehicle::fota::v1::EXECUTION_STATUS_POST_CHECKING;
        case ExecutionState::Rollback:        return ::vehicle::fota::v1::EXECUTION_STATUS_ROLLING_BACK;
        case ExecutionState::ResultPending:   return ::vehicle::fota::v1::EXECUTION_STATUS_RESULT_PENDING;
        case ExecutionState::Succeeded:       return ::vehicle::fota::v1::EXECUTION_STATUS_SUCCEEDED;
        case ExecutionState::Failed:          return ::vehicle::fota::v1::EXECUTION_STATUS_FAILED;
        case ExecutionState::RolledBack:      return ::vehicle::fota::v1::EXECUTION_STATUS_ROLLED_BACK;
        case ExecutionState::Canceled:        return ::vehicle::fota::v1::EXECUTION_STATUS_CANCELED;
    }
    return ::vehicle::fota::v1::EXECUTION_STATUS_UNSPECIFIED;
}

ExecutionState fromProtoExecution(::vehicle::fota::v1::ExecutionStatus s) {
    switch (s) {
        case ::vehicle::fota::v1::EXECUTION_STATUS_PERMIT_PERSISTED: return ExecutionState::PermitPersisted;
        case ::vehicle::fota::v1::EXECUTION_STATUS_READY:            return ExecutionState::Ready;
        case ::vehicle::fota::v1::EXECUTION_STATUS_INSTALL_STARTED:  return ExecutionState::InstallStarted;
        case ::vehicle::fota::v1::EXECUTION_STATUS_INSTALLING:       return ExecutionState::Install;
        case ::vehicle::fota::v1::EXECUTION_STATUS_REBOOTING:        return ExecutionState::Reboot;
        case ::vehicle::fota::v1::EXECUTION_STATUS_POST_CHECKING:    return ExecutionState::PostCheck;
        case ::vehicle::fota::v1::EXECUTION_STATUS_ROLLING_BACK:     return ExecutionState::Rollback;
        case ::vehicle::fota::v1::EXECUTION_STATUS_RESULT_PENDING:   return ExecutionState::ResultPending;
        case ::vehicle::fota::v1::EXECUTION_STATUS_SUCCEEDED:        return ExecutionState::Succeeded;
        case ::vehicle::fota::v1::EXECUTION_STATUS_FAILED:           return ExecutionState::Failed;
        case ::vehicle::fota::v1::EXECUTION_STATUS_ROLLED_BACK:      return ExecutionState::RolledBack;
        case ::vehicle::fota::v1::EXECUTION_STATUS_CANCELED:         return ExecutionState::Canceled;
        case ::vehicle::fota::v1::EXECUTION_STATUS_PAUSED:           return ExecutionState::Canceled;  // 内部无 Paused，保守映射为 Canceled
        default: return ExecutionState::PermitPersisted;
    }
}

} // namespace ota
} // namespace ccu_fota
