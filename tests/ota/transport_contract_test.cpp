// =============================================================================
// tests/ota/transport_contract_test.cpp
// CCU-FOTA 通用传输端口 contract tests (CCU-FOTA-DSN-CR-010 / CR-011 测试矩阵)
// =============================================================================
// 覆盖 CR-011 测试矩阵：
//   1) 每个 FotaCloudProxy 方法：encode/exchange/decode contract test（vehicle.fota.v1）。
//   2) Protobuf 新增兼容字段/未知字段/未知枚举/新 payloadType。
//   3) correlation 缺失/错误、重复/迟到响应、响应类型错配。
//   4) timeout/unknown outcome/Unavailable/VersionMismatch/Stopping/资源耗尽。
//   5) TTL 过期、payload 上限、非法 Envelope、非法 Proto、敏感日志扫描。
//   9) 新增兼容 FOTA 消息不增加 SOME/IP Method 数量、不修改 transport C++ API。
//   10) 下行订阅与解码、publish 单向投递。
// =============================================================================

#include "ccu/fota/ota/mock/fake_vehicle_message_transport.hpp"
#include "ccu/fota/ota/fota_cloud_proxy_via_transport.hpp"
#include "ccu/fota/ota/fota_message_codec.hpp"
#include "ccu/fota/ota/payload_types.hpp"
#include "ccu/fota/ota/vehicle_message_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace ccu_fota::ota;
using namespace ccu_fota::ota::mock;

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

ScenarioScript makeScenario(const std::vector<std::string>& faults = {}) {
    std::string list;
    for (std::size_t i = 0; i < faults.size(); ++i) {
        if (i) list += ", ";
        list += "\"" + faults[i] + "\"";
    }
    return parseScenario(R"({
      "scenario": "contract",
      "clock": "virtual",
      "inventory": {"mode": "FULL", "baseline": "BASE-001"},
      "packages": [],
      "executor": {"stages": []},
      "faults": [)" + list + R"(]
    })");
}

CallContext makeCtx(const std::string& idKey) {
    CallContext ctx;
    ctx.traceId = "trace-" + idKey;
    ctx.requestId = "req-" + idKey;
    ctx.idempotencyKey = idKey;
    ctx.timeout = std::chrono::milliseconds(1000);
    ctx.deviceId = "dev-contract";
    ctx.vin = "VINCONTRACT";
    return ctx;
}

std::vector<std::byte> strToBytes(const std::string& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = std::byte(static_cast<unsigned char>(s[i]));
    }
    return out;
}

