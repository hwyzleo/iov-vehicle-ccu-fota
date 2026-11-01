#pragma once

// =============================================================================
// include/ccu/fota/ota/mock/fake_vehicle_message_transport.hpp
// CCU-FOTA Fake 通用传输实现 (CCU-FOTA-DSN-CR-010 §组件设计/迁移方案 / CR-011)
// =============================================================================
// 仅在 FOTA_ENABLE_TEST_DOUBLES 定义时可用；量产构建不得包含。Fake 与量产
// SomeIpVehicleMessageTransport 实现同一 VehicleMessageTransport 端口，只在该
// 端口边界接收 Envelope + bytes，建立另一套业务协议。
//
// Fake 同时充当「TBOX 通用中继 + 云端 FOTA 端点」两个角色的替身：
//   * 传输边界：校验 message_kind/service/protocol version/TTL/大小/correlation，
//     注入 cloud_timeout 等故障 -> 返回 TransportOutcome。
//   * 云侧端点：按 fully-qualified PayloadType 分发，解码请求、构造业务响应
//     （vehicle.fota.v1）。生产环境 TBOX 中继不解析 payload，本 Fake 解析仅为在
//     无真实云端条件下跑通 FOTA 状态机。
//
// 使用确定性时钟/随机源与可复现场景脚本，不含生产秘密。
// =============================================================================

#ifndef FOTA_ENABLE_TEST_DOUBLES
#error "fake_vehicle_message_transport.hpp is only available with FOTA_ENABLE_TEST_DOUBLES (NON_PRODUCTION)"
#endif

#include "ccu/fota/ota/mock/scenario_script.hpp"
#include "ccu/fota/ota/fota_message_codec.hpp"
#include "ccu/fota/ota/payload_types.hpp"
#include "ccu/fota/ota/vehicle_message_transport.hpp"

#include "vehicle/fota/v1/consent.pb.h"
#include "vehicle/fota/v1/execution.pb.h"
#include "vehicle/fota/v1/log.pb.h"
#include "vehicle/fota/v1/package.pb.h"
#include "vehicle/fota/v1/policy.pb.h"
#include "vehicle/fota/v1/reconcile.pb.h"
#include "vehicle/fota/v1/task.pb.h"
#include "vehicle/fota/v1/types.pb.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ccu_fota {
namespace ota {
namespace mock {

class FakeVehicleMessageTransport : public VehicleMessageTransport {
public:
    explicit FakeVehicleMessageTransport(ScenarioScript s) : scenario_(std::move(s)) {}
    ~FakeVehicleMessageTransport() override = default;

    // ---- VehicleMessageTransport 端口 ----
    TransportResult<VehicleMessage> exchange(const VehicleMessage& msg,
                                             const ExchangeOptions& opts,
                                             const CallContext& ctx) override;
    TransportResult<void> publish(const VehicleMessage& msg, const CallContext& ctx) override;
    Subscription subscribe(std::string_view service, DownlinkHandler handler) override;

    // 云侧下行推送（测试辅助）：按 service 投递给订阅者。
    void deliverDownlink(const VehicleMessage& msg);

    // 测试钩子：注册新的 payloadType 处理器（模拟端点能力扩展/allowlist）。
    // 新增兼容 payloadType 只需注册新 handler，不修改 VehicleMessageTransport C++ API。
    using PayloadHandler =
        std::function<TransportResult<VehicleMessage>(const VehicleMessage&, const ExchangeOptions&)>;
    void registerPayloadHandler(std::string ptype, PayloadHandler handler);

    // ---- 断言辅助（测试用）----
    std::uint64_t acceptedSequenceNo() const { return acceptedSeq_.load(); }
    bool finalAccepted() const { return finalAccepted_.load(); }
    std::uint64_t publishCount() const { return publishCount_.load(); }
    std::size_t activeSubscriberCount() const;

private:
    ScenarioScript scenario_;

    // 云侧状态。
    std::atomic<std::uint64_t> acceptedSeq_{0};
    std::atomic<std::uint64_t> execSeq_{0};
    std::atomic<std::uint64_t> publishCount_{0};
    std::atomic<bool> finalAccepted_{false};

    // 下行订阅表。
    mutable std::mutex subMutex_;
    std::vector<std::pair<std::string, DownlinkHandler>> subscribers_;

