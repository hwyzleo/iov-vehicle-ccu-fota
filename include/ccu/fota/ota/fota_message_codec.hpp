#pragma once

// =============================================================================
// include/ccu/fota/ota/fota_message_codec.hpp
// CCU-FOTA 消息编解码 (CCU-FOTA-DSN-CR-011 §FotaMessageCodec)
// =============================================================================
// FotaMessageCodec 负责在强类型 vehicle.fota.v1 业务消息与通用传输消息
// （VehicleMessageEnvelope + 不透明 bytes）之间编解码：
//   * 使用统一生成代码序列化/解析（proto 为 wire SSOT；禁止 JSON/YAML 作为量产 codec）。
//   * 业务身份（request_id/device_id/vin/vehicle_task_id/execution_id/
//     idempotency_key）只由 VehicleMessageEnvelope 承载，FOTA payload 不内嵌信封。
//   * 校验 service、payloadType、protocol major、message kind、correlation、TTL、
//     大小与解析状态；未知枚举不得解释为业务成功。
//   * 将 transport cause 与 FOTA 业务错误分层返回（业务错误在各 Response.error_code）。
//
// 本层不得记录原始 payload、VIN、凭据、下载 URL/token 等敏感内容到日志或异常信息。
// =============================================================================

#include "ccu/fota/ota/call_context.hpp"
#include "ccu/fota/ota/message_id.hpp"
#include "ccu/fota/ota/payload_types.hpp"
#include "ccu/fota/ota/vehicle_message_transport.hpp"

#include "vehicle/common/v1/envelope.pb.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ccu_fota {
namespace ota {

// ---------------------------------------------------------------------------
// EnvelopeIdentity - 由调用上下文推导的稳定 Envelope 身份（跨段传播）。
// ---------------------------------------------------------------------------
struct EnvelopeIdentity {
    std::string messageId;         // 本次传输消息 ID（请求全局唯一）
    std::string correlationId;     // 下行/响应需回填的关联 ID（= 请求 messageId）；REQUEST 为空
    std::string requestId;         // 稳定业务请求身份（跨多次传输不变）
    std::string idempotencyKey;    // 写操作幂等键
    std::string vehicleTaskId;
    std::string executionId;
    std::string traceId;
    std::string protocolVersion;
    std::string deviceId;
    std::string vin;               // 协议承载；不进业务日志原文
};

// ---------------------------------------------------------------------------
// FotaProtocolError - 协议/编解码层错误（传输 cause 与业务错误分层）。
// ---------------------------------------------------------------------------
enum class FotaProtocolErrorKind {
    ServiceMismatch,     // service 与期望不匹配
    PayloadTypeMismatch, // payloadType 与期望不匹配
    VersionMismatch,     // protocolVersion 不兼容
    MessageKindMismatch, // 消息方向/形态不符
    CorrelationMismatch, // correlationId 缺失/不匹配
    TtlExpired,          // 消息已过期
    MalformedPayload,    // payload 解析失败
    MissingField,        // 必要字段缺失
    Internal,            // 其他内部错误
};

struct FotaProtocolError {
    FotaProtocolErrorKind kind = FotaProtocolErrorKind::Internal;
    std::string detail;                 // 不含 payload/敏感内容
    std::string frameworkCauseCode;     // 保留 CCU-FW-03xx 链
};

// 带错误的结果。ok 为 true 时 value 有效。
template <typename T>
struct Expected {
    bool ok = false;
    T value{};
    FotaProtocolError error;

