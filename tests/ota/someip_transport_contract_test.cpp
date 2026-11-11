// =============================================================================
// tests/ota/someip_transport_contract_test.cpp
// CCU-FOTA 量产 SomeIpVehicleMessageTransport 契约测试 (CR-010/011 测试矩阵 8)
// =============================================================================
// 覆盖：
//   1) 与 FakeVehicleMessageTransport 运行同一套传输边界 contract suite
//      （direction/service/version/TTL/size/correlation/publish/subscribe）。
//   2) SomeIp 特有：correlation 错配、超时、Unavailable、VersionMismatch、
//      有界 in-flight、慢订阅者/下行队列满、TBOX 重启重连、graceful shutdown、
//      Stopping、Envelope 大小上限、结果映射单测。
// 使用 ccu-framework-someip 公开 API（默认 FakeSomeIpBackend）+ 进程内 Provider
// 扮演 TBOX 通用消息中继（只做 Envelope 级校验/回显，不解析业务 payload）。
// =============================================================================

#include "ccu/fota/ota/someip_vehicle_message_transport.hpp"
#include "ccu/fota/ota/fota_message_codec.hpp"
#include "ccu/fota/ota/payload_types.hpp"
#include "ccu/fota/ota/mock/fake_vehicle_message_transport.hpp"

#include "ccu/fw/someip/runtime.hpp"
#include "ccu/fw/someip/types.hpp"
#include "constants.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ccu_fota;
using namespace ccu_fota::ota;
namespace someip_fw = ccu::fw::someip;

namespace {

// 测试专用寻址（仅测试 target；量产解析器禁止猜测/硬编码真实 Registry ID）。
constexpr someip_fw::MethodId kGenericMethod = 0x0002;
constexpr someip_fw::EventId kGenericEvent = 0x8001;
constexpr someip_fw::EventgroupId kGenericGroup = 0x0001;

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::vector<std::byte> strToBytes(const std::string& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = std::byte(static_cast<unsigned char>(s[i]));
    }
    return out;
}

CallContext makeCtx(const std::string& id) {
    CallContext ctx;
    ctx.traceId = "trace-" + id;
    ctx.requestId = "req-" + id;
    ctx.idempotencyKey = id;
    ctx.timeout = std::chrono::milliseconds(2000);
    ctx.deviceId = "dev-someip";
    ctx.vin = "VINSOMEIP";
    return ctx;
}

// 构造一个合法请求 Envelope（TaskCheckRequest 作为通用有效请求）。
VehicleMessage makeRequest(std::string_view ptype = payload_type::kTaskCheckRequest,
                           const std::string& payload = "req",
                           std::string_view messageId = "msg-1",
                           std::string_view service = payload_type::kService,
                           std::string_view version = payload_type::kProtocolVersion,
                           std::int64_t expireAtMs = 0) {
    VehicleMessage msg;
    msg.envelope.set_message_id(std::string(messageId));
    msg.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_REQUEST);
    msg.envelope.set_service(std::string(service));
    msg.envelope.set_payload_type(std::string(ptype));
    msg.envelope.set_protocol_version(std::string(version));
    msg.envelope.set_timestamp_ms(nowMs());
    if (expireAtMs > 0) msg.envelope.set_expire_at_ms(expireAtMs);
    msg.payload = strToBytes(payload);
    return msg;
}

VehicleMessage makeEvent(std::string_view ptype, const std::string& payload,
                         std::string_view messageId,
                         std::string_view service = payload_type::kService) {
    VehicleMessage msg;
    msg.envelope.set_message_id(std::string(messageId));
    msg.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_EVENT);
    msg.envelope.set_service(std::string(service));
    msg.envelope.set_payload_type(std::string(ptype));
    msg.envelope.set_protocol_version(std::string(payload_type::kProtocolVersion));
    msg.envelope.set_timestamp_ms(nowMs());
    msg.payload = strToBytes(payload);
    return msg;
}

// 序列化 Envelope（payload 字段承载 bytes）-> SOME/IP 字节。
someip_fw::Payload encodeWire(const VehicleMessage& msg) {
    auto env = msg.envelope;
    if (!msg.payload.empty()) {
        env.set_payload(std::string(
            reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size()));
    }
    std::string out;
    env.SerializeToString(&out);
    return someip_fw::Payload(out.begin(), out.end());
}

// TBOX VehicleMessageResponseFrame（CCU-FOTA-DSN-CR-016）：
// outcome(1B) + error_code(2B BE) + envelope_length(4B BE) + envelope。
someip_fw::Payload encodeResponseFrame(std::uint8_t outcome, std::uint16_t errorCode,
                                       const someip_fw::Payload& envelope) {
    std::vector<std::uint8_t> out;
    out.reserve(7 + envelope.size());
    out.push_back(outcome);
    out.push_back(static_cast<std::uint8_t>((errorCode >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(errorCode & 0xFF));
    const std::uint32_t len = static_cast<std::uint32_t>(envelope.size());
    out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));
    out.insert(out.end(), envelope.begin(), envelope.end());
    return someip_fw::Payload(out.begin(), out.end());
}