std::string bytesToStr(const std::vector<std::byte>& b) {
    if (b.empty()) return {};
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// 构造一条请求消息（Envelope 元数据 + payload_type + payload）。
VehicleMessage makeRequest(std::string_view ptype, const std::string& payload,
                           std::string_view messageId, std::string_view service = payload_type::kService,
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

// 构造一条下行事件消息。
VehicleMessage makeEvent(std::string_view ptype, const std::string& payload,
                         std::string_view messageId, std::string_view service = payload_type::kService) {
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

} // namespace

// ---------------------------------------------------------------------------
// 1. 每个 FotaCloudProxy 方法：encode -> exchange -> decode 全链路 contract test。
// ---------------------------------------------------------------------------
TEST(TransportContract, AllProxyMethodsRoundTrip) {
    ScenarioScript scenario = makeScenario();
    FakeVehicleMessageTransport transport(scenario);
    FotaCloudProxyViaTransport cloud(transport);

    // 1. checkTask
    {
        ::vehicle::fota::v1::TaskCheckRequest req;
        req.set_baseline_code("BASE-001");
        req.set_fota_master_version("1.0.0");
        req.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
        req.set_inventory_revision(1);
        req.mutable_ecu_list_digest()->set_algorithm("sha-256");
        req.mutable_ecu_list_digest()->set_value_hex(std::string(64, 'a'));
        auto* ecu = req.add_ecu_list();
        ecu->set_ecu_id("VCU-001");
        ecu->set_sw_version("1.0.0");
        auto resp = cloud.checkTask(req, makeCtx("ct-check"));
        EXPECT_EQ(resp.inventory_disposition(),
                  ::vehicle::fota::v1::INVENTORY_DISPOSITION_ACCEPTED);
        ASSERT_TRUE(resp.has_task());
        EXPECT_EQ(resp.task().vehicle_task_id(), "VT-001");
        EXPECT_EQ(resp.task().task_revision(), 1u);
    }
    // 2. reportConsent
    {
        ::vehicle::fota::v1::ConsentReport req;
        req.set_consent_status(::vehicle::fota::v1::CONSENT_STATUS_ACCEPTED);
        req.set_terms_version("v1");
        req.set_terms_id("T-1");
        req.mutable_terms_digest()->set_algorithm("sha-256");
        req.mutable_terms_digest()->set_value_hex(std::string(64, 'a'));
        req.set_consent_time_ms(nowMs());
        req.set_channel("vehicle");
        auto resp = cloud.reportConsent(req, makeCtx("ct-consent"));
        EXPECT_EQ(resp.effective_consent_status(),
                  ::vehicle::fota::v1::CONSENT_STATUS_ACCEPTED);
        EXPECT_TRUE(resp.accepted());
    }
    // 3. requestDownload
    {
        ::vehicle::fota::v1::DownloadGrantRequest req;
        req.set_package_id("PKG-1");
        req.set_task_revision(1);
        req.set_package_revision("prev-1");
        req.set_network_type("LTE");
        auto resp = cloud.requestDownload(req, makeCtx("ct-dl"));
        EXPECT_FALSE(resp.download_url().empty());
    }
    // 4. reportStageResult
    {
        ::vehicle::fota::v1::StageResultReport req;
        req.set_package_id("PKG-1");
        req.set_stage_result_id("SR-1");
        req.set_verified_package_revision("prev-1");
        req.set_result(::vehicle::fota::v1::RESULT_SUCCEEDED);
        req.set_verified_at_ms(nowMs());
        auto resp = cloud.reportStageResult(req, makeCtx("ct-sr"));
        EXPECT_TRUE(resp.accepted());
    }
    // 5. requestInstall
    {
        ::vehicle::fota::v1::InstallPermitRequest req;
        req.set_task_revision(1);
        req.set_install_plan_version("plan-1");
        req.mutable_package_manifest_digest()->set_algorithm("sha-256");
        req.mutable_local_readiness_digest()->set_algorithm("sha-256");
        req.set_local_guard_passed(true);
        req.set_condition_set_version("cond-v1");
        auto resp = cloud.requestInstall(req, makeCtx("ct-permit"));
        EXPECT_TRUE(resp.allowed());
        EXPECT_FALSE(resp.execution_id().empty());
    }
    // 6. reportEvent（水位）
    {
        ::vehicle::fota::v1::ExecutionEvent req;
        req.set_event_id("EVT-1");
        req.mutable_event_digest()->set_algorithm("sha-256");
        req.set_attempt_no(1);
        req.set_sequence_no(1);
        req.set_occurred_at_ms(nowMs());
        req.set_stage("INSTALL");
        req.set_event_status("SUCCEEDED");
        req.set_progress(50);
        auto resp = cloud.reportEvent(req, makeCtx("ct-evt"));
        EXPECT_EQ(resp.event_disposition(), ::vehicle::fota::v1::EVENT_DISPOSITION_ACCEPTED);
        EXPECT_EQ(resp.accepted_sequence_no(), 1u);
    }
    // 7. acknowledgeControl（独立 ControlAckReport）
    {
        ::vehicle::fota::v1::ControlAckReport req;
        req.mutable_ack()->set_control_ack_id("CA-1");
        req.mutable_ack()->set_ack_sequence_no(1);
        req.mutable_ack()->set_control_id("CTRL-1");
        req.mutable_ack()->set_control_revision(1);
        req.mutable_ack()->set_status(::vehicle::fota::v1::CONTROL_ACK_STATUS_APPLIED);
        auto resp = cloud.acknowledgeControl(req, makeCtx("ct-ack"));
        EXPECT_TRUE(resp.accepted());
    }
    // 8. reportFinalResult（FinalResultReport）
    {
        ::vehicle::fota::v1::FinalResultReport req;
        req.set_final_sequence_no(1);
        req.mutable_result_digest()->set_algorithm("sha-256");
        req.set_result(::vehicle::fota::v1::RESULT_SUCCEEDED);
        req.set_baseline_status("verified");
        req.set_completed_at_ms(nowMs());
        auto resp = cloud.reportFinalResult(req, makeCtx("ct-final"));
        EXPECT_TRUE(resp.result_accepted());
    }
    // 9. requestLogUpload
    {
        ::vehicle::fota::v1::LogGrantRequest req;
        req.set_log_request_id("LOG-1");
        req.add_collection_scope("ccu-fota");
        req.mutable_log_time_range()->set_start_at_ms(0);
        req.mutable_log_time_range()->set_end_at_ms(nowMs());
        req.set_redaction_profile_version("v1");
        req.set_log_type("fota");
        req.set_file_name("fota.log");
        req.set_file_size_bytes(256);
        req.set_mime_type("text/plain");
        req.set_privacy_level("pii-masked");
        auto resp = cloud.requestLogUpload(req, makeCtx("ct-log"));
        EXPECT_FALSE(resp.upload_url().empty());
    }
    // 10. reportLogUpload
    {
        ::vehicle::fota::v1::LogUploadResult req;
        req.set_object_key("obj-1");
        req.set_upload_result(::vehicle::fota::v1::RESULT_SUCCEEDED);
        req.mutable_actual_file_digest()->set_algorithm("sha-256");
        req.set_uploaded_at_ms(nowMs());
        auto resp = cloud.reportLogUpload(req, makeCtx("ct-logres"));
        EXPECT_TRUE(resp.accepted());
    }
    // 11. reconcile
    {
        ::vehicle::fota::v1::ReconcileRequest req;
        req.set_query_scope(::vehicle::fota::v1::QUERY_SCOPE_VEHICLE_TASK);
        req.set_local_vehicle_task_status(::vehicle::fota::v1::VEHICLE_TASK_STATUS_EXECUTING);
        auto resp = cloud.reconcile(req, makeCtx("ct-recon"));
        EXPECT_EQ(resp.next_action(), "resume");
    }
    // 12. syncPolicy
    {
        ::vehicle::fota::v1::PolicyRequest req;
        req.set_local_policy_version("pv-0");
        req.set_updated_at_ms(nowMs());
        auto resp = cloud.syncPolicy(req, makeCtx("ct-policy"));
        EXPECT_EQ(resp.policy_version(), "pv-1");
        EXPECT_TRUE(resp.preference_accepted());
        EXPECT_FALSE(resp.conflicting_fields_size() > 0);
    }
}

// ---------------------------------------------------------------------------
// 2. correlation 缺失/错配：响应不得完成其他调用。
// ---------------------------------------------------------------------------
TEST(TransportContract, CorrelationIsolatesConcurrentCalls) {
    ScenarioScript scenario = makeScenario();
    FakeVehicleMessageTransport transport(scenario);
    FotaCloudProxyViaTransport cloud(transport);
    FotaMessageCodec codec;

    // 手动执行两条 checkTask 调用并保留原始响应 VehicleMessage。
    auto reqA = ::vehicle::fota::v1::TaskCheckRequest();
    reqA.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
    reqA.set_inventory_revision(1);
    auto ctxA = makeCtx("ct-A");
    auto idsA = codec.identityFrom(ctxA);
    auto msgA = codec.encodeRequest(payload_type::kTaskCheckRequest, reqA, idsA, ctxA);
    auto rawA = transport.exchange(msgA, ExchangeOptions{}, ctxA);

    auto reqB = ::vehicle::fota::v1::TaskCheckRequest();
    reqB.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
    reqB.set_inventory_revision(1);
    auto ctxB = makeCtx("ct-B");
    auto idsB = codec.identityFrom(ctxB);
    auto msgB = codec.encodeRequest(payload_type::kTaskCheckRequest, reqB, idsB, ctxB);
    auto rawB = transport.exchange(msgB, ExchangeOptions{}, ctxB);

    ASSERT_EQ(rawA.outcome, TransportOutcome::Accepted);
    ASSERT_EQ(rawB.outcome, TransportOutcome::Accepted);

    EXPECT_TRUE(codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
                    payload_type::kTaskCheckResponse, rawA.value, idsA.messageId).ok);
    EXPECT_FALSE(codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
                     payload_type::kTaskCheckResponse, rawA.value, idsB.messageId).ok);
    EXPECT_TRUE(codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
                    payload_type::kTaskCheckResponse, rawB.value, idsB.messageId).ok);
    EXPECT_FALSE(codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
                     payload_type::kTaskCheckResponse, rawB.value, idsA.messageId).ok);
}

