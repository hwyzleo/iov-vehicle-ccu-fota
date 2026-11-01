#pragma once

// =============================================================================
// include/ccu/fota/ota/call_context.hpp
// CCU-FOTA 车云 OTA 调用上下文 (CCU-FOTA-DSN-CR-009)
// =============================================================================
// 承载跨段传播的 requestId/traceId/idempotencyKey/timeout/deadline。写操作必须
// 携带稳定 idempotencyKey。CloudProxy framework 自动重试关闭；业务重试由
// FotaOrchestrator 使用 durable 状态和同一幂等身份执行。
// =============================================================================

#include <chrono>
#include <cstdint>
#include <string>

namespace ccu_fota {
namespace ota {

// 调用上下文。所有 FotaCloudProxy 方法携带本上下文。
struct CallContext {
    std::string traceId;
    std::string requestId;
    std::string idempotencyKey;        // 写操作幂等键
    std::chrono::milliseconds timeout{10000};
    std::int64_t deadlineMs = 0;       // 绝对截止时间（ms since epoch）；0 表示由 timeout 推导
    std::string deviceId;
    std::string vin;                   // 协议承载；不进业务日志原文
    std::string vehicleTaskId;         // 关联 VehicleTask（Envelope 承载）
    std::string executionId;           // 关联 Execution（安装阶段）

    // 由 timeout 推导绝对 deadline（若未显式设置）。
    std::int64_t resolveDeadline(std::int64_t nowMs) const {
        return deadlineMs > 0 ? deadlineMs : nowMs + timeout.count();
    }
};

} // namespace ota
} // namespace ccu_fota