    // 自定义 payloadType 处理器（测试钩子）。
    std::map<std::string, PayloadHandler> extraHandlers_;

    // 单次故障注入状态。
    bool etagChangedOnce_ = false;
    bool installDeniedOnce_ = false;
    bool eventDroppedOnce_ = false;
    bool eventReorderOnce_ = false;
    bool cloudTimeoutOnce_ = false;

    static std::int64_t nowMs();

    // 传输边界校验。
    TransportResult<void> validate(const VehicleMessage& msg, const ExchangeOptions& opts) const;
    bool expired(const ::vehicle::common::v1::VehicleMessageEnvelope& env) const;

    // 按 payloadType 分发到云侧端点。
    TransportResult<VehicleMessage> dispatch(const std::string& ptype,
                                             const VehicleMessage& req,
                                             const ExchangeOptions& opts);

    // 构造响应 Envelope + payload，并执行响应大小上限。
    template <typename Resp>
    TransportResult<VehicleMessage> respond(const VehicleMessage& req, std::string_view ptype,
                                            const Resp& body, const ExchangeOptions& opts) const {
        VehicleMessage resp;
        resp.envelope.set_correlation_id(req.envelope.message_id());
        resp.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_RESPONSE);
        resp.envelope.set_service(std::string(payload_type::kService));
        resp.envelope.set_payload_type(std::string(ptype));
        resp.envelope.set_protocol_version(std::string(payload_type::kProtocolVersion));
        resp.envelope.set_timestamp_ms(nowMs());
        if (req.envelope.has_trace_context()) {
            resp.envelope.mutable_trace_context()->set_trace_id(
                req.envelope.trace_context().trace_id());
        }

        std::string raw;
        if (!body.SerializeToString(&raw)) return {TransportOutcome::ProtocolError, {}};
        if (opts.max_response_bytes > 0 &&
            raw.size() > opts.max_response_bytes) {
            return {TransportOutcome::PayloadTooLarge, {}};
        }
        resp.payload = stdStringToBytes(raw);
        return {TransportOutcome::Accepted, std::move(resp)};
    }