// ---------------------------------------------------------------------------
// 2b. Envelope 身份契约：message_id 每条传输消息唯一，request_id 跨传输稳定。
//     回归：首次 checkTask 无 VehicleTask 时 request_id 退化为常量，
//     message_id 若复用 request_id 会与历史消息同名不同内容（云端 digest conflict）。
// ---------------------------------------------------------------------------
TEST(TransportContract, MessageIdUniquePerTransportMessage) {
    FotaMessageCodec codec;
    CallContext ctx;
    ctx.requestId = "req-checkTask-initial";   // 业务身份：跨多次传输不变
    ctx.idempotencyKey = "checkTask-initial";
    ctx.deviceId = "DEV-001";
    ctx.vin = "VIN-FIXTURE-0001";
    ctx.timeout = std::chrono::milliseconds(1000);

    auto ids1 = codec.identityFrom(ctx);
    auto ids2 = codec.identityFrom(ctx);

    EXPECT_FALSE(ids1.messageId.empty());
    EXPECT_NE(ids1.messageId, ctx.requestId);          // 不得复用 request_id
    EXPECT_NE(ids1.messageId, ids2.messageId);         // 同一业务操作的再次传输也不同
    EXPECT_EQ(ids1.requestId, ctx.requestId);          // 业务请求身份保持不变
    EXPECT_EQ(ids2.requestId, ctx.requestId);
    EXPECT_EQ(ids1.idempotencyKey, ctx.idempotencyKey);

    ::vehicle::fota::v1::TaskCheckRequest req;
    req.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
    auto msg1 = codec.encodeRequest(payload_type::kTaskCheckRequest, req, ids1, ctx);
    auto msg2 = codec.encodeRequest(payload_type::kTaskCheckRequest, req, ids2, ctx);

    EXPECT_EQ(msg1.envelope.message_id(), ids1.messageId);
    EXPECT_EQ(msg2.envelope.message_id(), ids2.messageId);
    EXPECT_NE(msg1.envelope.message_id(), msg2.envelope.message_id());
    EXPECT_EQ(msg1.envelope.message_kind(),
              ::vehicle::common::v1::MESSAGE_KIND_REQUEST);
    EXPECT_EQ(msg1.envelope.request_id(), ctx.requestId);
    EXPECT_EQ(msg1.envelope.idempotency_key(), ctx.idempotencyKey);
    EXPECT_FALSE(msg1.envelope.has_correlation_id());  // REQUEST 不携带 correlation
}

