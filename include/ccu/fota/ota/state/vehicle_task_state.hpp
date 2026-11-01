#pragma once

// =============================================================================
// include/ccu/fota/ota/state/vehicle_task_state.hpp
// CCU-FOTA VehicleTask 状态机 (CCU-FOTA-DSN-CR-009 §状态模型, US-011)
// =============================================================================
// VehicleTask：单 VIN 生命周期，覆盖发现、授权、下载准备、等待窗口、执行、
// 重试/回滚等待和终态。Task/Campaign 状态只作为云端控制输入；每次安装尝试单独
// 建模为 Execution，禁止用 VehicleTask 状态替代 Execution 状态。
//
// 状态转换合法性见 isValidVehicleTaskTransition()。终态：COMPLETED、ENDED。
// 同一 VehicleTask 同时只允许一个活动 Execution；重试创建新 executionId/attemptNo。
// =============================================================================

#include <cstdint>

namespace ccu_fota {
namespace ota {

enum class VehicleTaskState : std::uint8_t {
    None,             // 无任务
    Discovered,       // 任务已接受
    ConsentPending,   // 等待授权
    DownloadPending,  // 等待下载
    Downloading,      // 下载中
    Ready,            // 全部 stage 结果已接受
    WaitingWindow,    // 等待安装窗口
    PermitPending,    // 等待安装许可
    Executing,        // 执行中
    RetryPending,     // 等待重试
    RollbackPending,  // 等待回滚
    Paused,           // 暂停
    Completed,        // 完成（终态）
    Ended,            // 结束（终态：拒绝/撤回/取消/取代/中止）
};

// 状态转换是否合法（依据 §13.2 状态图）。
bool isValidVehicleTaskTransition(VehicleTaskState from, VehicleTaskState to);

// 是否终态。
bool isTerminalVehicleTaskState(VehicleTaskState s);

// 状态名（稳定字符串，用于日志/序列化）。
const char* vehicleTaskStateToString(VehicleTaskState s);

// 由字符串解析状态；失败返回 false。
bool vehicleTaskStateFromString(const char* s, VehicleTaskState& out);

} // namespace ota
} // namespace ccu_fota