    // ---- 云侧端点业务 handlers（vehicle.fota.v1）----
    ::vehicle::fota::v1::TaskCheckResponse handleTaskCheck(const ::vehicle::fota::v1::TaskCheckRequest& req);
    ::vehicle::fota::v1::ConsentResponse handleConsent(const ::vehicle::fota::v1::ConsentReport& req);
    ::vehicle::fota::v1::DownloadGrantResponse handleDownload(const ::vehicle::fota::v1::DownloadGrantRequest& req);
    ::vehicle::fota::v1::StageResultResponse handleStageResult(const ::vehicle::fota::v1::StageResultReport& req);
    ::vehicle::fota::v1::InstallPermitResponse handleInstall(const ::vehicle::fota::v1::InstallPermitRequest& req);
    ::vehicle::fota::v1::EventResponse handleEvent(const ::vehicle::fota::v1::ExecutionEvent& req);
    ::vehicle::fota::v1::ControlAckResponse handleControlAck(const ::vehicle::fota::v1::ControlAckReport& req);
    ::vehicle::fota::v1::FinalResultResponse handleFinalResult(const ::vehicle::fota::v1::FinalResultReport& req);
    ::vehicle::fota::v1::LogGrantResponse handleLogGrant(const ::vehicle::fota::v1::LogGrantRequest& req);
    ::vehicle::fota::v1::LogResultResponse handleLogResult(const ::vehicle::fota::v1::LogUploadResult& req);
    ::vehicle::fota::v1::ReconcileResponse handleReconcile(const ::vehicle::fota::v1::ReconcileRequest& req);
    ::vehicle::fota::v1::PolicyResponse handlePolicy(const ::vehicle::fota::v1::PolicyRequest& req);
};

// ===========================================================================
// 传输边界
// ===========================================================================
inline std::int64_t FakeVehicleMessageTransport::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

inline bool FakeVehicleMessageTransport::expired(
    const ::vehicle::common::v1::VehicleMessageEnvelope& env) const {
    return env.expire_at_ms() > 0 && env.expire_at_ms() < nowMs();
}

inline TransportResult<void> FakeVehicleMessageTransport::validate(
    const VehicleMessage& msg, const ExchangeOptions& opts) const {
    const auto& env = msg.envelope;
    if (env.message_kind() != ::vehicle::common::v1::MESSAGE_KIND_REQUEST) {
        return {TransportOutcome::ProtocolError};
    }
    if (env.service() != payload_type::kService) {
        return {TransportOutcome::Rejected};
    }
    if (env.protocol_version() != payload_type::kProtocolVersion) {
        return {TransportOutcome::VersionMismatch};
    }
    if (env.message_id().empty()) {
        return {TransportOutcome::Rejected};
    }
    if (expired(env)) {
        return {TransportOutcome::Rejected};
    }
    if (opts.max_response_bytes > 0 && msg.payload.size() > opts.max_response_bytes) {
        return {TransportOutcome::PayloadTooLarge};
    }
    return {TransportOutcome::Accepted};
}

inline TransportResult<VehicleMessage> FakeVehicleMessageTransport::exchange(
    const VehicleMessage& msg, const ExchangeOptions& opts, const CallContext& /*ctx*/) {
    // 1. 传输边界校验。
    auto check = validate(msg, opts);
    if (check.outcome != TransportOutcome::Accepted) {
        return {check.outcome, {}};
    }
    // 2. 故障注入（单次，模拟云端超时/不可用）。
    if (scenario_.hasFault("cloud_timeout") && !cloudTimeoutOnce_) {
        cloudTimeoutOnce_ = true;
        return {TransportOutcome::Timeout, {}};
    }
    // 3. 分发到云侧端点。
    return dispatch(msg.envelope.payload_type(), msg, opts);
}

inline TransportResult<void> FakeVehicleMessageTransport::publish(
    const VehicleMessage& msg, const CallContext& /*ctx*/) {
    if (msg.envelope.message_kind() != ::vehicle::common::v1::MESSAGE_KIND_EVENT) {
        return {TransportOutcome::Rejected};
    }
    publishCount_.fetch_add(1);
    return {TransportOutcome::Accepted};
}

inline Subscription FakeVehicleMessageTransport::subscribe(std::string_view service,
                                                           DownlinkHandler handler) {
    std::lock_guard<std::mutex> lock(subMutex_);
    std::size_t idx = subscribers_.size();
    subscribers_.emplace_back(std::string(service), std::move(handler));
    auto* self = this;
    return Subscription([self, idx]() {
        std::lock_guard<std::mutex> guard(self->subMutex_);
        if (idx < self->subscribers_.size()) {
            self->subscribers_[idx].second = nullptr;
        }
    });
}

inline void FakeVehicleMessageTransport::deliverDownlink(const VehicleMessage& msg) {
    // 传输层下行校验：方向（EVENT）、TTL、service 由订阅匹配。
    if (msg.envelope.message_kind() != ::vehicle::common::v1::MESSAGE_KIND_EVENT) return;
    if (expired(msg.envelope)) return;

    std::vector<std::pair<std::string, DownlinkHandler>> snapshot;
    {
        std::lock_guard<std::mutex> lock(subMutex_);
        snapshot = subscribers_;
    }
    for (auto& entry : snapshot) {
        if (entry.second && entry.first == msg.envelope.service()) {
            auto copy = msg;
            entry.second(std::move(copy));
        }
    }
}

inline std::size_t FakeVehicleMessageTransport::activeSubscriberCount() const {
    std::lock_guard<std::mutex> lock(subMutex_);
    return static_cast<std::size_t>(std::count_if(
        subscribers_.begin(), subscribers_.end(),
        [](const auto& e) { return static_cast<bool>(e.second); }));
}

inline void FakeVehicleMessageTransport::registerPayloadHandler(std::string ptype,
                                                                PayloadHandler handler) {
    extraHandlers_[std::move(ptype)] = std::move(handler);
}

// ===========================================================================
// 云侧端点：按 fully-qualified PayloadType 分发
// ===========================================================================
inline TransportResult<VehicleMessage> FakeVehicleMessageTransport::dispatch(
    const std::string& ptype, const VehicleMessage& req, const ExchangeOptions& opts) {
    // 端点能力扩展：注册的自定义 payloadType 优先。
    auto custom = extraHandlers_.find(ptype);
    if (custom != extraHandlers_.end()) {
        return custom->second(req, opts);
    }

    using namespace ::vehicle::fota::v1;

    if (ptype == payload_type::kTaskCheckRequest) {
        TaskCheckRequest r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kTaskCheckResponse, handleTaskCheck(r), opts);
    }
    if (ptype == payload_type::kConsentReport) {
        ConsentReport r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kConsentResponse, handleConsent(r), opts);
    }
    if (ptype == payload_type::kDownloadGrantRequest) {
        DownloadGrantRequest r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kDownloadGrantResponse, handleDownload(r), opts);
    }
    if (ptype == payload_type::kStageResultReport) {
        StageResultReport r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kStageResultResponse, handleStageResult(r), opts);
    }
    if (ptype == payload_type::kInstallPermitRequest) {
        InstallPermitRequest r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kInstallPermitResponse, handleInstall(r), opts);
    }
    if (ptype == payload_type::kExecutionEvent) {
        ExecutionEvent r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kEventResponse, handleEvent(r), opts);
    }
    if (ptype == payload_type::kControlAckReport) {
        ControlAckReport r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kControlAckResponse, handleControlAck(r), opts);
    }
    if (ptype == payload_type::kFinalResultReport) {
        FinalResultReport r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kFinalResultResponse, handleFinalResult(r), opts);
    }
    if (ptype == payload_type::kLogGrantRequest) {
        LogGrantRequest r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kLogGrantResponse, handleLogGrant(r), opts);
    }
    if (ptype == payload_type::kLogUploadResult) {
        LogUploadResult r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kLogResultResponse, handleLogResult(r), opts);
    }
    if (ptype == payload_type::kReconcileRequest) {
        ReconcileRequest r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kReconcileResponse, handleReconcile(r), opts);
    }
    if (ptype == payload_type::kPolicyRequest) {
        PolicyRequest r;
        if (!r.ParseFromString(bytesToStdString(req.payload))) return {TransportOutcome::ProtocolError, {}};
        return respond(req, payload_type::kPolicyResponse, handlePolicy(r), opts);
    }
    // 未知 payloadType：明确拒绝，不得落入错误的默认 Handler。
    return {TransportOutcome::Rejected, {}};
}