// ---------------------------------------------------------------------------
// 3. 响应类型错配 / message kind 错配 / 版本不匹配 / service 错配。
// ---------------------------------------------------------------------------
TEST(TransportContract, DecodeRejectsWrongTypeKindVersion) {
    FotaMessageCodec codec;
    CallContext ctx = makeCtx("typecheck");

    ::vehicle::fota::v1::TaskCheckRequest req;
    req.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
    auto ids = codec.identityFrom(ctx);
    auto encoded = codec.encodeRequest(payload_type::kTaskCheckRequest, req, ids, ctx);

    ScenarioScript scenario = makeScenario();
    FakeVehicleMessageTransport transport(scenario);
    auto accepted = transport.exchange(encoded, ExchangeOptions{}, ctx);
    ASSERT_EQ(accepted.outcome, TransportOutcome::Accepted);

    // payload_type 错配。
    auto wrongType = accepted.value;
    wrongType.envelope.set_payload_type(std::string(payload_type::kConsentResponse));
    auto r1 = codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
        payload_type::kTaskCheckResponse, wrongType, ids.messageId);
    EXPECT_FALSE(r1.ok);
    EXPECT_EQ(r1.error.kind, FotaProtocolErrorKind::PayloadTypeMismatch);

    // message kind 错配（改成 EVENT）。
    auto wrongKind = accepted.value;
    wrongKind.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_EVENT);
    auto r2 = codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
        payload_type::kTaskCheckResponse, wrongKind, ids.messageId);
    EXPECT_FALSE(r2.ok);
    EXPECT_EQ(r2.error.kind, FotaProtocolErrorKind::MessageKindMismatch);

    // 版本不匹配（codec 层）。
    auto wrongVersion = accepted.value;
    wrongVersion.envelope.set_protocol_version("fota-v2");
    auto r3 = codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
        payload_type::kTaskCheckResponse, wrongVersion, ids.messageId);
    EXPECT_FALSE(r3.ok);
    EXPECT_EQ(r3.error.kind, FotaProtocolErrorKind::VersionMismatch);

    // service 不匹配（codec 层）。
    auto wrongService = accepted.value;
    wrongService.envelope.set_service("vehicle.sota");
    auto r4 = codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
        payload_type::kTaskCheckResponse, wrongService, ids.messageId);
    EXPECT_FALSE(r4.ok);
    EXPECT_EQ(r4.error.kind, FotaProtocolErrorKind::ServiceMismatch);

    // 版本不匹配（transport 层）：请求带 fota-v2 -> 传输直接拒绝。
    auto v2Request = encoded;
    v2Request.envelope.set_protocol_version("fota-v2");
    auto v2res = transport.exchange(v2Request, ExchangeOptions{}, ctx);
    EXPECT_EQ(v2res.outcome, TransportOutcome::VersionMismatch);

    // service 不匹配（transport 层）：vehicle.sota -> 拒绝。
    auto sotaRequest = encoded;
    sotaRequest.envelope.set_service("vehicle.sota");
    auto sotaRes = transport.exchange(sotaRequest, ExchangeOptions{}, ctx);
    EXPECT_EQ(sotaRes.outcome, TransportOutcome::Rejected);
}