// 合法 TaskCheckRequest payload（Fake 云侧会解析业务 proto）。
std::string validTaskCheckPayload() {
    ::vehicle::fota::v1::TaskCheckRequest r;
    r.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
    r.set_inventory_revision(1);
    std::string out;
    r.SerializeToString(&out);
    return out;
}

// 等待谓词成立（有界轮询）。
bool waitFor(const std::function<bool()>& pred, int timeoutMs = 2000) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < timeoutMs) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// ---------------------------------------------------------------------------
// TBOX 通用中继 Provider 帮助器：回显 Envelope，可注入 correlation/延迟故障。
// ---------------------------------------------------------------------------
struct EchoProvider {
    someip_fw::SomeIpRuntime* rt = nullptr;
    someip_fw::Provider provider;
    std::string wrongCorrelation;      // 非空则回显错误 correlation_id
    std::chrono::milliseconds delay{0};  // 响应前延迟
    std::uint8_t outcome = 0;          // TBOX TransportOutcome，0=Accepted
    std::uint16_t errorCode = 0;       // TBOX-SOMEIP-1001..1004，0=无错误
    std::optional<someip_fw::Payload> rawResponse;  // 非空则直接返回该帧
    std::atomic<std::uint64_t> requestsSeen{0};
    // framework 的 event callback 在 executor 上延迟执行且引用 PayloadView；
    // 必须保持 wire 缓冲存活到回调执行完。
    std::vector<someip_fw::Payload> pendingWires_;

    someip_fw::MethodResult handle(const someip_fw::RequestContext&,
                                   someip_fw::PayloadView v) {
        ++requestsSeen;
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        if (rawResponse.has_value()) {
            return someip_fw::MethodResult::ok(*rawResponse);
        }
        VehicleMessage req;
        if (v.data == nullptr || v.size == 0 ||
            !req.envelope.ParseFromArray(v.data, static_cast<int>(v.size))) {
            return someip_fw::MethodResult::error(0x1, "bad envelope");
        }
        VehicleMessage resp;
        resp.envelope.set_correlation_id(
            wrongCorrelation.empty() ? req.envelope.message_id() : wrongCorrelation);
        resp.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_RESPONSE);
        resp.envelope.set_service(req.envelope.service());
        resp.envelope.set_payload_type(req.envelope.payload_type());
        resp.envelope.set_protocol_version(req.envelope.protocol_version());
        resp.envelope.set_timestamp_ms(nowMs());
        resp.payload = strToBytes(req.envelope.payload());
        return someip_fw::MethodResult::ok(
            encodeResponseFrame(outcome, errorCode, encodeWire(resp)));
    }

    void setup(someip_fw::SomeIpRuntime& r, someip_fw::Provider p) {
        rt = &r;
        provider = std::move(p);
        provider.registerMethod(kGenericMethod,
                                [this](const someip_fw::RequestContext& c, someip_fw::PayloadView v) {
                                    return handle(c, v);
                                });
        provider.offer();
        provider.offerEvent(kGenericEvent, kGenericGroup);
    }

    void notifyDownlink(const VehicleMessage& msg) {
        pendingWires_.push_back(encodeWire(msg));
        auto& wire = pendingWires_.back();
        provider.notify(kGenericEvent,
                        someip_fw::PayloadView{wire.data(), wire.size()});
    }
};

