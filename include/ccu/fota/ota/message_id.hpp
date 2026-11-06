#pragma once

// =============================================================================
// include/ccu/fota/ota/message_id.hpp
// CCU-FOTA 传输消息 message_id 生成 (CGW-FOTA-SPEC设计 §13.10.3 / CR-010)
// =============================================================================
// 契约（VEH-PROTO vehicle.common.v1.VehicleMessageEnvelope）：
//   * message_id    : 每条传输消息唯一标识；请求 message_id 全局唯一。
//   * request_id    : 业务请求标识，跨一次业务操作的多个传输消息保持不变。
//   * idempotency_key: 写操作/可重试业务操作的稳定幂等身份。
// 因此 message_id 不得复用 request_id 或 idempotency_key：否则同一业务操作的
// 再次传输（业务重试、状态恢复重投、重启后重新 checkTask）会与历史传输消息
// 同一 ID 而内容不同。云端按 (message_id, 完整 Envelope 摘要) 判定幂等，会将其
// 判为 digest conflict 并转入 DLQ，而不是正常的 duplicate 跳过。
//
// 生成组成：进程实例随机 token（进程重启后重新生成）+ 单调递增计数器 + epoch ms。
// 进程内线程安全；不引入外部唯一性服务或 UUID 库依赖；可在受限环境回退
// （random_device 熵不足时仍有计数器与时间戳保证进程内唯一）。
// 本层不记录 VIN/payload 等敏感内容。
// =============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace ccu_fota {
namespace ota {

namespace message_id_detail {

// 进程实例随机 token：避免重启后计数器与同毫秒时间戳重复。
inline const std::string& instanceToken() {
    static const std::string token = [] {
        std::random_device rd;
        std::seed_seq seq{static_cast<std::uint32_t>(rd()), static_cast<std::uint32_t>(rd()),
                          static_cast<std::uint32_t>(rd()), static_cast<std::uint32_t>(rd())};
        std::mt19937_64 gen(seq);
        const std::uint64_t v = gen();
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%08llx%08llx",
                      static_cast<unsigned long long>(v >> 32),
                      static_cast<unsigned long long>(v & 0xFFFFFFFFULL));
        return std::string(buf);
    }();
    return token;
}

}  // namespace message_id_detail

// 生成一条传输消息的全局唯一 message_id（每次调用都不同）。
inline std::string nextMessageId() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    const auto n = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    return "msg-" + std::to_string(tsMs) + "-" + message_id_detail::instanceToken() +
           "-" + std::to_string(n);
}

}  // namespace ota
}  // namespace ccu_fota