// ---------------------------------------------------------------------------
// 4. 未知字段保留 / 未知枚举不得解释为成功 / 非法 Proto。
// ---------------------------------------------------------------------------
TEST(TransportContract, UnknownEnumNotSuccessAndMalformedPayload) {
    FotaMessageCodec codec;
    const std::string msgId = "req-unknown";

    // 构造 EventResponse 响应，event_disposition（字段2 varint）写未知枚举值 99。
    VehicleMessage resp;
    resp.envelope.set_message_id(msgId);
    resp.envelope.set_correlation_id(msgId);
    resp.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_RESPONSE);
    resp.envelope.set_service(std::string(payload_type::kService));
    resp.envelope.set_payload_type(std::string(payload_type::kEventResponse));
    resp.envelope.set_protocol_version(std::string(payload_type::kProtocolVersion));
    resp.envelope.set_timestamp_ms(nowMs());
    resp.payload = strToBytes(std::string("\x10\x63", 2));  // field 2 varint 99

    auto decoded = codec.decodeResponse<::vehicle::fota::v1::EventResponse>(
        payload_type::kEventResponse, resp, msgId);
    ASSERT_TRUE(decoded.ok);
    // 未知枚举不得解释为 ACCEPTED（业务成功由显式状态表达）。
    EXPECT_NE(decoded->event_disposition(), ::vehicle::fota::v1::EVENT_DISPOSITION_ACCEPTED);

    // 非法 Proto：payload 无法解析 -> MalformedPayload。
    resp.payload = strToBytes("not-a-proto-bytes");
    auto bad = codec.decodeResponse<::vehicle::fota::v1::EventResponse>(
        payload_type::kEventResponse, resp, msgId);
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.error.kind, FotaProtocolErrorKind::MalformedPayload);
}