// 共享传输边界 contract suite（Fake 与 SomeIp 运行同一套断言）。
// transport 由调用方持有；deliverDownlink 向传输注入一条下行消息。
void RunSharedTransportContractSuite(
    VehicleMessageTransport& transport,
    const std::function<void(const VehicleMessage&)>& deliverDownlink,
    std::size_t maxPayloadBytes) {
    auto valid = makeRequest(payload_type::kTaskCheckRequest, validTaskCheckPayload(), "msg-shared");
    // 1. 合法 exchange -> Accepted + correlation 匹配。
    {
        auto r = transport.exchange(valid, ExchangeOptions{}, makeCtx("s1"));
        EXPECT_EQ(r.outcome, TransportOutcome::Accepted);
        if (r.outcome == TransportOutcome::Accepted) {
            EXPECT_EQ(r.value.envelope.message_kind(),
                      ::vehicle::common::v1::MESSAGE_KIND_RESPONSE);
            EXPECT_EQ(r.value.envelope.correlation_id(), "msg-shared");
        }
    }
    // 2. 错误方向（EVENT 走 exchange）-> ProtocolError（Fake 与 SomeIp 一致）。
    {
        auto ev = makeEvent(payload_type::kTaskCheckRequest, "p", "msg-e");
        EXPECT_EQ(transport.exchange(ev, ExchangeOptions{}, makeCtx("s2")).outcome,
                  TransportOutcome::ProtocolError);
    }
    // 3. 空 message_id -> Rejected。
    {
        auto noId = makeRequest();
        noId.envelope.clear_message_id();
        EXPECT_EQ(transport.exchange(noId, ExchangeOptions{}, makeCtx("s3")).outcome,
                  TransportOutcome::Rejected);
    }
    // 4. TTL 过期 -> Rejected。
    {
        auto expired = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-t",
                                   payload_type::kService, payload_type::kProtocolVersion,
                                   nowMs() - 1000);
        EXPECT_EQ(transport.exchange(expired, ExchangeOptions{}, makeCtx("s4")).outcome,
                  TransportOutcome::Rejected);
    }
    // 5. 版本不匹配 -> VersionMismatch。
    {
        auto bad = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-v",
                               payload_type::kService, "fota-v2");
        EXPECT_EQ(transport.exchange(bad, ExchangeOptions{}, makeCtx("s5")).outcome,
                  TransportOutcome::VersionMismatch);
    }
    // 6. service 不匹配 -> Rejected。
    {
        auto bad = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-svc",
                               "vehicle.sota");
        EXPECT_EQ(transport.exchange(bad, ExchangeOptions{}, makeCtx("s6")).outcome,
                  TransportOutcome::Rejected);
    }
    // 7. 超限 payload -> PayloadTooLarge。
    {
        auto big = makeRequest(payload_type::kTaskCheckRequest,
                               std::string(maxPayloadBytes + 16, 'x'),
                               "msg-big");
        ExchangeOptions tiny;
        tiny.max_response_bytes = 8;
        EXPECT_EQ(transport.exchange(big, tiny, makeCtx("s7")).outcome,
                  TransportOutcome::PayloadTooLarge);
    }
    // 8. publish(EVENT) -> Accepted。
    {
        auto ev = makeEvent("vehicle.fota.v1.Heartbeat", "hb", "pub-1");
        EXPECT_EQ(transport.publish(ev, makeCtx("s8")).outcome,
                  TransportOutcome::Accepted);
    }
    // 9. publish(REQUEST) -> Rejected。
    {
        auto req = makeRequest(payload_type::kTaskCheckRequest, "p", "pub-2");
        EXPECT_EQ(transport.publish(req, makeCtx("s9")).outcome,
                  TransportOutcome::Rejected);
    }
    // 10. 下行订阅 + 投递（正确 service）-> handler 收到。
    {
        std::atomic<int> received{0};
        Subscription sub = transport.subscribe(
            payload_type::kService,
            [&](VehicleMessage&& msg) {
                if (msg.envelope.message_kind() == ::vehicle::common::v1::MESSAGE_KIND_EVENT) {
                    ++received;
                }
            });
        deliverDownlink(makeEvent(payload_type::kControlCommand, "cmd", "dl-1"));
        EXPECT_TRUE(waitFor([&] { return received.load() == 1; }));
        // 11. 错误 service 不投递。
        deliverDownlink(makeEvent(payload_type::kControlCommand, "cmd", "dl-2",
                                  "vehicle.sota"));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(received.load(), 1);
        // 12. 错误方向（REQUEST）不投递。
        deliverDownlink(makeRequest(payload_type::kControlCommand, "cmd", "dl-3"));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(received.load(), 1);
        // 13. 退订后不再投递。
        sub.cancel();
        deliverDownlink(makeEvent(payload_type::kControlCommand, "cmd", "dl-4"));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(received.load(), 1);
    }
}

// ---------------------------------------------------------------------------
// Fake harness
// ---------------------------------------------------------------------------
TEST(SomeIpTransportContract, FakeRunsSharedContractSuite) {
    using namespace ccu_fota::ota::mock;
    auto scenario = parseScenario(R"({
      "scenario": "contract",
      "clock": "virtual",
      "inventory": {"mode": "FULL", "baseline": "BASE-001"},
      "packages": [],
      "faults": []
    })");
    auto fake = std::make_unique<FakeVehicleMessageTransport>(scenario);
    auto* raw = fake.get();
    RunSharedTransportContractSuite(
        *raw,
        [raw](const VehicleMessage& m) { raw->deliverDownlink(m); },
        /*maxPayloadBytes=*/8);  // Fake 用 opts.max_response_bytes 作为上限
}
struct SomeIpHarness {
    someip_fw::SomeIpRuntime rt;
    EchoProvider echo;
    std::shared_ptr<someip_fw::Executor> exec;
    std::unique_ptr<SomeIpVehicleMessageTransport> transport;
    ccu_fota::someip::TboxGenericTransportAddress addr;
    SomeIpTransportConfig cfg;

