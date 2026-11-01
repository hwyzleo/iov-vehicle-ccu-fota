#pragma once

// =============================================================================
// include/ccu/fota/ota/ports/consent_provider.hpp
// CCU-FOTA 授权提供者端口 (CCU-FOTA-DSN-CR-009 §授权 / CR-011 类型校准)
// =============================================================================
// ConsentProvider 输出用户选择及条款身份；上报使用稳定 idempotencyKey。
// 不得以响应 accepted 推断用户同意；必须使用 effective_consent_status。
// 条款身份（terms_version/terms_id/terms_digest）来自云端任务快照（VEH-PROTO）。
// =============================================================================

#include "vehicle/fota/v1/types.pb.h"

#include <string>

namespace ccu_fota {
namespace ota {

// 条款身份（扁平化，见 vehicle.fota.v1 ConsentReport）。
struct ConsentTermsId {
    std::string termsVersion;
    std::string termsId;
    std::string termsDigestHex;   // 小写十六进制
    std::string algorithm;        // 如 "sha-256"
};

// 授权提供者返回的用户选择结果。
struct ConsentChoice {
    ::vehicle::fota::v1::ConsentStatus userChoice =
        ::vehicle::fota::v1::CONSENT_STATUS_UNSPECIFIED;
    ConsentTermsId terms;
    bool termsAvailable = false;       // 是否取得有效条款身份
};

class ConsentProvider {
public:
    virtual ~ConsentProvider() = default;

    // 请求用户授权结果（HMI/TBOX 或 Mock）。terms 为云端下发的条款身份。
    virtual ConsentChoice
    requestConsent(const std::string& vehicleTaskId, const ConsentTermsId& terms) = 0;
};

} // namespace ota
} // namespace ccu_fota