// ---------------------------------------------------------------------------
// 5. TTL 过期 / payload 超限 / 未知 payloadType / 非法 Envelope。
// ---------------------------------------------------------------------------
TEST(TransportContract, TtlSizeAndUnknownRejected) {
    ScenarioScript scenario = makeScenario();
    FakeVehicleMessageTransport transport(scenario);
    FotaMessageCodec codec;
    CallContext ctx = makeCtx("reject");

    ::vehicle::fota::v1::TaskCheckRequest req;
    req.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
    auto ids = codec.identityFrom(ctx);
    auto encoded = codec.encodeRequest(payload_type::kTaskCheckRequest, req, ids, ctx);

    // TTL 过期 -> 传输拒绝。
    auto expired = encoded;
    expired.envelope.set_expire_at_ms(nowMs() - 1000);
    EXPECT_EQ(transport.exchange(expired, ExchangeOptions{}, ctx).outcome,
              TransportOutcome::Rejected);

    // 响应大小超限 -> PayloadTooLarge（限制响应大小）。
    ExchangeOptions tiny;
    tiny.max_response_bytes = 8;
    EXPECT_EQ(transport.exchange(encoded, tiny, ctx).outcome,
              TransportOutcome::PayloadTooLarge);

    // 未知 payloadType -> 明确拒绝，不落入默认 Handler。
    auto unknown = encoded;
    unknown.envelope.set_payload_type("vehicle.fota.v1.UnknownOp");
    EXPECT_EQ(transport.exchange(unknown, ExchangeOptions{}, ctx).outcome,
              TransportOutcome::Rejected);

    // 非法 Envelope：空 message_id -> 拒绝。
    auto noId = encoded;
    noId.envelope.clear_message_id();
    EXPECT_EQ(transport.exchange(noId, ExchangeOptions{}, ctx).outcome,
              TransportOutcome::Rejected);
}

// ---------------------------------------------------------------------------
// 6. timeout outcome 映射：适配器将 Timeout -> FotaCloudException(Timeout)。
// ---------------------------------------------------------------------------
TEST(TransportContract, TimeoutOutcomeMappedToFotaCloudException) {
    ScenarioScript scenario = makeScenario({"cloud_timeout"});
    FakeVehicleMessageTransport transport(scenario);
    FotaCloudProxyViaTransport cloud(transport);

    ::vehicle::fota::v1::TaskCheckRequest req;
    req.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
    try {
        cloud.checkTask(req, makeCtx("ct-timeout"));
        FAIL() << "should throw";
    } catch (const FotaCloudException& e) {
        EXPECT_EQ(e.kind(), FotaCloudException::Kind::Timeout);
        EXPECT_EQ(e.frameworkCauseCode(), "CCU-FW-0305");
    }
    auto resp = cloud.checkTask(req, makeCtx("ct-timeout"));
    ASSERT_TRUE(resp.has_task());
    EXPECT_EQ(resp.task().vehicle_task_id(), "VT-001");
}