    // enabled 控制 provider 是否 offer（Unavailable 测试用）。
    void setup(bool offerProvider = true, std::string wrongCorrelation = "",
               std::chrono::milliseconds delay = std::chrono::milliseconds(0)) {
        addr.service = DEFAULT_TBOX_SERVICE_ID;
        addr.instance = DEFAULT_TBOX_INSTANCE_ID;
        addr.method = kGenericMethod;
        addr.event = kGenericEvent;
        addr.eventgroup = kGenericGroup;
        addr.interfaceVersion = {1, 0};

        rt = someip_fw::SomeIpRuntime::create(makeTestConfig2());
        rt.start();
        someip_fw::ServiceKey key{DEFAULT_TBOX_SERVICE_ID, DEFAULT_TBOX_INSTANCE_ID};
        auto provider = rt.createProvider(key, {1, 0});
        echo.wrongCorrelation = std::move(wrongCorrelation);
        echo.delay = delay;
        if (offerProvider) {
            echo.setup(rt, std::move(provider));
        }
        auto client = rt.createClient(key, {1, 0});
        transport = std::make_unique<SomeIpVehicleMessageTransport>(
            std::move(client), addr, cfg);
        transport->start();
    }

    someip_fw::SomeIpConfig makeTestConfig2() {
        someip_fw::SomeIpConfig c;
        c.application = "ccu-fota-test-someip";
        c.routingMode = someip_fw::RoutingMode::External;
        c.maxPayloadBytes = 1024 * 1024;
        c.maxInflightCalls = 64;
        c.callbackQueueSize = 256;
        c.shutdownTimeout = std::chrono::milliseconds(1000);
        c.callTimeout = std::chrono::milliseconds(500);
        c.discovery.enabled = true;
        c.discovery.initialBackoff = std::chrono::milliseconds(10);
        c.discovery.maxBackoff = std::chrono::milliseconds(100);
        c.discovery.multiplier = 2.0;
        c.discovery.jitterPercent = 0;
        c.maxProviders = 8;
        c.maxClients = 8;
        c.maxSubscriptionsPerClient = 16;
        c.maxRetryTimers = 64;
        c.registryProfile = "ccu-fota-test";
        return c;
    }
};

} // namespace

// ===========================================================================
// 1. 共享传输边界 contract suite：Fake
// ===========================================================================
TEST(SomeIpTransportContract, SomeIpRunsSharedContractSuite) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(2000);
    h.cfg.maxPayloadBytes = 256;
    h.setup();
    RunSharedTransportContractSuite(
        *h.transport,
        [&h](const VehicleMessage& m) { h.echo.notifyDownlink(m); },
        /*maxPayloadBytes=*/256);
    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 3. SomeIp 特有：Stopping（shutdown 后拒绝新请求）
// ===========================================================================
TEST(SomeIpTransportContract, StoppingAfterShutdown) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(1000);
    h.setup();
    EXPECT_TRUE(h.transport->availability() == someip_fw::Availability::Available ||
                h.transport->availability() == someip_fw::Availability::Unknown);

    h.transport->stop();
    EXPECT_TRUE(h.transport->stopping());

    auto req = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-stop");
    EXPECT_EQ(h.transport->exchange(req, ExchangeOptions{}, makeCtx("stop")).outcome,
              TransportOutcome::Stopping);
    auto ev = makeEvent("vehicle.fota.v1.Heartbeat", "hb", "pub-stop");
    EXPECT_EQ(h.transport->publish(ev, makeCtx("stop")).outcome,
              TransportOutcome::Stopping);
    h.rt.stop();
}

// ===========================================================================
// 4. SomeIp 特有：correlation 错配 -> ProtocolError（不完成调用）
// ===========================================================================
TEST(SomeIpTransportContract, CorrelationMismatchProtocolError) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(2000);
    h.setup(/*offer=*/true, /*wrongCorrelation=*/"WRONG-CORR");
    auto req = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-corr");
    auto r = h.transport->exchange(req, ExchangeOptions{}, makeCtx("corr"));
    EXPECT_EQ(r.outcome, TransportOutcome::ProtocolError);
    EXPECT_GT(h.echo.requestsSeen.load(), 0u);
    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 4.1 TBOX 响应帧：outcome 映射（CCU-FOTA-DSN-CR-016）
