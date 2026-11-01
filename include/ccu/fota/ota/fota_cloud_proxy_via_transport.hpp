#pragma once

// =============================================================================
// include/ccu/fota/ota/fota_cloud_proxy_via_transport.hpp
// CCU-FOTA FotaCloudProxy 通用传输适配器 (CCU-FOTA-DSN-CR-010 §架构决策/接口设计)
// =============================================================================
// FotaCloudProxyViaTransport 是 FotaCloudProxy 的具体实现：在稳定的
// VehicleMessageTransport 端口之上，把 12 个强类型 FOTA 业务方法逐一迁移到通用
// exchange/publish。FotaOrchestrator 继续只依赖强类型 FotaCloudProxy 接口，本类
// 不推进任何 Task/VehicleTask/Execution 状态机。
//
// 职责：
//   * 选择 fully-qualified PayloadType、序列化/反序列化 vehicle.fota.v1（经
//     FotaMessageCodec）。
//   * 校验业务响应类型与 correlation；将 TransportOutcome 映射为 FotaCloudException。
//   * 下行经 subscribeDownlink 订阅，按 payloadType 解码后交业务回调。
//
// 本次 vehicle.fota.v1 迁移不新增 SOME/IP Method/Event，不修改
// VehicleMessageTransport C++ API（CCU-FOTA-DSN-CR-011）。
// =============================================================================

#include "ccu/fota/ota/fota_cloud_proxy.hpp"
#include "ccu/fota/ota/fota_message_codec.hpp"
#include "ccu/fota/ota/payload_types.hpp"
#include "ccu/fota/ota/vehicle_message_transport.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace ccu_fota {
namespace ota {

class FotaCloudProxyViaTransport : public FotaCloudProxy {
public:
    explicit FotaCloudProxyViaTransport(VehicleMessageTransport& transport,
                                        std::size_t maxResponseBytes = 0);
    ~FotaCloudProxyViaTransport() override = default;

    // ---- 强类型业务方法（内部经通用 exchange）----
    ::vehicle::fota::v1::TaskCheckResponse
    checkTask(const ::vehicle::fota::v1::TaskCheckRequest& request,
              const CallContext& ctx) override;
    ::vehicle::fota::v1::ConsentResponse
    reportConsent(const ::vehicle::fota::v1::ConsentReport& request,
                  const CallContext& ctx) override;
    ::vehicle::fota::v1::DownloadGrantResponse
    requestDownload(const ::vehicle::fota::v1::DownloadGrantRequest& request,
                    const CallContext& ctx) override;
    ::vehicle::fota::v1::StageResultResponse
    reportStageResult(const ::vehicle::fota::v1::StageResultReport& request,
                      const CallContext& ctx) override;
    ::vehicle::fota::v1::InstallPermitResponse
    requestInstall(const ::vehicle::fota::v1::InstallPermitRequest& request,
                   const CallContext& ctx) override;
    ::vehicle::fota::v1::EventResponse
    reportEvent(const ::vehicle::fota::v1::ExecutionEvent& request,
                const CallContext& ctx) override;
    ::vehicle::fota::v1::ControlAckResponse
    acknowledgeControl(const ::vehicle::fota::v1::ControlAckReport& request,
                       const CallContext& ctx) override;
    ::vehicle::fota::v1::FinalResultResponse
    reportFinalResult(const ::vehicle::fota::v1::FinalResultReport& request,
                      const CallContext& ctx) override;
    ::vehicle::fota::v1::LogGrantResponse
    requestLogUpload(const ::vehicle::fota::v1::LogGrantRequest& request,
                     const CallContext& ctx) override;
    ::vehicle::fota::v1::LogResultResponse
    reportLogUpload(const ::vehicle::fota::v1::LogUploadResult& request,
                    const CallContext& ctx) override;
    ::vehicle::fota::v1::ReconcileResponse
    reconcile(const ::vehicle::fota::v1::ReconcileRequest& request,
              const CallContext& ctx) override;
    ::vehicle::fota::v1::PolicyResponse
    syncPolicy(const ::vehicle::fota::v1::PolicyRequest& request,
               const CallContext& ctx) override;

    // ---- 下行 ----
    // 订阅 FOTA 业务域下行入口（透明转发给 transport；解码由回调经 decodeDownlink 完成）。
    Subscription subscribeDownlink(std::string_view service, DownlinkHandler handler);

    // 按 payloadType 解码下行消息（事件形态）为强类型业务消息。
    template <typename Message>
    Expected<Message> decodeDownlink(std::string_view ptype,
                                     const VehicleMessage& msg) const {
        return codec_.decodeDownlink<Message>(ptype, msg);
    }

private:
    VehicleMessageTransport& transport_;
    std::size_t maxResponseBytes_;
    FotaMessageCodec codec_;

    ExchangeOptions makeOptions(const CallContext& ctx) const;
    void throwFor(TransportOutcome outcome, std::string_view action) const;

    // 通用 exchange 封装：identity -> encode -> exchange -> outcome 映射 -> decode -> 返回强类型响应。
    template <typename Request, typename Response>
    Response exchangeCall(std::string_view reqType, std::string_view respType,
                          const Request& request, const CallContext& ctx) {
        auto ids = codec_.identityFrom(ctx);
        auto msg = codec_.encodeRequest(reqType, request, ids, ctx);
        auto result = transport_.exchange(msg, makeOptions(ctx), ctx);
        if (result.outcome != TransportOutcome::Accepted) {
            throwFor(result.outcome, reqType);
        }
        auto decoded = codec_.decodeResponse<Response>(respType, result.value, ids.messageId);
        if (!decoded) {
            throw FotaCloudException(FotaCloudException::Kind::Codec,
                                     decoded.error.frameworkCauseCode,
                                     std::string("codec: ") + decoded.error.detail);
        }
        return std::move(*decoded);
    }
};

} // namespace ota
} // namespace ccu_fota