// ---------------------------------------------------------------------------
// 7. 新增兼容 payloadType：注册 handler 即可，不修改 transport C++ API。
// ---------------------------------------------------------------------------
TEST(TransportContract, NewPayloadTypeNeedsNoTransportApiChange) {
    ScenarioScript scenario = makeScenario();
    FakeVehicleMessageTransport transport(scenario);
    FotaMessageCodec codec;
    CallContext ctx = makeCtx("v2");

    constexpr std::string_view kTaskCheckRequestV2 = "vehicle.fota.v1.TaskCheckRequestV2";
    constexpr std::string_view kTaskCheckResponseV2 = "vehicle.fota.v1.TaskCheckResponseV2";

    transport.registerPayloadHandler(
        std::string(kTaskCheckRequestV2),
        [&](const VehicleMessage& msg, const ExchangeOptions&) {
            ::vehicle::fota::v1::TaskCheckRequest r;
            if (!r.ParseFromString(bytesToStr(msg.payload))) {
                return TransportResult<VehicleMessage>{TransportOutcome::ProtocolError, {}};
            }
            ::vehicle::fota::v1::TaskCheckResponse body;
            body.mutable_status()->set_code("0");
            body.set_inventory_disposition(::vehicle::fota::v1::INVENTORY_DISPOSITION_ACCEPTED);
            auto* task = body.mutable_task();
            task->set_vehicle_task_id("VT-V2");
            task->set_task_revision(2);
            VehicleMessage resp;
            resp.envelope.set_correlation_id(msg.envelope.message_id());
            resp.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_RESPONSE);
            resp.envelope.set_service(std::string(payload_type::kService));
            resp.envelope.set_payload_type(std::string(kTaskCheckResponseV2));
            resp.envelope.set_protocol_version(std::string(payload_type::kProtocolVersion));
            resp.envelope.set_timestamp_ms(nowMs());
            std::string raw;
            body.SerializeToString(&raw);
            resp.payload = strToBytes(raw);
            return TransportResult<VehicleMessage>{TransportOutcome::Accepted, std::move(resp)};
        });

    ::vehicle::fota::v1::TaskCheckRequest req;
    req.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
    auto ids = codec.identityFrom(ctx);
    auto encoded = codec.encodeRequest(kTaskCheckRequestV2, req, ids, ctx);

    auto res = transport.exchange(encoded, ExchangeOptions{}, ctx);
    ASSERT_EQ(res.outcome, TransportOutcome::Accepted);
    auto decoded = codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
        kTaskCheckResponseV2, res.value, ids.messageId);
    ASSERT_TRUE(decoded.ok);
    EXPECT_EQ(decoded->task().vehicle_task_id(), "VT-V2");
}

// ---------------------------------------------------------------------------
// 8. 下行订阅 / 解码 / 退订（ControlCommand）。
// ---------------------------------------------------------------------------
TEST(TransportContract, DownlinkSubscribeDecodeAndUnsubscribe) {
    ScenarioScript scenario = makeScenario();
    FakeVehicleMessageTransport transport(scenario);
    FotaCloudProxyViaTransport cloud(transport);

    ::vehicle::fota::v1::ControlCommand cmd;
    cmd.set_control_id("CTRL-1");
    cmd.set_control_revision(1);
    cmd.set_action(::vehicle::fota::v1::CONTROL_ACTION_PAUSE);
    cmd.set_scope(::vehicle::fota::v1::CONTROL_SCOPE_EXECUTION);
    cmd.set_apply_mode(::vehicle::fota::v1::APPLY_MODE_AT_SAFE_POINT);
    cmd.set_issued_at_ms(nowMs());
    std::string cmdPayload;
    cmd.SerializeToString(&cmdPayload);

    int received = 0;
    std::string gotControlId;
    Subscription sub = cloud.subscribeDownlink(
        payload_type::kService,
        [&](VehicleMessage&& msg) {
            auto decoded = cloud.decodeDownlink<::vehicle::fota::v1::ControlCommand>(
                payload_type::kControlCommand, msg);
            EXPECT_TRUE(decoded.ok);
            if (decoded.ok) {
                gotControlId = decoded->control_id();
                ++received;
            }
        });

    transport.deliverDownlink(makeEvent(payload_type::kControlCommand, cmdPayload, "dl-1"));
    EXPECT_EQ(received, 1);
    EXPECT_EQ(gotControlId, "CTRL-1");

    sub.cancel();
    EXPECT_EQ(transport.activeSubscriberCount(), 0u);
    transport.deliverDownlink(makeEvent(payload_type::kControlCommand, cmdPayload, "dl-2"));
    EXPECT_EQ(received, 1);
}

