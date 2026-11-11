// =============================================================================
// src/ota/someip_vehicle_message_transport.cpp
// CCU-FOTA 量产 SOME/IP 通用车云消息传输实现 (CCU-FOTA-DSN-CR-010/011)
// =============================================================================

#include "ccu/fota/ota/someip_vehicle_message_transport.hpp"

#include "fota_log_adapter.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace ccu_fota {
namespace ota {

using ::vehicle::common::v1::MESSAGE_KIND_EVENT;
using ::vehicle::common::v1::MESSAGE_KIND_REQUEST;
using ::vehicle::common::v1::MESSAGE_KIND_RESPONSE;
using ::vehicle::common::v1::VehicleMessageEnvelope;

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// wire = 单一序列化的 Envelope；payload 字段承载不透明业务 bytes（本层不解析）。
std::vector<std::uint8_t> encodeWire(const VehicleMessage& msg) {
    VehicleMessageEnvelope env = msg.envelope;
    if (!msg.payload.empty()) {
        env.set_payload(std::string(
            reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size()));
    }
    std::string out;
    env.SerializeToString(&out);
    return std::vector<std::uint8_t>(out.begin(), out.end());
}

bool decodeWire(const std::vector<std::uint8_t>& bytes, VehicleMessage& out) {
    if (bytes.empty() ||
        !out.envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return false;
    }
    const auto& pay = out.envelope.payload();
    out.payload.resize(pay.size());
    std::transform(pay.begin(), pay.end(), out.payload.begin(),
                   [](char c) { return std::byte(static_cast<unsigned char>(c)); });
    return true;
}

bool expired(const VehicleMessageEnvelope& env) {
    return env.expire_at_ms() > 0 && env.expire_at_ms() < nowMs();
}

// ---------------------------------------------------------------------------
// TBOX 通用 Method 响应帧（CCU-FOTA-DSN-CR-016 / TBOX-SOMEIP-DSN-CR-008）
//
// wire = outcome(1B) + error_code(2B, BE) + envelope_length(4B, BE)
//        + response Envelope(envelope_length > 0)
// 请求方向仍是单一序列化的 Envelope；响应方向必须消费该帧，不能把整个
// Method 响应当裸 Envelope 解析。
// ---------------------------------------------------------------------------
constexpr std::size_t kResponseFrameHeaderBytes = 7;

std::uint16_t readU16BE(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

std::uint32_t readU32BE(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

TransportOutcome mapResponseOutcome(std::uint8_t raw) {
    switch (raw) {
        case 0: return TransportOutcome::Accepted;
        case 1: return TransportOutcome::Rejected;
        case 2: return TransportOutcome::Timeout;
        case 3: return TransportOutcome::Unknown;
        case 4: return TransportOutcome::Unavailable;
        case 5: return TransportOutcome::VersionMismatch;
        case 6: return TransportOutcome::PayloadTooLarge;
        case 7: return TransportOutcome::ProtocolError;
        case 8: return TransportOutcome::Stopping;
        default: return TransportOutcome::Unknown;  // 未知不得视为成功
    }
}

const char* outcomeName(TransportOutcome outcome) {
    switch (outcome) {
        case TransportOutcome::Accepted: return "Accepted";
        case TransportOutcome::Rejected: return "Rejected";
        case TransportOutcome::Timeout: return "Timeout";
        case TransportOutcome::Unknown: return "Unknown";
        case TransportOutcome::Unavailable: return "Unavailable";
        case TransportOutcome::VersionMismatch: return "VersionMismatch";
        case TransportOutcome::PayloadTooLarge: return "PayloadTooLarge";
        case TransportOutcome::ProtocolError: return "ProtocolError";
        case TransportOutcome::Stopping: return "Stopping";
    }
    return "Unknown";
}

struct ResponseFrameHeader {
    bool ok = false;
    TransportOutcome outcome = TransportOutcome::ProtocolError;
    std::uint16_t errorCode = 0;
    std::uint32_t envelopeLength = 0;
    const char* reason = "frame_too_short";
};

ResponseFrameHeader parseResponseFrameHeader(const std::vector<std::uint8_t>& wire,
                                             std::size_t maxEnvelopeBytes) {
    ResponseFrameHeader h;
    if (wire.size() < kResponseFrameHeaderBytes) {
        h.reason = "frame_too_short";
        h.outcome = TransportOutcome::ProtocolError;
        return h;
    }
    h.errorCode = readU16BE(wire.data() + 1);
    h.envelopeLength = readU32BE(wire.data() + 3);
    h.outcome = mapResponseOutcome(wire[0]);
    if (maxEnvelopeBytes > 0 && h.envelopeLength > maxEnvelopeBytes) {
        h.outcome = TransportOutcome::PayloadTooLarge;
        h.reason = "oversize";
        return h;
    }
    if (wire.size() - kResponseFrameHeaderBytes < h.envelopeLength) {
        h.outcome = TransportOutcome::ProtocolError;
        h.reason = "truncated";
        return h;
    }
    h.ok = true;
    h.reason = "";
    return h;
}

void logResponseFrameFailure(const ResponseFrameHeader& h, const std::string& payloadType) {
    FotaLogAdapter::orchestrator().warn(
        fota_events::CLOUD_TRANSPORT_FRAME,
        "TBOX response frame rejected",
        {flog::f_str("reason", h.reason),
         flog::f_str("outcome", outcomeName(h.outcome)),
         flog::f_int("tbox_error_code", static_cast<std::int64_t>(h.errorCode)),
         flog::f_str("payload_type", payloadType)});
}

} // namespace

// ---------------------------------------------------------------------------
// 传输结果映射
// ---------------------------------------------------------------------------
TransportOutcome mapCallResultToOutcome(const ccu::fw::someip::CallResult& result) {
    if (result.success) return TransportOutcome::Accepted;
    // CCU-FW-03xx 链（CR-005 §12）。只描述本地受理/可用性/版本/超时/容量/协议结果。
    const std::string& code = result.code;
    if (code == "CCU-FW-0305") return TransportOutcome::Timeout;
    if (code == "CCU-FW-0306") return TransportOutcome::ProtocolError;
    if (code == "CCU-FW-0307" || code == "CCU-FW-0309") return TransportOutcome::Rejected;
    if (code == "CCU-FW-0308") return TransportOutcome::Rejected;
    if (code == "CCU-FW-0310") return TransportOutcome::Stopping;
    if (code == "CCU-FW-0301" || code == "CCU-FW-0302" || code == "CCU-FW-0303") {
        return TransportOutcome::Unavailable;
    }
    if (code == "CCU-FW-0304") {
        // 不可用/版本不匹配；调用方按 client availability 区分 VersionMismatch。
        return TransportOutcome::Unavailable;
    }
    // 未知/不确定 -> Unknown（不得推断为成功）。
    return TransportOutcome::Unknown;
}

TransportOutcome validateOutboundEnvelope(const VehicleMessageEnvelope& env,
                                          std::size_t payloadSize,
                                          std::size_t maxPayloadBytes,
                                          std::size_t /*maxEnvelopeBytes*/) {
    if (env.message_kind() != MESSAGE_KIND_REQUEST &&
        env.message_kind() != MESSAGE_KIND_EVENT) {
        return TransportOutcome::ProtocolError;
    }
    if (env.service() != payload_type::kService) return TransportOutcome::Rejected;
    if (env.protocol_version() != payload_type::kProtocolVersion) {
        return TransportOutcome::VersionMismatch;
    }
    if (env.message_id().empty()) return TransportOutcome::Rejected;
    if (expired(env)) return TransportOutcome::Rejected;  // TTL 已过期
    if (payloadSize > maxPayloadBytes) return TransportOutcome::PayloadTooLarge;
    return TransportOutcome::Accepted;
}

TransportOutcome validateInboundEnvelope(const VehicleMessageEnvelope& env,
                                         std::size_t payloadSize,
                                         std::size_t maxPayloadBytes,
                                         std::size_t /*maxEnvelopeBytes*/) {
    if (env.service() != payload_type::kService) return TransportOutcome::Rejected;
    if (env.protocol_version() != payload_type::kProtocolVersion) {
        return TransportOutcome::VersionMismatch;
    }
    if (expired(env)) return TransportOutcome::Rejected;
    if (payloadSize > maxPayloadBytes) return TransportOutcome::PayloadTooLarge;
    return TransportOutcome::Accepted;
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
SomeIpVehicleMessageTransport::SomeIpVehicleMessageTransport(
    ccu::fw::someip::Client client,
    const ccu_fota::someip::TboxGenericTransportAddress& address,
    SomeIpTransportConfig cfg)
    : client_(std::move(client))
    , address_(address)
    , cfg_(std::move(cfg)) {
    // fail-closed：未完全分配/非法地址禁止构造传输（不得猜测 ID）。
    if (!address_.fullyAllocated()) {
        throw std::logic_error(
            "SomeIpVehicleMessageTransport: Registry 未分配 generic Method/Event/"
            "Eventgroup ID，禁止构造（fail-closed）");
    }
}

SomeIpVehicleMessageTransport::~SomeIpVehicleMessageTransport() { stop(); }

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void SomeIpVehicleMessageTransport::start() {
    if (started_.exchange(true)) return;
    stopping_.store(false);

    client_.requestService();

    // 有界等待可用（Available / VersionMismatch / Stopping 均为稳定状态）。
    {
        const int waitMs = static_cast<int>(cfg_.availabilityWait.count());
        for (int i = 0; i < waitMs; ++i) {
            auto a = client_.availability();
            if (a == ccu::fw::someip::Availability::Available ||
                a == ccu::fw::someip::Availability::VersionMismatch ||
                a == ccu::fw::someip::Availability::Stopping) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // 启动独立下行 executor（有界队列 + worker 线程）。
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueStopping_ = false;
    }
    for (std::size_t i = 0; i < cfg_.downlinkWorkers; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }

    // 订阅 TBOX 通用下行 Event。framework executor 上回调只做校验+入队，
    // 绝不在 SOME/IP I/O 线程推进业务状态机。
    downlinkSub_ = client_.subscribe(
        address_.eventgroup, address_.event,
        [this](const ccu::fw::someip::EventContext& ec, ccu::fw::someip::PayloadView v) {
            onDownlinkEvent(ec, v);
        });
}

void SomeIpVehicleMessageTransport::stop() {
    // 幂等。
    const bool wasStarted = started_.exchange(false);
    stopping_.store(true);   // 1. 拒绝新调用 -> Stopping

    // 2. 停止下行 worker（drain 剩余队列后再退出）。
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueStopping_ = true;
    }
    queueCv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    // 3. 取消下行订阅。
    if (downlinkSub_.active()) downlinkSub_.cancel();

    // 4. 有界等待 in-flight 收敛（exchange/publish 由调用线程持有未来，超时自返）。
    {
        const int waitMs = static_cast<int>(cfg_.exchangeTimeout.count()) + 1000;
        for (int i = 0; i < waitMs && inflightCount() > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // 5. release service（SomeIpRuntime.stop 由 daemon/main 统一执行）。
    if (wasStarted) client_.releaseService();

    // 清理 correlation 表。
    {
        std::lock_guard<std::mutex> lock(inflightMutex_);
        inflight_.clear();
    }
}

ccu::fw::someip::Availability SomeIpVehicleMessageTransport::availability() const {
    return client_.availability();
}

// ---------------------------------------------------------------------------
// 有界 in-flight correlation 表
// ---------------------------------------------------------------------------
bool SomeIpVehicleMessageTransport::reserveInflight(const std::string& messageId,
                                                    std::int64_t deadlineMs) {
    std::lock_guard<std::mutex> lock(inflightMutex_);
    evictExpiredInflight();
    if (inflight_.count(messageId) != 0) return false;  // correlation 冲突
    if (inflight_.size() >= cfg_.maxInFlight) return false;  // 有界
    inflight_.emplace(messageId, InflightEntry{deadlineMs, ++inflightSeq_});
    return true;
}

void SomeIpVehicleMessageTransport::releaseInflight(const std::string& messageId) {
    std::lock_guard<std::mutex> lock(inflightMutex_);
    inflight_.erase(messageId);
}

void SomeIpVehicleMessageTransport::evictExpiredInflight() {
    const std::int64_t now = nowMs();
    for (auto it = inflight_.begin(); it != inflight_.end();) {
        if (it->second.deadlineMs <= now) it = inflight_.erase(it);
        else ++it;
    }
}

std::size_t SomeIpVehicleMessageTransport::inflightCount() const {
    std::lock_guard<std::mutex> lock(inflightMutex_);
    return inflight_.size();
}

// ---------------------------------------------------------------------------
// exchange
// ---------------------------------------------------------------------------
TransportResult<VehicleMessage> SomeIpVehicleMessageTransport::exchange(
    const VehicleMessage& msg, const ExchangeOptions& opts, const CallContext& ctx) {
    (void)ctx;
    if (stopping_.load()) return {TransportOutcome::Stopping, {}};
    if (msg.envelope.message_kind() != MESSAGE_KIND_REQUEST) {
        return {TransportOutcome::ProtocolError, {}};  // 方向错误（与 Fake 一致）
    }
    const std::string messageId = msg.envelope.message_id();
    if (messageId.empty()) return {TransportOutcome::Rejected, {}};

    auto vo = validateOutboundEnvelope(msg.envelope, msg.payload.size(),
                                       cfg_.maxPayloadBytes, cfg_.maxEnvelopeBytes);
    if (vo != TransportOutcome::Accepted) return {vo, {}};

    auto timeout = std::min(opts.timeout, cfg_.exchangeTimeout);
    if (timeout.count() <= 0) timeout = cfg_.exchangeTimeout;
    const std::int64_t deadline = nowMs() + timeout.count();

    if (!reserveInflight(messageId, deadline)) {
        return {TransportOutcome::Rejected, {}};  // 有界 in-flight 耗尽/冲突
    }

    auto wire = encodeWire(msg);
    if (wire.size() > cfg_.maxEnvelopeBytes) {
        releaseInflight(messageId);
        return {TransportOutcome::PayloadTooLarge, {}};
    }

    ccu::fw::someip::CallOptions co;
    co.timeout = timeout;
    co.retry = ccu::fw::someip::RetryMode::None;  // framework 自动重试保持关闭
    co.maxRetries = 0;

    auto fut = client_.callAsync(address_.method, std::move(wire), co);
    if (fut.wait_for(timeout) != std::future_status::ready) {
        releaseInflight(messageId);
        return {TransportOutcome::Timeout, {}};
    }
    auto result = fut.get();
    releaseInflight(messageId);

    if (!result.success) {
        auto mapped = mapCallResultToOutcome(result);
        if (mapped == TransportOutcome::Unavailable &&
            client_.availability() == ccu::fw::someip::Availability::VersionMismatch) {
            return {TransportOutcome::VersionMismatch, {}};
        }
        return {mapped, {}};
    }

    // 响应方向是 TBOX VehicleMessageResponseFrame，不是裸 Envelope。
    const std::size_t responseLimit =
        opts.max_response_bytes > 0
            ? std::min(opts.max_response_bytes, cfg_.maxEnvelopeBytes)
            : cfg_.maxEnvelopeBytes;
    ResponseFrameHeader frame =
        parseResponseFrameHeader(result.response, responseLimit);
    if (!frame.ok) {
        logResponseFrameFailure(frame, msg.envelope.payload_type());
        return {frame.outcome, {}};
    }
    if (frame.outcome != TransportOutcome::Accepted) {
        frame.reason = "outcome_not_accepted";
        logResponseFrameFailure(frame, msg.envelope.payload_type());
        return {frame.outcome, {}};
    }
    if (frame.envelopeLength == 0) {
        frame.reason = "empty_envelope";
        logResponseFrameFailure(frame, msg.envelope.payload_type());
        return {TransportOutcome::ProtocolError, {}};
    }

    std::vector<std::uint8_t> envelopeBytes(
        result.response.begin() + kResponseFrameHeaderBytes,
        result.response.begin() + kResponseFrameHeaderBytes + frame.envelopeLength);
    VehicleMessage resp;
    if (!decodeWire(envelopeBytes, resp)) {
        frame.reason = "envelope_decode_failed";
        logResponseFrameFailure(frame, msg.envelope.payload_type());
        return {TransportOutcome::ProtocolError, {}};
    }
    // 校验 response correlation_id == 请求 message_id；迟到/错配不完成调用。
    if (!resp.envelope.has_correlation_id() ||
        resp.envelope.correlation_id() != messageId) {
        return {TransportOutcome::ProtocolError, {}};
    }
    if (resp.envelope.message_kind() != MESSAGE_KIND_RESPONSE) {
        return {TransportOutcome::ProtocolError, {}};
    }
    auto vi = validateInboundEnvelope(resp.envelope, resp.payload.size(),
                                      cfg_.maxPayloadBytes, cfg_.maxEnvelopeBytes);
    if (vi != TransportOutcome::Accepted) return {vi, {}};
    return {TransportOutcome::Accepted, std::move(resp)};
}

// ---------------------------------------------------------------------------
// publish（单向事件投递；无需同步业务响应）
// ---------------------------------------------------------------------------
TransportResult<void> SomeIpVehicleMessageTransport::publish(const VehicleMessage& msg,
                                                             const CallContext& ctx) {
    if (stopping_.load()) return {TransportOutcome::Stopping};
    if (msg.envelope.message_kind() != MESSAGE_KIND_EVENT) {
        return {TransportOutcome::Rejected};
    }
    auto vo = validateOutboundEnvelope(msg.envelope, msg.payload.size(),
                                       cfg_.maxPayloadBytes, cfg_.maxEnvelopeBytes);
    if (vo != TransportOutcome::Accepted) return {vo};

    auto wire = encodeWire(msg);
    if (wire.size() > cfg_.maxEnvelopeBytes) return {TransportOutcome::PayloadTooLarge};

    auto timeout = std::min(ctx.timeout, cfg_.exchangeTimeout);
    if (timeout.count() <= 0) timeout = cfg_.exchangeTimeout;

    ccu::fw::someip::CallOptions co;
    co.timeout = timeout;
    co.retry = ccu::fw::someip::RetryMode::None;
    co.maxRetries = 0;

    auto fut = client_.callAsync(address_.method, std::move(wire), co);
    if (fut.wait_for(timeout) != std::future_status::ready) {
        return {TransportOutcome::Timeout};
    }
    auto result = fut.get();
    if (!result.success) {
        auto mapped = mapCallResultToOutcome(result);
        if (mapped == TransportOutcome::Unavailable &&
            client_.availability() == ccu::fw::someip::Availability::VersionMismatch) {
            return {TransportOutcome::VersionMismatch};
        }
        return {mapped};
    }
    // SOME/IP 成功 / TBOX accepted / MQTT PUBACK 均只映射为 Accepted（受理）。
    return {TransportOutcome::Accepted};
}

// ---------------------------------------------------------------------------
// 下行订阅
// ---------------------------------------------------------------------------
Subscription SomeIpVehicleMessageTransport::subscribe(std::string_view service,
                                                      DownlinkHandler handler) {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    const std::size_t idx = handlers_.size();
    handlers_.emplace_back(SubEntry{std::string(service), std::move(handler)});
    auto* self = this;
    return Subscription([self, idx]() {
        std::lock_guard<std::mutex> guard(self->handlersMutex_);
        if (idx < self->handlers_.size()) {
            self->handlers_[idx].handler = nullptr;
        }
    });
}

void SomeIpVehicleMessageTransport::dispatchToHandlers(VehicleMessage&& msg) {
    std::vector<DownlinkHandler> targets;
    {
        std::lock_guard<std::mutex> lock(handlersMutex_);
        for (auto& e : handlers_) {
            if (e.handler && e.service == msg.envelope.service()) {
                targets.push_back(e.handler);
            }
        }
    }
    for (auto& h : targets) {
        auto copy = msg;
        h(std::move(copy));
    }
}

void SomeIpVehicleMessageTransport::workerLoop() {
    while (true) {
        VehicleMessage msg;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] {
                return !downlinkQueue_.empty() || queueStopping_;
            });
            if (downlinkQueue_.empty()) {
                if (queueStopping_) return;
                continue;
            }
            msg = std::move(downlinkQueue_.front());
            downlinkQueue_.pop_front();
        }
        dispatchToHandlers(std::move(msg));
    }
}

void SomeIpVehicleMessageTransport::onDownlinkEvent(
    const ccu::fw::someip::EventContext&, ccu::fw::someip::PayloadView view) {
    // framework executor 上：只做基础校验 + 投递到有界队列，不推进业务状态机。
    if (stopping_.load()) return;
    if (view.data == nullptr || view.size == 0) return;

    VehicleMessage msg;
    if (!decodeWire(std::vector<std::uint8_t>(view.data, view.data + view.size), msg)) {
        return;
    }
    if (msg.envelope.message_kind() != MESSAGE_KIND_EVENT) return;
    if (validateInboundEnvelope(msg.envelope, msg.payload.size(),
                                cfg_.maxPayloadBytes, cfg_.maxEnvelopeBytes) !=
        TransportOutcome::Accepted) {
        return;
    }

    // 有界队列：满则丢弃（背压），计数供诊断。
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queueStopping_) return;
        if (downlinkQueue_.size() >= cfg_.downlinkQueueCapacity) {
            ++downlinkDropped_;
            return;
        }
        downlinkQueue_.push_back(std::move(msg));
    }
    queueCv_.notify_one();
}

std::size_t SomeIpVehicleMessageTransport::downlinkQueueDepth() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return downlinkQueue_.size();
}

std::uint64_t SomeIpVehicleMessageTransport::downlinkDropped() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return downlinkDropped_;
}

} // namespace ota
} // namespace ccu_fota