// ===========================================================================
// 云侧端点业务 handlers（vehicle.fota.v1）
// ===========================================================================
inline ::vehicle::fota::v1::TaskCheckResponse
FakeVehicleMessageTransport::handleTaskCheck(const ::vehicle::fota::v1::TaskCheckRequest&) {
    using namespace ::vehicle::fota::v1;
    TaskCheckResponse resp;
    resp.mutable_status()->set_code("0");
    if (scenario_.hasFault("state_conflict")) {
        resp.set_inventory_disposition(INVENTORY_DISPOSITION_REVISION_CONFLICT);
        resp.set_availability_status(AVAILABILITY_STATUS_NOT_RELEASED);
        return resp;
    }
    resp.set_inventory_disposition(INVENTORY_DISPOSITION_ACCEPTED);
    resp.set_accepted_inventory_revision(1);
    resp.set_availability_status(AVAILABILITY_STATUS_AVAILABLE);
    resp.set_local_task_disposition("NONE");
    resp.set_package_cache_action("NONE");
    resp.set_download_allowed(true);
    resp.set_install_request_allowed(true);
    resp.set_vehicle_task_status(VEHICLE_TASK_STATUS_DISCOVERED);
    auto* task = resp.mutable_task();
    task->set_activity_id("ACT-001");
    task->set_vehicle_task_id("VT-001");
    task->set_task_revision(1);
    task->set_target_baseline_code(scenario_.baseline);
    task->set_release_at_ms(0);
    task->set_start_time_ms(0);
    task->set_end_time_ms(INT64_MAX);
    task->set_consent_required(true);
    task->set_install_plan_version("plan-1");
    for (const auto& sp : scenario_.packages) {
        auto* p = task->add_package_list();
        p->set_package_id(sp.packageId);
        p->set_package_revision("prev-1");
        p->set_plan_node_id("NODE-1");
        p->set_ecu_id("VCU-001");
        p->set_source_version("1.0.0");
        p->set_target_version("2.0.0");
        p->set_size_bytes(sp.bytes);
        p->mutable_package_digest()->set_algorithm("sha-256");
        p->mutable_package_digest()->set_value_hex(std::string(64, 'e'));
        p->set_differential_type("full");
    }
    return resp;
}