    explicit operator bool() const { return ok; }
    T& operator*() { return value; }
    const T& operator*() const { return value; }
    T* operator->() { return &value; }
    const T* operator->() const { return &value; }
};

// payload bytes <-> string 互转（不复制语义，仅类型转换）。
inline std::string bytesToStdString(const std::vector<std::byte>& bytes) {
    if (bytes.empty()) return {};
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

inline std::vector<std::byte> stdStringToBytes(const std::string& s) {
    std::vector<std::byte> out(s.size());
    std::transform(s.begin(), s.end(), out.begin(),
                   [](char c) { return std::byte(static_cast<unsigned char>(c)); });
    return out;
}

// ---------------------------------------------------------------------------
// FotaMessageCodec - 业务类型 <-> 通用 Envelope+bytes。
// ---------------------------------------------------------------------------
class FotaMessageCodec {
public:
    explicit FotaMessageCodec(std::string expectedProtocolVersion = std::string(payload_type::kProtocolVersion))
        : expectedProtocolVersion_(std::move(expectedProtocolVersion))
        , expectedService_(std::string(payload_type::kService)) {}

    // 从调用上下文推导 Envelope 身份（业务 payload 不再内嵌信封）。
    //
    // §13.10.3 契约：请求 message_id 全局唯一（每条传输消息一个），request_id 才是
    // 跨多次传输稳定不变的业务身份，二者不得同值。若复用 request_id（如
    // "req-checkTask-<vehicle_task_id>"，且 vehicle_task_id 为空时退化为常量），
    // 同一业务操作的再次传输会与历史消息同 ID 而内容不同，云端按
    // (message_id, Envelope 摘要) 判为 digest conflict 并进 DLQ。
    // 每次调用生成新的 message_id：重试/恢复重投携带新的传输消息身份，
    // 幂等语义由 request_id + idempotency_key 承载。
    EnvelopeIdentity identityFrom(const CallContext& ctx) const {
        EnvelopeIdentity ids;
        ids.messageId = nextMessageId();
        // REQUEST 不携带 correlation_id；RESPONSE 由云端回填请求 message_id。
        ids.correlationId.clear();
        ids.requestId = ctx.requestId;
        ids.idempotencyKey = ctx.idempotencyKey;
        ids.vehicleTaskId = ctx.vehicleTaskId;
        ids.executionId = ctx.executionId;
        ids.traceId = ctx.traceId;
        ids.protocolVersion = expectedProtocolVersion_;
        ids.deviceId = ctx.deviceId;
        ids.vin = ctx.vin;
        return ids;
    }

    // 编码业务请求 -> 通用传输消息（Envelope + payload bytes）。
    template <typename Request>
    VehicleMessage encodeRequest(std::string_view payload_type, const Request& request,
                                 const EnvelopeIdentity& ids, const CallContext& ctx) const {
        VehicleMessage msg;
        auto& env = msg.envelope;
        env.set_request_id(ids.requestId);
        env.set_message_id(ids.messageId);
        if (!ids.correlationId.empty()) env.set_correlation_id(ids.correlationId);
        env.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_REQUEST);
        env.set_service(std::string(payload_type::kService));
        env.set_payload_type(std::string(payload_type));
        env.set_protocol_version(ids.protocolVersion);
        env.set_timestamp_ms(nowMs());
        env.set_expire_at_ms(ctx.resolveDeadline(nowMs()));
        env.set_device_id(ids.deviceId);
        env.set_vin(ids.vin);
        if (!ids.idempotencyKey.empty()) env.set_idempotency_key(ids.idempotencyKey);
        if (!ids.vehicleTaskId.empty()) env.set_vehicle_task_id(ids.vehicleTaskId);
        if (!ids.executionId.empty()) env.set_execution_id(ids.executionId);
        if (!ids.traceId.empty()) env.mutable_trace_context()->set_trace_id(ids.traceId);

        std::string payload;
        if (!request.SerializeToString(&payload)) {
            // 编码失败：空 payload，由适配层按协议错误处理。
            msg.envelope.set_payload_type(std::string(payload_type));
        }
        msg.payload = stdStringToBytes(payload);
        return msg;
    }

    // 解码业务响应。校验 service / message kind / correlation / payloadType / version / TTL 后解析。
    template <typename Response>
    Expected<Response> decodeResponse(std::string_view expected_payload_type,
                                      const VehicleMessage& response,
                                      std::string_view expectedCorrelationId) const {
        return decode<Response>(::vehicle::common::v1::MESSAGE_KIND_RESPONSE,
                                expected_payload_type, response, expectedCorrelationId);
    }

    // 解码下行消息（事件形态）。用于 subscribe 下行回调按 payloadType 解码。
    template <typename Message>
    Expected<Message> decodeDownlink(std::string_view expected_payload_type,
                                     const VehicleMessage& msg,
                                     std::string_view expectedCorrelationId = {}) const {
        return decode<Message>(::vehicle::common::v1::MESSAGE_KIND_EVENT,
                               expected_payload_type, msg, expectedCorrelationId);
    }

private:
    std::string expectedProtocolVersion_;
    std::string expectedService_;

    static std::int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static FotaProtocolError makeError(FotaProtocolErrorKind kind, std::string detail) {
        FotaProtocolError e;
        e.kind = kind;
        e.detail = std::move(detail);
        e.frameworkCauseCode = "CCU-FW-0306";
        return e;
    }

    bool compatibleVersion(std::string_view candidate) const {
        // 当前 MVP 为精确匹配；Envelope major 不兼容变化必须建立链路配对 CR。
        return candidate == expectedProtocolVersion_;
    }

    template <typename Message>
    Expected<Message> decode(::vehicle::common::v1::MessageKind expectedKind,
                             std::string_view expected_payload_type,
                             const VehicleMessage& msg,
                             std::string_view expectedCorrelationId) const {
        Expected<Message> out;
        const auto& env = msg.envelope;

        if (env.service() != expectedService_) {
            out.error = makeError(FotaProtocolErrorKind::ServiceMismatch,
                                  "service mismatch");
            return out;
        }
        if (env.message_kind() != expectedKind) {
            out.error = makeError(FotaProtocolErrorKind::MessageKindMismatch,
                                  "message kind mismatch");
            return out;
        }
        if (env.payload_type() != expected_payload_type) {
            out.error = makeError(FotaProtocolErrorKind::PayloadTypeMismatch,
                                  "payload_type mismatch");
            return out;
        }
        if (!compatibleVersion(env.protocol_version())) {
            out.error = makeError(FotaProtocolErrorKind::VersionMismatch,
                                  "protocol_version mismatch");
            return out;
        }
        if (!expectedCorrelationId.empty() && env.correlation_id() != expectedCorrelationId) {
            out.error = makeError(FotaProtocolErrorKind::CorrelationMismatch,
                                  "correlation_id mismatch");
            return out;
        }
        if (env.expire_at_ms() > 0 && env.expire_at_ms() < nowMs()) {
            out.error = makeError(FotaProtocolErrorKind::TtlExpired, "message expired");
            return out;
        }
        if (msg.payload.empty()) {
            out.error = makeError(FotaProtocolErrorKind::MissingField, "empty payload");
            return out;
        }
        std::string raw = bytesToStdString(msg.payload);
        if (!out.value.ParseFromString(raw)) {
            out.error = makeError(FotaProtocolErrorKind::MalformedPayload,
                                  "payload parse failed");
            return out;
        }
        out.ok = true;
        return out;
    }
};

} // namespace ota
} // namespace ccu_fota
