// =============================================================================
// include/ccu/fota/someip/error_mapper.hpp
// CCU-FOTA SOME/IP framework 错误映射 (CCU-FOTA-DSN-CR-007 §错误映射)
// =============================================================================
// 将 ccu-framework-someip 的 CCU-FW-03xx 错误原因链映射为 FOTA 业务错误码，
// 并保留 framework cause / peer / operation / attempt / phase。公共 API 不泄漏
// 后端枚举或异常 (CR-007 §错误映射 / §影响与边界)。
//
// 映射表（CR-007 §错误映射）：
//   来源                       framework       FOTA 处理
//   Runtime/配置/Registry      0301/0302       阻止服务开放（启动失败）
//   offer/request/discovery    0303            有界恢复；长期不可用则降级
//   DIAG 不可用/版本不匹配     0304            不调用；采集失败保留原因链
//   DIAG timeout/transport     0305            按业务策略重试；DIAG 业务响应错误保持原码
//   TBOX 不可用/版本不匹配     0304            CCU-FOTA-1005，保留 0304 cause
//   TBOX send/transport        0303/0305       CCU-FOTA-1004，结果未知保持 active_job
//   TBOX timeout               0305            CCU-FOTA-1006，按同一幂等身份重试
//   payload/codec/大小         0306            入站返回 wire error；快照编码失败映射 1003
//   handler/response           0307            返回 wire error 或本轮失败
//   资源上限                   0309            有界拒绝/busy
//   shutdown/backend           0310            尽力清理并报告不完整停机
// =============================================================================

#pragma once

#include "error_codes.h"
#include "ccu/fw/someip/types.hpp"

#include <string>

namespace ccu_fota {
namespace someip {

// 映射结果：业务错误码 + 原始 framework cause（保留原因链）+ 描述。
struct MappedError {
    ErrorCode code{ErrorCode::SUCCESS};
    std::string cause;     // 原始 CCU-FW-03xx code（如 "CCU-FW-0305"）
    std::string message;
};

// DIAG 采集调用错误映射。
// - 0304 (unavailable/version mismatch): 采集失败，不调用 (CCU_FOTA_1006 视为采集超时/失败)
// - 0305 (timeout/transport): 采集超时/传输失败 (CCU_FOTA_1006)
// - 0306 (payload/codec): 解析失败 (CCU_DIAG_1005)
// - 0303/0307/0309/0310: 通用采集失败 (CCU_DIAG_1006)
// DIAG 业务响应错误（return code 非 0）保持原码，由调用方透传。
MappedError mapDiagCallError(const ccu::fw::someip::CallResult& result);

// TBOX 提交调用错误映射。
// - 0304 (unavailable/version mismatch): TBOX 不可达 (CCU_FOTA_1005)
// - 0303/0305 transport failure: 车内发送失败 (CCU_FOTA_1004)，结果未知
// - 0305 timeout: 上报超时 (CCU_FOTA_1006)
// - 0306 (payload/codec/size): 快照编码失败 (CCU_FOTA_1003)
// - 0307/0309/0310: 车内发送失败 (CCU_FOTA_1004)
MappedError mapTboxCallError(const ccu::fw::someip::CallResult& result);

// 启动期 Runtime/配置/Registry 错误是否阻止服务开放 (0301/0302)。
bool isStartupBlocking(const std::string& frameworkCode);

// 资源上限错误 (0309) 是否应有界拒绝。
bool isResourceLimit(const std::string& frameworkCode);

// 将 framework 错误码字符串规范化（去前缀取数字），便于比较。
// 例如 "CCU-FW-0305" -> "0305"。
std::string frameworkErrorNumber(const std::string& code);

} // namespace someip
} // namespace ccu_fota
