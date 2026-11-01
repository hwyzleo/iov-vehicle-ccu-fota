// =============================================================================
// src/ota/fota_cloud_proxy_via_transport.cpp
// CCU-FOTA FotaCloudProxy 通用传输适配器实现 (CCU-FOTA-DSN-CR-011 §调用流程)
// =============================================================================
// 将 12 个强类型 FotaCloudProxy 方法逐一迁移到通用 exchange。业务 payload 使用
// vehicle.fota.v1 Protobuf 经 FotaMessageCodec 编解码；TransportOutcome 映射为
// FotaCloudException（保留 CCU-FW-03xx 链）。本文件不推进状态机、不记录敏感内容。
// =============================================================================

#include "ccu/fota/ota/fota_cloud_proxy_via_transport.hpp"

namespace ccu_fota {
namespace ota {

using ::vehicle::fota::v1::ConsentReport;
using ::vehicle::fota::v1::ConsentResponse;
using ::vehicle::fota::v1::ControlAckReport;
using ::vehicle::fota::v1::ControlAckResponse;
using ::vehicle::fota::v1::DownloadGrantRequest;
using ::vehicle::fota::v1::DownloadGrantResponse;
using ::vehicle::fota::v1::EventResponse;
using ::vehicle::fota::v1::ExecutionEvent;
using ::vehicle::fota::v1::FinalResultReport;
using ::vehicle::fota::v1::FinalResultResponse;
using ::vehicle::fota::v1::InstallPermitRequest;
using ::vehicle::fota::v1::InstallPermitResponse;
using ::vehicle::fota::v1::LogGrantRequest;
using ::vehicle::fota::v1::LogGrantResponse;
using ::vehicle::fota::v1::LogResultResponse;
using ::vehicle::fota::v1::LogUploadResult;
using ::vehicle::fota::v1::PolicyRequest;
using ::vehicle::fota::v1::PolicyResponse;
using ::vehicle::fota::v1::ReconcileRequest;
using ::vehicle::fota::v1::ReconcileResponse;
using ::vehicle::fota::v1::StageResultReport;
using ::vehicle::fota::v1::StageResultResponse;
using ::vehicle::fota::v1::TaskCheckRequest;
using ::vehicle::fota::v1::TaskCheckResponse;

FotaCloudProxyViaTransport::FotaCloudProxyViaTransport(VehicleMessageTransport& transport,
                                                       std::size_t maxResponseBytes)
    : transport_(transport)
    , maxResponseBytes_(maxResponseBytes) {}

ExchangeOptions FotaCloudProxyViaTransport::makeOptions(const CallContext& ctx) const {
    ExchangeOptions opts;
    opts.timeout = ctx.timeout;
    opts.max_response_bytes = maxResponseBytes_;
    return opts;
}

void FotaCloudProxyViaTransport::throwFor(TransportOutcome outcome,
                                          std::string_view action) const {
    using K = FotaCloudException::Kind;
    switch (outcome) {
        case TransportOutcome::Timeout:
            throw FotaCloudException(K::Timeout, "CCU-FW-0305",
                                     std::string(action) + " timeout");
        case TransportOutcome::Unavailable:
            throw FotaCloudException(K::Unavailable, "CCU-FW-0304",
                                     std::string(action) + " unavailable");
        case TransportOutcome::VersionMismatch:
        case TransportOutcome::PayloadTooLarge:
        case TransportOutcome::ProtocolError:
            throw FotaCloudException(K::Codec, "CCU-FW-0306",
                                     std::string(action) + " protocol error");
        case TransportOutcome::Stopping:
            throw FotaCloudException(K::Internal, "CCU-FW-0307",
                                     std::string(action) + " stopping");
        case TransportOutcome::Rejected:
        case TransportOutcome::Unknown:
        default:
            // 超时/unknown/rejected 视为可重试，由 FotaOrchestrator 用原业务身份重试。
            throw FotaCloudException(K::Timeout, "CCU-FW-0305",
                                     std::string(action) + " unknown outcome");
    }
}

Subscription FotaCloudProxyViaTransport::subscribeDownlink(std::string_view service,
                                                           DownlinkHandler handler) {
    return transport_.subscribe(service, std::move(handler));
}

// ---------------------------------------------------------------------------
// 1. 任务检测
// ---------------------------------------------------------------------------
TaskCheckResponse FotaCloudProxyViaTransport::checkTask(const TaskCheckRequest& request,
                                                        const CallContext& ctx) {
    return exchangeCall<TaskCheckRequest, TaskCheckResponse>(
        payload_type::kTaskCheckRequest, payload_type::kTaskCheckResponse, request, ctx);
}

// ---------------------------------------------------------------------------
// 2. 授权结果上报
// ---------------------------------------------------------------------------
ConsentResponse FotaCloudProxyViaTransport::reportConsent(const ConsentReport& request,
                                                          const CallContext& ctx) {
    return exchangeCall<ConsentReport, ConsentResponse>(
        payload_type::kConsentReport, payload_type::kConsentResponse, request, ctx);
}

// ---------------------------------------------------------------------------
// 3. 下载授权
// ---------------------------------------------------------------------------
DownloadGrantResponse FotaCloudProxyViaTransport::requestDownload(
    const DownloadGrantRequest& request, const CallContext& ctx) {
    return exchangeCall<DownloadGrantRequest, DownloadGrantResponse>(
        payload_type::kDownloadGrantRequest, payload_type::kDownloadGrantResponse,
        request, ctx);
}

// ---------------------------------------------------------------------------
// 4. 下载/校验阶段结果
// ---------------------------------------------------------------------------
StageResultResponse FotaCloudProxyViaTransport::reportStageResult(
    const StageResultReport& request, const CallContext& ctx) {
    return exchangeCall<StageResultReport, StageResultResponse>(
        payload_type::kStageResultReport, payload_type::kStageResultResponse, request, ctx);
}

// ---------------------------------------------------------------------------
// 5. 安装许可
// ---------------------------------------------------------------------------
InstallPermitResponse FotaCloudProxyViaTransport::requestInstall(
    const InstallPermitRequest& request, const CallContext& ctx) {
    return exchangeCall<InstallPermitRequest, InstallPermitResponse>(
        payload_type::kInstallPermitRequest, payload_type::kInstallPermitResponse,
        request, ctx);
}

// ---------------------------------------------------------------------------
// 6. 执行事件（需 EventResponse 水位 -> exchange）
// ---------------------------------------------------------------------------
EventResponse FotaCloudProxyViaTransport::reportEvent(const ExecutionEvent& request,
                                                      const CallContext& ctx) {
    return exchangeCall<ExecutionEvent, EventResponse>(
        payload_type::kExecutionEvent, payload_type::kEventResponse, request, ctx);
}

// ---------------------------------------------------------------------------
// 7. 控制回执
// ---------------------------------------------------------------------------
ControlAckResponse FotaCloudProxyViaTransport::acknowledgeControl(
    const ControlAckReport& request, const CallContext& ctx) {
    return exchangeCall<ControlAckReport, ControlAckResponse>(
        payload_type::kControlAckReport, payload_type::kControlAckResponse, request, ctx);
}

// ---------------------------------------------------------------------------
// 8. 最终结果
// ---------------------------------------------------------------------------
FinalResultResponse FotaCloudProxyViaTransport::reportFinalResult(
    const FinalResultReport& request, const CallContext& ctx) {
    return exchangeCall<FinalResultReport, FinalResultResponse>(
        payload_type::kFinalResultReport, payload_type::kFinalResultResponse, request, ctx);
}

// ---------------------------------------------------------------------------
// 9. 日志上传凭证
// ---------------------------------------------------------------------------
LogGrantResponse FotaCloudProxyViaTransport::requestLogUpload(const LogGrantRequest& request,
                                                              const CallContext& ctx) {
    return exchangeCall<LogGrantRequest, LogGrantResponse>(
        payload_type::kLogGrantRequest, payload_type::kLogGrantResponse, request, ctx);
}

// ---------------------------------------------------------------------------
// 10. 日志上传结果
// ---------------------------------------------------------------------------
LogResultResponse FotaCloudProxyViaTransport::reportLogUpload(const LogUploadResult& request,
                                                              const CallContext& ctx) {
    return exchangeCall<LogUploadResult, LogResultResponse>(
        payload_type::kLogUploadResult, payload_type::kLogResultResponse, request, ctx);
}

// ---------------------------------------------------------------------------
// 11. 对账
// ---------------------------------------------------------------------------
ReconcileResponse FotaCloudProxyViaTransport::reconcile(const ReconcileRequest& request,
                                                        const CallContext& ctx) {
    return exchangeCall<ReconcileRequest, ReconcileResponse>(
        payload_type::kReconcileRequest, payload_type::kReconcileResponse, request, ctx);
}

// ---------------------------------------------------------------------------
// 12. 策略同步
// ---------------------------------------------------------------------------
PolicyResponse FotaCloudProxyViaTransport::syncPolicy(const PolicyRequest& request,
                                                      const CallContext& ctx) {
    return exchangeCall<PolicyRequest, PolicyResponse>(
        payload_type::kPolicyRequest, payload_type::kPolicyResponse, request, ctx);
}

} // namespace ota
} // namespace ccu_fota
