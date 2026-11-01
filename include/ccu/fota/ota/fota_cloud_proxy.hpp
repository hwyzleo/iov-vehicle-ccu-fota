#pragma once

// =============================================================================
// include/ccu/fota/ota/fota_cloud_proxy.hpp
// CCU-FOTA 车云 FOTA 代理接口 (CCU-FOTA-DSN-CR-011 §强类型接口)
// =============================================================================
// CCU-FOTA 不直接建立 MQTT/HTTP 云连接；所有车云业务调用经 TBOX 的 FotaCloudProxy，
// 沿用 CCU-FOTA ↔ TBOX SOME/IP 和 TBOX ↔ 云端安全通道。业务 payload 统一 Protobuf
// （vehicle.fota.v1）。
//
// 方法名是模块内部语义名；SOME/IP Method/Event ID 必须由 Service Registry 和权威
// IDL 分配，不在本 CR 中硬编码。本次 FOTA 业务 package 迁移不新增按操作绑定的
// SOME/IP Method/Event。
//
// 传输/框架失败（timeout/unavailable/codec）抛 FotaCloudException；业务错误通过
// 各 Response 的 status.code/reason 字段返回。framework 自动重试关闭，业务重试由
// FotaOrchestrator 使用 durable 状态和同一幂等身份执行。
// =============================================================================

#include "ccu/fota/ota/call_context.hpp"

#include "vehicle/common/v1/envelope.pb.h"
#include "vehicle/fota/v1/types.pb.h"
#include "vehicle/fota/v1/task.pb.h"
#include "vehicle/fota/v1/consent.pb.h"
#include "vehicle/fota/v1/package.pb.h"
#include "vehicle/fota/v1/execution.pb.h"
#include "vehicle/fota/v1/log.pb.h"
#include "vehicle/fota/v1/reconcile.pb.h"
#include "vehicle/fota/v1/policy.pb.h"

#include <stdexcept>
#include <string>

namespace ccu_fota {
namespace ota {

// ---------------------------------------------------------------------------
// FotaCloudException - 传输/框架失败（业务错误见各 Response.status）
// frameworkCauseCode 保留 CCU-FW-03xx 链，不得丢失。
// ---------------------------------------------------------------------------
class FotaCloudException : public std::runtime_error {
public:
    enum class Kind {
        Timeout,        // 调用超时 -> 业务映射 FOTA-*-TIMEOUT
        Unavailable,    // TBOX/服务不可用 -> FOTA-*-UNAVAILABLE
        Codec,          // payload/codec 失败 -> FOTA-*-CODEC
        Internal,       // runtime/resource/shutdown
    };

    FotaCloudException(Kind k, std::string frameworkCauseCode, std::string detail)
        : std::runtime_error(detail)
        , kind_(k)
        , frameworkCauseCode_(std::move(frameworkCauseCode)) {}

    Kind kind() const { return kind_; }
    const std::string& frameworkCauseCode() const { return frameworkCauseCode_; }

private:
    Kind kind_;
    std::string frameworkCauseCode_;
};

// ---------------------------------------------------------------------------
// FotaCloudProxy - 车云 FOTA 业务代理（纯虚接口，供真实 SOME/IP 适配与 Mock/桩实现）。
// 所有未限定类型均来自 vehicle::fota::v1。
// ---------------------------------------------------------------------------
class FotaCloudProxy {
public:
    virtual ~FotaCloudProxy() = default;

    // 1. 任务检测
    virtual ::vehicle::fota::v1::TaskCheckResponse
    checkTask(const ::vehicle::fota::v1::TaskCheckRequest& req, const CallContext& ctx) = 0;

    // 2. 授权结果上报
    virtual ::vehicle::fota::v1::ConsentResponse
    reportConsent(const ::vehicle::fota::v1::ConsentReport& req, const CallContext& ctx) = 0;

    // 3. 下载授权
    virtual ::vehicle::fota::v1::DownloadGrantResponse
    requestDownload(const ::vehicle::fota::v1::DownloadGrantRequest& req, const CallContext& ctx) = 0;

    // 4. 下载/校验阶段结果
    virtual ::vehicle::fota::v1::StageResultResponse
    reportStageResult(const ::vehicle::fota::v1::StageResultReport& req, const CallContext& ctx) = 0;

    // 5. 安装许可
    virtual ::vehicle::fota::v1::InstallPermitResponse
    requestInstall(const ::vehicle::fota::v1::InstallPermitRequest& req, const CallContext& ctx) = 0;

    // 6. 执行事件
    virtual ::vehicle::fota::v1::EventResponse
    reportEvent(const ::vehicle::fota::v1::ExecutionEvent& req, const CallContext& ctx) = 0;

    // 7. 控制回执（独立 ControlAckReport）
    virtual ::vehicle::fota::v1::ControlAckResponse
    acknowledgeControl(const ::vehicle::fota::v1::ControlAckReport& req, const CallContext& ctx) = 0;

    // 8. 最终结果（FinalResultReport）
    virtual ::vehicle::fota::v1::FinalResultResponse
    reportFinalResult(const ::vehicle::fota::v1::FinalResultReport& req, const CallContext& ctx) = 0;

    // 9. 日志上传凭证
    virtual ::vehicle::fota::v1::LogGrantResponse
    requestLogUpload(const ::vehicle::fota::v1::LogGrantRequest& req, const CallContext& ctx) = 0;

    // 10. 日志上传结果
    virtual ::vehicle::fota::v1::LogResultResponse
    reportLogUpload(const ::vehicle::fota::v1::LogUploadResult& req, const CallContext& ctx) = 0;

    // 11. 对账
    virtual ::vehicle::fota::v1::ReconcileResponse
    reconcile(const ::vehicle::fota::v1::ReconcileRequest& req, const CallContext& ctx) = 0;

    // 12. 策略同步
    virtual ::vehicle::fota::v1::PolicyResponse
    syncPolicy(const ::vehicle::fota::v1::PolicyRequest& req, const CallContext& ctx) = 0;
};

} // namespace ota
} // namespace ccu_fota