inline ::vehicle::fota::v1::ConsentResponse
FakeVehicleMessageTransport::handleConsent(const ::vehicle::fota::v1::ConsentReport& req) {
    using namespace ::vehicle::fota::v1;
    ConsentResponse resp;
    resp.mutable_status()->set_code("0");
    if (req.consent_status() == CONSENT_STATUS_REJECTED) {
        resp.set_accepted(true);
        resp.set_effective_consent_status(CONSENT_STATUS_REJECTED);
        resp.set_vehicle_task_status(VEHICLE_TASK_STATUS_ENDED);
        resp.set_next_action("stop");
        return resp;
    }
    resp.set_accepted(true);
    resp.set_effective_consent_status(CONSENT_STATUS_ACCEPTED);
    resp.set_consent_receipt_id("RCP-001");
    resp.set_receipt_expires_at_ms(INT64_MAX);
    resp.set_vehicle_task_status(VEHICLE_TASK_STATUS_DOWNLOAD_PENDING);
    resp.set_next_action("proceed");
    return resp;
}

inline ::vehicle::fota::v1::DownloadGrantResponse
FakeVehicleMessageTransport::handleDownload(const ::vehicle::fota::v1::DownloadGrantRequest& req) {
    using namespace ::vehicle::fota::v1;
    DownloadGrantResponse resp;
    resp.mutable_status()->set_code("0");
    resp.set_next_action("proceed");
    resp.set_package_revision("prev-1");
    resp.set_download_url("mock://cdn/" + req.package_id());
    resp.set_expires_at_ms(INT64_MAX);
    resp.set_size_bytes(1048576);
    resp.mutable_package_digest()->set_algorithm("sha-256");
    resp.mutable_package_digest()->set_value_hex(std::string(64, 'e'));
    resp.set_etag("etag-" + req.package_id());
    resp.set_supports_range(true);
    if (scenario_.hasFault("etag_changed") && !etagChangedOnce_) {
        etagChangedOnce_ = true;
        resp.set_etag("etag-NEW-" + req.package_id());
        resp.set_package_revision("prev-2");
    }
    return resp;
}

inline ::vehicle::fota::v1::StageResultResponse
FakeVehicleMessageTransport::handleStageResult(const ::vehicle::fota::v1::StageResultReport&) {
    ::vehicle::fota::v1::StageResultResponse resp;
    resp.mutable_status()->set_code("0");
    resp.set_accepted(true);
    resp.set_next_action("proceed");
    return resp;
}

inline ::vehicle::fota::v1::InstallPermitResponse
FakeVehicleMessageTransport::handleInstall(const ::vehicle::fota::v1::InstallPermitRequest&) {
    using namespace ::vehicle::fota::v1;
    InstallPermitResponse resp;
    resp.mutable_status()->set_code("0");
    if (scenario_.hasFault("guard_failed") && !installDeniedOnce_) {
        installDeniedOnce_ = true;
        resp.set_allowed(false);
        resp.set_deny_reason("guard_failed");
        resp.set_next_retry_at_ms(0);
        return resp;
    }
    resp.set_allowed(true);
    resp.set_execution_id("EX-" + std::to_string(execSeq_.fetch_add(1) + 1));
    resp.set_attempt_no(1);
    resp.set_permit_id("PMT-001");
    resp.set_permit_token("permit-tok");
    resp.set_control_revision(1);
    resp.set_issued_at_ms(nowMs());
    resp.set_valid_until_ms(INT64_MAX);
    resp.set_install_plan_version("plan-1");
    auto* op = resp.mutable_offline_policy();
    op->set_allowed(true);
    op->set_max_offline_duration_sec(60);
    op->set_heartbeat_timeout_sec(300);
    auto* tp = resp.mutable_timeout_policy();
    tp->set_max_execution_duration_sec(300);
    return resp;
}