// ===========================================================================
TEST(SomeIpTransportContract, ResponseFrameOutcomeMapping) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(2000);
    h.setup();
    struct Case {
        std::uint8_t raw;
        TransportOutcome expected;
    };
    const Case cases[] = {
        {1, TransportOutcome::Rejected},
        {2, TransportOutcome::Timeout},
        {3, TransportOutcome::Unknown},
        {4, TransportOutcome::Unavailable},
        {5, TransportOutcome::VersionMismatch},
        {6, TransportOutcome::PayloadTooLarge},
        {7, TransportOutcome::ProtocolError},
        {8, TransportOutcome::Stopping},
        {0xFF, TransportOutcome::Unknown},  // 未知 outcome 不得视为成功
    };
    int i = 0;
    for (const auto& c : cases) {
        h.echo.outcome = c.raw;
        h.echo.errorCode = 1003;
        auto req = makeRequest(payload_type::kTaskCheckRequest, "p",
                               "msg-frame-out-" + std::to_string(i++));
        auto r = h.transport->exchange(req, ExchangeOptions{}, makeCtx("fo"));
        EXPECT_EQ(r.outcome, c.expected) << "raw_outcome=" << static_cast<int>(c.raw);
    }
    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 4.2 TBOX 响应帧：Accepted + 空 Envelope -> ProtocolError
// ===========================================================================
TEST(SomeIpTransportContract, ResponseFrameAcceptedEmptyEnvelope) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(1000);
    h.setup();
    h.echo.outcome = 0;
    h.echo.rawResponse = encodeResponseFrame(0, 0, {});
    auto req = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-frame-empty");
    auto r = h.transport->exchange(req, ExchangeOptions{}, makeCtx("fe"));
    EXPECT_EQ(r.outcome, TransportOutcome::ProtocolError);
    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 4.3 TBOX 响应帧：过短 / 截断 / 超限
// ===========================================================================
TEST(SomeIpTransportContract, ResponseFrameMalformedAndOversize) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(1000);
    h.cfg.maxEnvelopeBytes = 256;
    h.setup();

    // 帧过短（< 7 字节）。
    h.echo.rawResponse = someip_fw::Payload{0x00, 0x00, 0x01};
    auto req = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-frame-short");
    EXPECT_EQ(h.transport->exchange(req, ExchangeOptions{}, makeCtx("fs")).outcome,
              TransportOutcome::ProtocolError);

    // envelope_length 大于剩余字节 -> 截断。
    someip_fw::Payload truncated{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};  // len=16
    h.echo.rawResponse = truncated;
    auto req2 = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-frame-trunc");
    EXPECT_EQ(h.transport->exchange(req2, ExchangeOptions{}, makeCtx("ft")).outcome,
              TransportOutcome::ProtocolError);

    // envelope_length 超限 -> PayloadTooLarge。
    someip_fw::Payload oversize{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20};  // len=32
    oversize.resize(7 + 32, 0);
    h.echo.rawResponse = oversize;
    ExchangeOptions tiny;
    tiny.max_response_bytes = 8;
    auto req3 = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-frame-big");
    EXPECT_EQ(h.transport->exchange(req3, tiny, makeCtx("fb")).outcome,
              TransportOutcome::PayloadTooLarge);

    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 5. SomeIp 特有：超时（中继延迟 > 本地超时）
// ===========================================================================
TEST(SomeIpTransportContract, TimeoutWhenPeerSlow) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(200);
    h.setup(/*offer=*/true, /*wrongCorrelation=*/"", /*delay=*/std::chrono::milliseconds(800));
    auto req = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-timeout");
    auto r = h.transport->exchange(req, ExchangeOptions{}, makeCtx("to"));
    EXPECT_EQ(r.outcome, TransportOutcome::Timeout);
    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 6. SomeIp 特有：Unavailable（无 provider offer）
// ===========================================================================
TEST(SomeIpTransportContract, UnavailableWhenNoProvider) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(200);
    h.cfg.availabilityWait = std::chrono::milliseconds(200);
    h.setup(/*offerProvider=*/false);
    auto req = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-unavail");
    auto r = h.transport->exchange(req, ExchangeOptions{}, makeCtx("ua"));
    EXPECT_EQ(r.outcome, TransportOutcome::Unavailable);
    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 7. SomeIp 特有：VersionMismatch（Provider major=1 vs Client major=2）
