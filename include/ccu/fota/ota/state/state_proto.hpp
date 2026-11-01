#pragma once

// =============================================================================
// include/ccu/fota/ota/state/state_proto.hpp
// CCU-FOTA 状态机与 Protobuf 枚举映射 (CCU-FOTA-DSN-CR-009 / CR-011 类型校准)
// =============================================================================
// 内部 C++ 状态枚举与 vehicle.fota.v1 proto 枚举互转。状态机逻辑保持纯 C++，
// 仅在编排边界做映射。
// =============================================================================

#include "ccu/fota/ota/state/execution_state.hpp"
#include "ccu/fota/ota/state/vehicle_task_state.hpp"

#include "vehicle/fota/v1/types.pb.h"

namespace ccu_fota {
namespace ota {

// VehicleTaskState::None 无对应用户态 proto 值（新契约无 VEHICLE_TASK_STATUS_NONE），
// 映射为 UNSPECIFIED（即不设置 local_vehicle_task_status）。
::vehicle::fota::v1::VehicleTaskStatus toProto(VehicleTaskState s);
VehicleTaskState fromProtoVehicleTask(::vehicle::fota::v1::VehicleTaskStatus s);

::vehicle::fota::v1::ExecutionStatus toProto(ExecutionState s);
ExecutionState fromProtoExecution(::vehicle::fota::v1::ExecutionStatus s);

} // namespace ota
} // namespace ccu_fota
