#pragma once

// =============================================================================
// include/ccu/fota/ota/ports/installed_version_registrar.hpp
// CCU-FOTA 安装结果登记端口 (CCU-FOTA-DSN-CR-013 §2 / US-037)
// =============================================================================
// 职责：在云端接受最终结果后，把“本次 Execution 实际安装的版本事实”登记到
// CCU-DIAG（清单权威方）。FOTA 不写 DIAG 存储、不推算清单版本（CR-013 D1/D2）。
//
// 幂等语义：
//   * 幂等身份 = executionId + attemptNo（report.idempotencyKey 同源）；
//   * 重试必须保持 items 内容不变，否则 DIAG 返回 IDEMPOTENCY_CONFLICT；
//   * 收口条件 = 处置为 APPLIED/DUPLICATE 且拿到 registrationId 并 durable 保存。
//
// 处置四态与 FOTA 处置（CR-013 §跨 CR 对齐（CCU-DIAG-DSN-CR-010））：
//   APPLIED / DUPLICATE     -> 记录 registrationId 并收口 Execution
//   RETRYABLE_FAILED / 超时 -> 保持 RESULT_PENDING，用同一身份重试
//   REJECTED_INVALID        -> ERROR 事件后收口，不重试
//   UNAVAILABLE             -> 能力不可用：不关闭本次成功执行，等待能力恢复
// =============================================================================

#include "ccu/fota/ota/call_context.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {

// 单个 ECU 的已安装版本事实。installedVersion 来自冻结任务包清单的
// targetVersion（不来自本地猜测）。
struct InstalledVersionItem {
    std::string ecuId;
    std::string packageId;
    std::string packageRevision;
    std::string installedVersion;
};

// 一次 Execution 的安装结果登记请求。
struct InstalledVersionsReport {
    std::string vehicleTaskId;
    std::uint64_t taskRevision = 0;
    std::string executionId;
    std::uint32_t attemptNo = 0;
    // executionId + ':' + attemptNo（重试必须复用同一值）。
    std::string idempotencyKey;
    std::int64_t appliedAtMs = 0;
    std::vector<InstalledVersionItem> items;
};

// DIAG 登记接口的处置结果（由 CCU-DIAG-DSN-CR-010 冻结）。
enum class RegistrationDisposition {
    Applied,
    Duplicate,
    RejectedInvalid,  // 永久性拒绝：不重试
    RetryableFailed,  // 可重试失败
    Unavailable,      // 传输/能力不可用：等待恢复后按同一身份重试
};

// 处置结果的稳定字符串名（日志/指标用），inline 以避免额外 .cpp 依赖。
inline const char* registrationDispositionToString(RegistrationDisposition d) {
    switch (d) {
        case RegistrationDisposition::Applied:          return "APPLIED";
        case RegistrationDisposition::Duplicate:        return "DUPLICATE";
        case RegistrationDisposition::RejectedInvalid:  return "REJECTED_INVALID";
        case RegistrationDisposition::RetryableFailed:  return "RETRYABLE_FAILED";
        case RegistrationDisposition::Unavailable:      return "UNAVAILABLE";
    }
    return "UNKNOWN";
}

// 被 DIAG 逐项拒绝的条目（只含安全字段，不含版本原值之外的敏感信息）。
struct RejectedRegistrationItem {
    std::string ecuId;
    std::string reasonCode;
};

// 登记结果。accepted() 为真时 registrationId 必须非空才能收口。
struct RegistrationResult {
    RegistrationDisposition disposition = RegistrationDisposition::RetryableFailed;
    std::string registrationId;
    std::uint32_t appliedCount = 0;
    std::vector<RejectedRegistrationItem> rejectedItems;
    std::uint64_t effectiveRevision = 0;
    std::string effectiveDigestHex;
    // FOTA-INVENTORY-* 错误码与安全细节（不含 items 原值与 VIN）。
    std::string errorCode;
    std::string errorDetail;

    // 登记完成（可收口）。仍要求 registrationId 非空，调用方需再校验。
    bool completed() const {
        return disposition == RegistrationDisposition::Applied ||
               disposition == RegistrationDisposition::Duplicate;
    }
    bool retryable() const {
        return disposition == RegistrationDisposition::RetryableFailed ||
               disposition == RegistrationDisposition::Unavailable;
    }
    bool rejected() const {
        return disposition == RegistrationDisposition::RejectedInvalid;
    }
};

// 安装结果登记端口（FOTA 内部端口，非 wire 端口）。生产实现 =
// DiagInstalledVersionRegistrar；测试实现 = FakeInstalledVersionRegistrar。
class InstalledVersionRegistrar {
public:
    virtual ~InstalledVersionRegistrar() = default;

    // 登记能力是否可用（DIAG 可达 + Method ID 已分配 + 功能未禁用）。
    // 不可用时不得开始新的安装（CR-013 D7）。
    virtual bool available() = 0;

    // 幂等登记一次 Execution 的已安装版本。实现不得抛异常穿过边界：所有失败
    // 以 RegistrationResult 表达。
    virtual RegistrationResult registerInstalledVersions(
        const InstalledVersionsReport& report, CallContext& ctx) = 0;
};

} // namespace ota
} // namespace ccu_fota
