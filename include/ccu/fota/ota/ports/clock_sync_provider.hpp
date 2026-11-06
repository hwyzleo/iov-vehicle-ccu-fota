#pragma once

// =============================================================================
// include/ccu/fota/ota/ports/clock_sync_provider.hpp
// CCU-FOTA 系统时钟同步就绪端口
// =============================================================================
// 每条上行 VehicleMessageEnvelope 都携带由 CLOCK_REALTIME 推导的绝对时间字段
// （timestamp_ms、expire_at_ms = now + timeout）。CCU 上电后 RTC 可能尚未与可信
// 时间源同步（掉电回落到 1970/出厂时间，或 NTP 尚未收敛），此时推导出的
// expire_at_ms 落在过去，云侧网关按契约判定 MESSAGE_EXPIRED 并丢入 DLQ
// （上行永久丢失，且车端看不到业务回执）。
//
// 本端口把"系统时钟是否可信"抽象为独立判定来源，供 FotaOrchestrator 在推进任何
// 上行动作（含启动期的版本/清单上报）之前做 fail-closed 门禁：时钟未同步则不上行，
// 只等待并重试，不发出注定被云端拒绝的消息。
//
// 实现约束：
//   * 只读判定，不得修改系统时间（授时是平台/时间服务职责，不是 FOTA 职责）。
//   * 判定结果不含 VIN/device_sn/token 等敏感内容，可直接进日志。
// =============================================================================

#include <cstdint>

namespace ccu_fota {
namespace ota {

// 时钟就绪判定结果（字段均为非敏感诊断信息）。
struct ClockSyncStatus {
    // 系统实时钟是否已与可信时间源同步（false => 绝对时间字段不可用于协议）。
    bool synchronized = false;
    // 判定时刻的 CLOCK_REALTIME（ms since epoch），用于定位 RTC 回落/偏移。
    std::int64_t systemTimeMs = 0;
    // 判定依据或未就绪原因（静态字面量，无敏感内容）。
    const char* detail = "";
};

// 时钟同步就绪提供者。
class ClockSyncProvider {
public:
    virtual ~ClockSyncProvider() = default;

    // 查询当前时钟就绪状态。实现必须是只读且可被频繁调用（轮询语义）。
    virtual ClockSyncStatus status() = 0;
};

} // namespace ota
} // namespace ccu_fota