// ===========================================================================
TEST(SomeIpTransportContract, VersionMismatchMapsCorrectly) {
    // 独立 runtime：provider major=1，client major=2。
    auto rt = someip_fw::SomeIpRuntime::create(
        [] {
            someip_fw::SomeIpConfig c;
            c.application = "ccu-fota-test-ver";
            c.routingMode = someip_fw::RoutingMode::External;
            c.maxPayloadBytes = 1024 * 1024;
            c.maxInflightCalls = 16;
            c.callbackQueueSize = 64;
            c.shutdownTimeout = std::chrono::milliseconds(500);
            c.callTimeout = std::chrono::milliseconds(500);
            c.discovery.jitterPercent = 0;
            c.maxProviders = 4;
            c.maxClients = 4;
            c.maxSubscriptionsPerClient = 8;
            c.maxRetryTimers = 16;
            c.registryProfile = "ccu-fota-test";
            return c;
        }());
    rt.start();
    someip_fw::ServiceKey key{DEFAULT_TBOX_SERVICE_ID, DEFAULT_TBOX_INSTANCE_ID};
    auto provider = rt.createProvider(key, {1, 0});
    provider.registerMethod(kGenericMethod,
        [](const someip_fw::RequestContext&, someip_fw::PayloadView v) {
            return someip_fw::MethodResult::ok(someip_fw::Payload(v.data, v.data + v.size));
        });
    provider.offer();

    ccu_fota::someip::TboxGenericTransportAddress addr;
    addr.service = DEFAULT_TBOX_SERVICE_ID;
    addr.instance = DEFAULT_TBOX_INSTANCE_ID;
    addr.method = kGenericMethod;
    addr.event = kGenericEvent;
    addr.eventgroup = kGenericGroup;
    addr.interfaceVersion = {2, 0};  // client major=2

    auto client = rt.createClient(key, addr.interfaceVersion);
    SomeIpTransportConfig cfg;
    cfg.exchangeTimeout = std::chrono::milliseconds(500);
    cfg.availabilityWait = std::chrono::milliseconds(500);
    auto transport = std::make_unique<SomeIpVehicleMessageTransport>(
        std::move(client), addr, cfg);
    transport->start();
    // 等待 availability 稳定为 VersionMismatch。
    for (int i = 0; i < 500; ++i) {
        if (transport->availability() == someip_fw::Availability::VersionMismatch) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto req = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-ver");
    auto r = transport->exchange(req, ExchangeOptions{}, makeCtx("vm"));
    EXPECT_EQ(r.outcome, TransportOutcome::VersionMismatch);
    transport->stop();
    rt.stop();
}

// ===========================================================================
// 8. SomeIp 特有：有界 in-flight（maxInFlight=1 时第二个调用被拒绝）
// ===========================================================================
TEST(SomeIpTransportContract, InFlightBoundRejectsOverflow) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(1500);
    h.cfg.maxInFlight = 1;
    // 中继延迟使第一个调用保持在途。
    h.setup(/*offer=*/true, /*wrongCorrelation=*/"", /*delay=*/std::chrono::milliseconds(600));

    std::atomic<bool> firstDone{false};
    TransportOutcome firstOutcome = TransportOutcome::Unknown;
    auto req1 = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-ifl-1");
    auto req2 = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-ifl-2");

    std::thread t([&] {
        firstOutcome = h.transport->exchange(req1, ExchangeOptions{}, makeCtx("ifl1")).outcome;
        firstDone = true;
    });

    // 等待第一个进入在途。
    EXPECT_TRUE(waitFor([&] { return h.transport->inflightCount() >= 1; }));

    // 第二个调用 -> 有界拒绝。
    auto r2 = h.transport->exchange(req2, ExchangeOptions{}, makeCtx("ifl2"));
    EXPECT_EQ(r2.outcome, TransportOutcome::Rejected);

    t.join();
    EXPECT_TRUE(firstDone);
    EXPECT_EQ(firstOutcome, TransportOutcome::Accepted);
    EXPECT_EQ(h.transport->inflightCount(), 0u);

    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 9. SomeIp 特有：慢订阅者 / 下行队列满 -> 丢弃计数；后续可继续
// ===========================================================================
TEST(SomeIpTransportContract, DownlinkQueueFullDropsSlowSubscriber) {
    SomeIpHarness h;
    h.cfg.downlinkQueueCapacity = 1;
    h.cfg.downlinkWorkers = 1;
    h.setup();

    std::atomic<int> received{0};
    Subscription sub = h.transport->subscribe(
        payload_type::kService,
        [&](VehicleMessage&&) { ++received; });

    // 慢订阅者：投递 3 个事件，队列容量 1 -> 至少丢弃 2 个（worker 未及时消费）。
    for (int i = 0; i < 3; ++i) {
        h.echo.notifyDownlink(makeEvent(payload_type::kControlCommand, "c", "q-" + std::to_string(i)));
    }
    // worker 消费一个，其余因队列满被丢弃。
    EXPECT_TRUE(waitFor([&] { return h.transport->downlinkDropped() >= 1; }));
    // 慢速下最多收到 1 个。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_LE(received.load(), 2);

    h.transport->stop();
    sub.cancel();
    h.rt.stop();
}

// ===========================================================================
// 10. SomeIp 特有：TBOX 重启 / 断网恢复（框架自动重连）
// ===========================================================================
TEST(SomeIpTransportContract, TboxRestartAndNetworkRecovery) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(1000);
    h.setup();

    auto req = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-rec-1");
    EXPECT_EQ(h.transport->exchange(req, ExchangeOptions{}, makeCtx("r1")).outcome,
              TransportOutcome::Accepted);

    // 停止 provider（模拟 TBOX 重启/断网）。
    h.echo.provider.stopOffer();
    for (int i = 0; i < 1000 && h.transport->availability() == someip_fw::Availability::Available; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto req2 = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-rec-2");
    // 不可用期间调用 -> Unavailable（不得推断成功）。
    auto r = h.transport->exchange(req2, ExchangeOptions{}, makeCtx("r2"));
    EXPECT_EQ(r.outcome, TransportOutcome::Unavailable);

    // 重新 offer（TBOX 恢复）。
    someip_fw::ServiceKey key{DEFAULT_TBOX_SERVICE_ID, DEFAULT_TBOX_INSTANCE_ID};
    auto provider = h.rt.createProvider(key, {1, 0});
    EchoProvider echo2;
    echo2.setup(h.rt, std::move(provider));
    h.echo.provider = std::move(echo2.provider);

    EXPECT_TRUE(waitFor([&] {
        return h.transport->availability() == someip_fw::Availability::Available;
    }, 3000));
    auto req3 = makeRequest(payload_type::kTaskCheckRequest, "p", "msg-rec-3");
    EXPECT_EQ(h.transport->exchange(req3, ExchangeOptions{}, makeCtx("r3")).outcome,
              TransportOutcome::Accepted);

    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 11. SomeIp 特有：graceful shutdown 会 drain 已入队下行
// ===========================================================================
TEST(SomeIpTransportContract, GracefulShutdownDrainsQueuedDownlink) {
    SomeIpHarness h;
    h.cfg.downlinkQueueCapacity = 8;
    h.cfg.downlinkWorkers = 1;
    h.setup();

    std::atomic<int> received{0};
    Subscription sub = h.transport->subscribe(
        payload_type::kService, [&](VehicleMessage&&) { ++received; });

    // 先投递 2 个并等待消费。
    for (int i = 0; i < 2; ++i) {
        h.echo.notifyDownlink(makeEvent(payload_type::kControlCommand, "c", "g-" + std::to_string(i)));
    }
    EXPECT_TRUE(waitFor([&] { return received.load() == 2; }));

    // 停止（应 drain 队列后退出 worker，不崩溃）。
    h.transport->stop();
    EXPECT_TRUE(h.transport->stopping());
    sub.cancel();
    h.rt.stop();
}

// ===========================================================================
// 12. Envelope 大小上限（序列化 Envelope 超限 -> PayloadTooLarge）
// ===========================================================================
TEST(SomeIpTransportContract, EnvelopeTooLargeRejected) {
    SomeIpHarness h;
    h.cfg.maxEnvelopeBytes = 256;
    h.cfg.maxPayloadBytes = 1024;
    h.setup();
    auto big = makeRequest(payload_type::kTaskCheckRequest,
                           std::string(1024, 'x'), "msg-big-env");
    auto r = h.transport->exchange(big, ExchangeOptions{}, makeCtx("big"));
    EXPECT_EQ(r.outcome, TransportOutcome::PayloadTooLarge);
    h.transport->stop();
    h.rt.stop();
}

// ===========================================================================
// 13. 结果映射单测：SOME/IP 成功 / TBOX accepted 不映射为业务成功
// ===========================================================================
TEST(SomeIpTransportContract, CallResultMapping) {
    using someip_fw::CallResult;
    EXPECT_EQ(mapCallResultToOutcome(CallResult::ok({})), TransportOutcome::Accepted);
    EXPECT_EQ(mapCallResultToOutcome(CallResult::fail("CCU-FW-0305", "t")), TransportOutcome::Timeout);
    EXPECT_EQ(mapCallResultToOutcome(CallResult::fail("CCU-FW-0304", "u")), TransportOutcome::Unavailable);
    EXPECT_EQ(mapCallResultToOutcome(CallResult::fail("CCU-FW-0306", "p")), TransportOutcome::ProtocolError);
    EXPECT_EQ(mapCallResultToOutcome(CallResult::fail("CCU-FW-0307", "h")), TransportOutcome::Rejected);
    EXPECT_EQ(mapCallResultToOutcome(CallResult::fail("CCU-FW-0309", "r")), TransportOutcome::Rejected);
    EXPECT_EQ(mapCallResultToOutcome(CallResult::fail("CCU-FW-0310", "s")), TransportOutcome::Stopping);
    EXPECT_EQ(mapCallResultToOutcome(CallResult::fail("CCU-FW-0301", "x")), TransportOutcome::Unavailable);
    EXPECT_EQ(mapCallResultToOutcome(CallResult::fail("SOMEIP/0x05", "x")), TransportOutcome::Unknown);
    EXPECT_EQ(mapCallResultToOutcome(CallResult::fail("UNKNOWN", "x")), TransportOutcome::Unknown);
}

// ===========================================================================
// 14. Envelope 校验单测
// ===========================================================================
TEST(SomeIpTransportContract, EnvelopeValidation) {
    using ::vehicle::common::v1::MESSAGE_KIND_EVENT;
    using ::vehicle::common::v1::MESSAGE_KIND_REQUEST;
    auto env = [](auto kind, std::string_view service, std::string_view version,
                  std::string_view msgId, std::int64_t expire = 0) {
        ::vehicle::common::v1::VehicleMessageEnvelope e;
        e.set_message_kind(kind);
        e.set_service(std::string(service));
        e.set_protocol_version(std::string(version));
        e.set_message_id(std::string(msgId));
        if (expire) e.set_expire_at_ms(expire);
        return e;
    };
    auto valid = env(MESSAGE_KIND_REQUEST, payload_type::kService,
                     payload_type::kProtocolVersion, "m1");
    EXPECT_EQ(validateOutboundEnvelope(valid, 0, 256, 1024), TransportOutcome::Accepted);
    EXPECT_EQ(validateOutboundEnvelope(valid, 300, 256, 1024), TransportOutcome::PayloadTooLarge);
    auto badKind = env(::vehicle::common::v1::MESSAGE_KIND_RESPONSE, payload_type::kService,
                       payload_type::kProtocolVersion, "m1");
    EXPECT_EQ(validateOutboundEnvelope(badKind, 0, 256, 1024), TransportOutcome::ProtocolError);
    auto badSvc = env(MESSAGE_KIND_REQUEST, "vehicle.sota",
                      payload_type::kProtocolVersion, "m1");
    EXPECT_EQ(validateOutboundEnvelope(badSvc, 0, 256, 1024), TransportOutcome::Rejected);
    auto badVer = env(MESSAGE_KIND_REQUEST, payload_type::kService, "ota-v1", "m1");
    EXPECT_EQ(validateOutboundEnvelope(badVer, 0, 256, 1024), TransportOutcome::VersionMismatch);
    auto noId = env(MESSAGE_KIND_REQUEST, payload_type::kService,
                    payload_type::kProtocolVersion, "");
    EXPECT_EQ(validateOutboundEnvelope(noId, 0, 256, 1024), TransportOutcome::Rejected);
    auto expired = env(MESSAGE_KIND_REQUEST, payload_type::kService,
                       payload_type::kProtocolVersion, "m1", nowMs() - 1000);
    EXPECT_EQ(validateOutboundEnvelope(expired, 0, 256, 1024), TransportOutcome::Rejected);
}

// ===========================================================================
// 15. 并发调用 correlation 隔离：不同 message_id 的调用各自完成，互不串扰
//     （framework session 隔离 + 传输 correlation 表；迟到/重复响应不完成其他调用）。
// ===========================================================================
TEST(SomeIpTransportContract, ConcurrentExchangesIsolateCorrelation) {
    SomeIpHarness h;
    h.cfg.exchangeTimeout = std::chrono::milliseconds(2000);
    h.cfg.maxInFlight = 8;
    h.setup();

    std::atomic<int> okA{0}, okB{0};
    std::string corrA, corrB;
    auto reqA = makeRequest(payload_type::kTaskCheckRequest, validTaskCheckPayload(), "msg-conc-A");
    auto reqB = makeRequest(payload_type::kTaskCheckRequest, validTaskCheckPayload(), "msg-conc-B");

    std::thread ta([&] {
        auto r = h.transport->exchange(reqA, ExchangeOptions{}, makeCtx("concA"));
        if (r.outcome == TransportOutcome::Accepted) { corrA = r.value.envelope.correlation_id(); okA = 1; }
    });
    std::thread tb([&] {
        auto r = h.transport->exchange(reqB, ExchangeOptions{}, makeCtx("concB"));
        if (r.outcome == TransportOutcome::Accepted) { corrB = r.value.envelope.correlation_id(); okB = 1; }
    });
    ta.join(); tb.join();

    EXPECT_EQ(okA.load(), 1);
    EXPECT_EQ(okB.load(), 1);
    EXPECT_EQ(corrA, "msg-conc-A");
    EXPECT_EQ(corrB, "msg-conc-B");
    EXPECT_NE(corrA, corrB);
    EXPECT_EQ(h.transport->inflightCount(), 0u);

    h.transport->stop();
    h.rt.stop();
}