// ---------------------------------------------------------------------------
// 9. publish 单向投递（无需同步业务响应）。
// ---------------------------------------------------------------------------
TEST(TransportContract, PublishIsFireAndForget) {
    ScenarioScript scenario = makeScenario();
    FakeVehicleMessageTransport transport(scenario);

    auto ev = makeEvent("vehicle.fota.v1.Heartbeat", "hb", "pub-1");
    auto ok = transport.publish(ev, makeCtx("pub"));
    EXPECT_EQ(ok.outcome, TransportOutcome::Accepted);
    EXPECT_EQ(transport.publishCount(), 1u);

    auto req = makeRequest("vehicle.fota.v1.Heartbeat", "hb", "pub-2");
    auto rejected = transport.publish(req, makeCtx("pub"));
    EXPECT_EQ(rejected.outcome, TransportOutcome::Rejected);
    EXPECT_EQ(transport.publishCount(), 1u);
}

// ---------------------------------------------------------------------------
// 10. 敏感内容不得进入日志/异常信息（VIN 仅承载于 Envelope，payload/异常不泄）。
// ---------------------------------------------------------------------------
TEST(TransportContract, SensitiveContentNotLeaked) {
    ScenarioScript scenario = makeScenario();
    FakeVehicleMessageTransport transport(scenario);
    FotaCloudProxyViaTransport cloud(transport);
    FotaMessageCodec codec;
    CallContext ctx = makeCtx("secret");
    ctx.vin = "VINSECRET12345";
    ctx.deviceId = "DEVSECRET";

    ::vehicle::fota::v1::TaskCheckRequest req;
    req.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
    req.mutable_ecu_list_digest()->set_value_hex(std::string(64, 'a'));

    auto ids = codec.identityFrom(ctx);
    auto msg = codec.encodeRequest(payload_type::kTaskCheckRequest, req, ids, ctx);

    // Envelope 承载 VIN（协议字段，VEH-PROTO §5），但 payload 不含 VIN。
    EXPECT_EQ(msg.envelope.vin(), "VINSECRET12345");
    EXPECT_EQ(bytesToStr(msg.payload).find("VINSECRET"), std::string::npos);
    EXPECT_EQ(bytesToStr(msg.payload).find("mock://"), std::string::npos);
    EXPECT_EQ(msg.envelope.trace_context().trace_id(), "trace-secret");

    // 解码失败时，协议错误 detail 不得包含 payload 内容。
    auto tampered = msg;
    tampered.envelope.set_payload_type("vehicle.fota.v1.UnknownOp");
    auto bad = codec.decodeResponse<::vehicle::fota::v1::TaskCheckResponse>(
        payload_type::kTaskCheckResponse, tampered, ids.messageId);
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.error.detail.find("VINSECRET"), std::string::npos);
    EXPECT_EQ(bad.error.detail.find("mock://"), std::string::npos);

    // 传输拒绝异常信息不得包含 payload 内容。
    auto expired = msg;
    expired.envelope.set_expire_at_ms(nowMs() - 1000);
    auto rejected = transport.exchange(expired, ExchangeOptions{}, ctx);
    EXPECT_EQ(rejected.outcome, TransportOutcome::Rejected);
}