inline ::vehicle::fota::v1::EventResponse
FakeVehicleMessageTransport::handleEvent(const ::vehicle::fota::v1::ExecutionEvent& req) {
    using namespace ::vehicle::fota::v1;
    EventResponse resp;
    resp.mutable_status()->set_code("0");
    if (scenario_.hasFault("event_drop") && !eventDroppedOnce_ && req.sequence_no() == 2) {
        eventDroppedOnce_ = true;
        resp.set_event_disposition(EVENT_DISPOSITION_BUFFERED);
        resp.set_accepted_sequence_no(1);
        return resp;
    }
    if (scenario_.hasFault("event_reorder") && !eventReorderOnce_ && req.sequence_no() == 3) {
        eventReorderOnce_ = true;
        resp.set_event_disposition(EVENT_DISPOSITION_BUFFERED);
        resp.set_accepted_sequence_no(1);
        auto* r = resp.add_missing_sequence_ranges();
        r->set_start(2);
        r->set_end(2);
        return resp;
    }
    resp.set_event_disposition(EVENT_DISPOSITION_ACCEPTED);
    resp.set_accepted_sequence_no(req.sequence_no());
    resp.set_vehicle_task_status(VEHICLE_TASK_STATUS_EXECUTING);
    acceptedSeq_ = req.sequence_no();
    return resp;
}

inline ::vehicle::fota::v1::ControlAckResponse
FakeVehicleMessageTransport::handleControlAck(const ::vehicle::fota::v1::ControlAckReport& req) {
    ::vehicle::fota::v1::ControlAckResponse resp;
    resp.mutable_status()->set_code("0");
    resp.set_accepted(true);
    resp.set_current_control_status(req.ack().status());
    return resp;
}

inline ::vehicle::fota::v1::FinalResultResponse
FakeVehicleMessageTransport::handleFinalResult(const ::vehicle::fota::v1::FinalResultReport&) {
    using namespace ::vehicle::fota::v1;
    FinalResultResponse resp;
    resp.mutable_status()->set_code("0");
    resp.set_result_accepted(true);
    resp.set_execution_status(EXECUTION_STATUS_SUCCEEDED);
    resp.set_accepted_sequence_no(1);
    resp.set_vehicle_task_status(VEHICLE_TASK_STATUS_COMPLETED);
    resp.set_next_action("proceed");
    finalAccepted_ = true;
    return resp;
}

inline ::vehicle::fota::v1::LogGrantResponse
FakeVehicleMessageTransport::handleLogGrant(const ::vehicle::fota::v1::LogGrantRequest& req) {
    using namespace ::vehicle::fota::v1;
    LogGrantResponse resp;
    resp.mutable_status()->set_code("0");
    resp.set_upload_url("mock://logs/" + req.log_request_id());
    resp.set_object_key("logobj-" + req.log_request_id());
    resp.set_expires_at_ms(INT64_MAX);
    resp.set_max_size_bytes(1048576);
    return resp;
}

inline ::vehicle::fota::v1::LogResultResponse
FakeVehicleMessageTransport::handleLogResult(const ::vehicle::fota::v1::LogUploadResult&) {
    ::vehicle::fota::v1::LogResultResponse resp;
    resp.mutable_status()->set_code("0");
    resp.set_accepted(true);
    return resp;
}

inline ::vehicle::fota::v1::ReconcileResponse
FakeVehicleMessageTransport::handleReconcile(const ::vehicle::fota::v1::ReconcileRequest&) {
    using namespace ::vehicle::fota::v1;
    ReconcileResponse resp;
    resp.mutable_status()->set_code("0");
    resp.set_vehicle_task_status(VEHICLE_TASK_STATUS_EXECUTING);
    resp.set_execution_status(EXECUTION_STATUS_INSTALLING);
    resp.set_next_action("resume");
    return resp;
}

inline ::vehicle::fota::v1::PolicyResponse
FakeVehicleMessageTransport::handlePolicy(const ::vehicle::fota::v1::PolicyRequest&) {
    using namespace ::vehicle::fota::v1;
    PolicyResponse resp;
    resp.mutable_status()->set_code("0");
    auto* ep = resp.mutable_effective_policy();
    ep->set_auto_download(true);
    ep->set_install_mode("auto");
    ep->set_max_retry_attempts(3);
    resp.set_policy_version("pv-1");
    resp.set_effective_at_ms(nowMs());
    resp.set_preference_accepted(true);
    resp.set_current_preference_version("pref-1");
    return resp;
}

} // namespace mock
} // namespace ota
} // namespace ccu_fota
