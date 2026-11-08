#pragma once

// =============================================================================
// include/ccu/fota/ota/inventory_error_codes.hpp
// CCU-FOTA 清单读取/登记错误码 (CCU-FOTA-REQ-CR-013 §错误码与可观测)
// =============================================================================
// CR-013 在既有 FOTA-INVENTORY-* 下新增可定位子项。清单读取与安装结果登记是
// 两条独立的失败边界：
//   * 读取失败 -> 不上报、不分配 revision、不推进任务（fail-closed）；
//   * 登记失败 -> 不关闭 Execution（RESULT_PENDING），用同一幂等身份重试。
// 错误码仅用于日志/指标，不改变 vehicle.fota.v1 wire 契约。
// =============================================================================

namespace ccu_fota {
namespace ota {
namespace inventory_error {

// DIAG 清单 payload 非法（缺字段、枚举越界、编码版本不匹配）。
constexpr const char* kDiagPayloadInvalid = "FOTA-INVENTORY-DIAG-PAYLOAD-INVALID";
// DIAG 清单为空（ecuList 缺失或空）：不得作为成功清单上报。
constexpr const char* kDiagListEmpty = "FOTA-INVENTORY-DIAG-LIST-EMPTY";
// 登记能力不可用（DIAG 不可达 / 传输失败 / 未就绪）。
constexpr const char* kRegistrationUnavailable =
    "FOTA-INVENTORY-REGISTRATION-UNAVAILABLE";
// 登记失败且可重试（DIAG RETRYABLE_FAILED、超时、结果未知、缺 registrationId）。
constexpr const char* kRegistrationFailed = "FOTA-INVENTORY-REGISTRATION-FAILED";
// 登记方法 ID 未被 Registry 分配（0）或接口版本未就绪：能力关闭。
constexpr const char* kRegistrationIdUnallocated =
    "FOTA-INVENTORY-REGISTRATION-ID-UNALLOCATED";
// 永久性拒绝（REJECTED_INVALID / IDEMPOTENCY_CONFLICT）：不重试，记录后收口。
constexpr const char* kRegistrationRejected =
    "FOTA-INVENTORY-REGISTRATION-REJECTED";

} // namespace inventory_error
} // namespace ota
} // namespace ccu_fota
