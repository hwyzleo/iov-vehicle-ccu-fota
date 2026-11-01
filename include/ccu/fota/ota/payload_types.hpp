#pragma once

// =============================================================================
// include/ccu/fota/ota/payload_types.hpp
// CCU-FOTA 通用传输 PayloadType Registry (CCU-FOTA-DSN-CR-011 §Envelope 与 Codec)
// =============================================================================
// payload_type 使用 Protobuf fully-qualified message name（如
// "vehicle.fota.v1.TaskCheckRequest"），service 固定为 "vehicle.fota"，协议版本
// 固定为 "fota-v1"。TBOX 依据 service/payloadType allowlist 路由但不得解析 payload
// 字段。新增向后兼容业务消息只增加新的 PayloadType，不修改 VehicleMessageTransport
// C++ API，也不新增 SOME/IP Method/Event。
//
// 命名规则：vehicle.fota.v1.<MessageName>；禁止自定义短名、Topic 名或 ota.* 别名。
// =============================================================================

#include <string_view>

namespace ccu_fota {
namespace ota {
namespace payload_type {

// 稳定业务域标识与协议版本（VEH-PROTO §5.3）。
inline constexpr std::string_view kService = "vehicle.fota";
inline constexpr std::string_view kProtocolVersion = "fota-v1";

// 1. 任务检测
inline constexpr std::string_view kTaskCheckRequest  = "vehicle.fota.v1.TaskCheckRequest";
inline constexpr std::string_view kTaskCheckResponse = "vehicle.fota.v1.TaskCheckResponse";

// 2. 授权结果上报
inline constexpr std::string_view kConsentReport   = "vehicle.fota.v1.ConsentReport";
inline constexpr std::string_view kConsentResponse = "vehicle.fota.v1.ConsentResponse";

// 3. 下载授权
inline constexpr std::string_view kDownloadGrantRequest  = "vehicle.fota.v1.DownloadGrantRequest";
inline constexpr std::string_view kDownloadGrantResponse = "vehicle.fota.v1.DownloadGrantResponse";

// 4. 下载/校验阶段结果
inline constexpr std::string_view kStageResultReport   = "vehicle.fota.v1.StageResultReport";
inline constexpr std::string_view kStageResultResponse = "vehicle.fota.v1.StageResultResponse";

// 5. 安装许可
inline constexpr std::string_view kInstallPermitRequest  = "vehicle.fota.v1.InstallPermitRequest";
inline constexpr std::string_view kInstallPermitResponse = "vehicle.fota.v1.InstallPermitResponse";

// 6. 执行事件
inline constexpr std::string_view kExecutionEvent = "vehicle.fota.v1.ExecutionEvent";
inline constexpr std::string_view kEventResponse  = "vehicle.fota.v1.EventResponse";

// 7. 控制下行（EVENT）与回执
inline constexpr std::string_view kControlCommand     = "vehicle.fota.v1.ControlCommand";
inline constexpr std::string_view kControlAckReport   = "vehicle.fota.v1.ControlAckReport";
inline constexpr std::string_view kControlAckResponse = "vehicle.fota.v1.ControlAckResponse";

// 8. 最终结果
inline constexpr std::string_view kFinalResultReport   = "vehicle.fota.v1.FinalResultReport";
inline constexpr std::string_view kFinalResultResponse = "vehicle.fota.v1.FinalResultResponse";

// 9. 日志上传凭证
inline constexpr std::string_view kLogGrantRequest  = "vehicle.fota.v1.LogGrantRequest";
inline constexpr std::string_view kLogGrantResponse = "vehicle.fota.v1.LogGrantResponse";

// 10. 日志上传结果
inline constexpr std::string_view kLogUploadResult = "vehicle.fota.v1.LogUploadResult";
inline constexpr std::string_view kLogResultResponse = "vehicle.fota.v1.LogResultResponse";

// 11. 对账
inline constexpr std::string_view kReconcileRequest  = "vehicle.fota.v1.ReconcileRequest";
inline constexpr std::string_view kReconcileResponse = "vehicle.fota.v1.ReconcileResponse";

// 12. 策略同步
inline constexpr std::string_view kPolicyRequest  = "vehicle.fota.v1.PolicyRequest";
inline constexpr std::string_view kPolicyResponse = "vehicle.fota.v1.PolicyResponse";

} // namespace payload_type
} // namespace ota
} // namespace ccu_fota
