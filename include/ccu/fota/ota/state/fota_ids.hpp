#pragma once

// =============================================================================
// include/ccu/fota/ota/state/fota_ids.hpp
// CCU-FOTA 标识类型别名 (CCU-FOTA-DSN-CR-009 §状态模型 / CR-011 类型校准)
// =============================================================================
// Task、VehicleTask、Execution 三层状态分别建模。任务快照以 taskRevision 管理；
// 包以 packageRevision 管理；安装计划、条件集合、控制指令和策略均使用独立版本。
// 同一 VehicleTask 同时最多一个活动 Execution；重试创建新 executionId/attemptNo。
//
// VEH-PROTO 正式契约：task_revision/control_revision/inventory_revision 为
// uint64；sequence 为 uint64；其余稳定字符串 ID 不变。
// =============================================================================

#include <cstdint>
#include <string>

namespace ccu_fota {
namespace ota {
namespace ids {

using VehicleTaskId     = std::string;
using TaskRevision      = std::uint64_t;
using ExecutionId       = std::string;
using AttemptNo         = std::uint32_t;
using PackageId         = std::string;
using PackageRevision   = std::string;
using ControlRevision   = std::uint64_t;
using SequenceNo        = std::uint64_t;
using StageResultId     = std::string;
using LogRequestId      = std::string;
using ConsentReceiptId  = std::string;
using PermitId          = std::string;
using InventoryRevision = std::uint64_t;
using PreferenceVersion = std::string;

} // namespace ids
} // namespace ota
} // namespace ccu_fota
