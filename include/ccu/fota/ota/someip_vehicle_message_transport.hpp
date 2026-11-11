#pragma once

// =============================================================================
// include/ccu/fota/ota/someip_vehicle_message_transport.hpp
// CCU-FOTA 量产 SOME/IP 通用车云消息传输 (CCU-FOTA-DSN-CR-010 §组件设计 / CR-011)
// =============================================================================
// 与测试 FakeVehicleMessageTransport 实现同一 VehicleMessageTransport 端口。
// 通过 ccu-framework-someip 调用 TBOX-SOMEIP 聚合服务（寻址由
// TboxGenericTransportAddress 承载，来自 Registry/IDL/运行配置，fail-closed）。
//
// 语义约束（CR-010 §接口设计 / US-020~024）：
//   * 请求 wire = 单一序列化的 vehicle.common.v1.VehicleMessageEnvelope（payload
//     字段承载不透明 vehicle.fota.v1 bytes）；本层不解析业务 payload。
//   * 响应 wire = TBOX VehicleMessageResponseFrame：outcome(1B) +
//     error_code(2B, BE) + envelope_length(4B, BE) + 可选 RESPONSE Envelope。
//     必须先解帧，再按 outcome 映射；不得把整个 Method 响应当裸 Envelope
//     （CCU-FOTA-DSN-CR-016 / TBOX-SOMEIP-DSN-CR-008）。
//   * exchange/publish 只表示传输层受理，SOME/IP 成功 / TBOX accepted / MQTT
//     PUBACK 不得映射为 FOTA 业务成功（业务成功由各 Response 业务字段表达）。
//   * exchange 维护有界 in-flight correlation 表并校验 response correlation_id；
//     迟到/重复/错配响应不得完成其他调用。
//   * subscribe 使用有界下行队列 + 独立 executor 线程；回调绝不在 SOME/IP I/O
//     线程推进 FotaOrchestrator。
//   * framework Method 自动重试保持关闭（RetryMode::None）；超时/unknown 由
//     FotaOrchestrator 用 durable 状态与同一幂等身份重试。
//   * 本层不得记录原始 payload、VIN、凭据、下载 URL/token 等敏感内容到日志。
// =============================================================================

#include "ccu/fota/ota/payload_types.hpp"
#include "ccu/fota/ota/vehicle_message_transport.hpp"
#include "ccu/fota/someip/tbox_generic_transport_contract.hpp"
#include "ccu/fota/config/fota_config.hpp"

#include "ccu/fw/someip/client.hpp"
#include "ccu/fw/someip/types.hpp"

#include "vehicle/common/v1/envelope.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ccu_fota {
namespace ota {

// ---------------------------------------------------------------------------
// 传输结果映射：framework CallResult -> TransportOutcome。
// SOME/IP 成功只映射为 Accepted（本地受理），不代表业务成功。
// ---------------------------------------------------------------------------
TransportOutcome mapCallResultToOutcome(const ccu::fw::someip::CallResult& result);

// 出站 Envelope 校验（exchange/publish 前）：方向/服务/版本/关联/TTL/大小。
TransportOutcome validateOutboundEnvelope(
    const ::vehicle::common::v1::VehicleMessageEnvelope& env,
    std::size_t payloadSize, std::size_t maxPayloadBytes,
    std::size_t maxEnvelopeBytes);

// 入站 Envelope 校验（响应/下行后）：方向/服务/版本/TTL/大小。
TransportOutcome validateInboundEnvelope(
    const ::vehicle::common::v1::VehicleMessageEnvelope& env,
    std::size_t payloadSize, std::size_t maxPayloadBytes,
    std::size_t maxEnvelopeBytes);

// ---------------------------------------------------------------------------
// SomeIpVehicleMessageTransport - 量产 SOME/IP 通用车云消息传输。
// 生命周期：start() 开放（requestService + 下行订阅 + worker），stop() 收敛
// （拒绝新调用 -> 取消下行订阅 -> 有界等待 in-flight -> releaseService）。
// ---------------------------------------------------------------------------
class SomeIpVehicleMessageTransport : public VehicleMessageTransport {
public:
    // address 必须 fullyAllocated（由 resolveTboxGenericTransport 校验）。
    SomeIpVehicleMessageTransport(ccu::fw::someip::Client client,
                                  const ccu_fota::someip::TboxGenericTransportAddress& address,
                                  SomeIpTransportConfig cfg);
    ~SomeIpVehicleMessageTransport() override;

    SomeIpVehicleMessageTransport(const SomeIpVehicleMessageTransport&) = delete;
    SomeIpVehicleMessageTransport& operator=(const SomeIpVehicleMessageTransport&) = delete;

    // ---- VehicleMessageTransport 端口 ----
    TransportResult<VehicleMessage> exchange(const VehicleMessage& msg,
                                             const ExchangeOptions& opts,
                                             const CallContext& ctx) override;
    TransportResult<void> publish(const VehicleMessage& msg,
                                  const CallContext& ctx) override;
    Subscription subscribe(std::string_view service, DownlinkHandler handler) override;

    // ---- 生命周期 ----
    void start();   // requestService + 有界等待可用 + 下行订阅 + 启动 worker
    void stop();    // 关闭顺序：拒绝新调用 -> 取消下行订阅 -> 收敛 in-flight -> release

    // ---- 诊断/测试 ----
    ccu::fw::someip::Availability availability() const;
    std::size_t inflightCount() const;
    std::size_t downlinkQueueDepth() const;
    std::uint64_t downlinkDropped() const;
    bool stopping() const { return stopping_.load(); }

private:
    struct InflightEntry {
        std::int64_t deadlineMs = 0;
        std::uint64_t seq = 0;
    };

    ccu::fw::someip::Client client_;
    ccu_fota::someip::TboxGenericTransportAddress address_;
    SomeIpTransportConfig cfg_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};

    // 有界 in-flight correlation 表。
    mutable std::mutex inflightMutex_;
    std::map<std::string, InflightEntry> inflight_;
    std::uint64_t inflightSeq_ = 0;

    // 下行订阅者（按 service）。
    struct SubEntry {
        std::string service;
        DownlinkHandler handler;
    };
    mutable std::mutex handlersMutex_;
    std::vector<SubEntry> handlers_;

    // 有界下行队列 + 独立 executor 线程。
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<VehicleMessage> downlinkQueue_;
    std::vector<std::thread> workers_;
    bool queueStopping_ = false;
    std::uint64_t downlinkDropped_ = 0;

    ccu::fw::someip::Subscription downlinkSub_;

    // correlation 表管理。
    bool reserveInflight(const std::string& messageId, std::int64_t deadlineMs);
    void releaseInflight(const std::string& messageId);
    void evictExpiredInflight();

    // 下行投递（worker 线程调用）。
    void dispatchToHandlers(VehicleMessage&& msg);
    void workerLoop();

    // SOME/IP 事件回调（框架 executor 上；只做校验+入队，不推进业务）。
    void onDownlinkEvent(const ccu::fw::someip::EventContext&, ccu::fw::someip::PayloadView);
};

} // namespace ota
} // namespace ccu_fota
