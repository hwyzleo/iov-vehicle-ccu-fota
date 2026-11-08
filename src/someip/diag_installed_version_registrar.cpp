// =============================================================================
// src/someip/diag_installed_version_registrar.cpp
// CCU-DIAG 安装结果登记适配器实现 (CCU-FOTA-DSN-CR-013 §2 / US-037)
// =============================================================================
// 失败分类（决定 FOTA 是重试还是收口）：
//   * 传输失败/超时/撤销/无响应  -> RetryableFailed（DIAG 幂等，可安全重试）
//   * payload 编解码失败         -> RetryableFailed（不猜测内容）
//   * 响应缺 registrationId      -> RetryableFailed（收口必要条件）
//   * REJECTED_INVALID           -> RejectedInvalid（ERROR 事件后收口，不重试）
//   * 能力未分配/禁用/服务不可用 -> Unavailable（不调用；不关闭成功执行）
// 日志只记 executionId/attemptNo/appliedCount/错误码，不输出 items 原值与 VIN。
// =============================================================================

#include "ccu/fota/someip/diag_installed_version_registrar.hpp"

#include "ccu/fota/ota/inventory_error_codes.hpp"
#include "ccu/fota/someip/error_mapper.hpp"
#include "fota_log_adapter.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <exception>
#include <string>
#include <vector>

namespace ccu_fota {
namespace someip {

using ccu::fw::someip::Availability;
using ccu::fw::someip::CallOptions;
using ccu::fw::someip::CallResult;
using ccu::fw::someip::PayloadView;
using ccu::fw::someip::RetryMode;
namespace ota = ccu_fota::ota;

namespace {

using json = nlohmann::json;

// 冻结 payload 版本（与清单接口一致；wire 上以整数声明）。
constexpr int kSchemaVersionWire = 1;
constexpr const char* kSchemaVersion = "1";

std::string normalizeEnum(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 4);
    bool prevUnderscore = false;
    for (char c : raw) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::toupper(uc)));
            prevUnderscore = false;
        } else if (!out.empty() && !prevUnderscore) {
            out.push_back('_');
            prevUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

const json* findField(const json& j, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = j.find(k);
        if (it != j.end() && !it->is_null()) return &(*it);
    }
    return nullptr;
}

ota::RegistrationResult failed(ota::RegistrationDisposition disposition,
                               const char* code, std::string detail) {
    ota::RegistrationResult r;
    r.disposition = disposition;
    r.errorCode = code;
    r.errorDetail = std::move(detail);
    return r;
}

} // namespace

DiagInstalledVersionRegistrar::DiagInstalledVersionRegistrar(
    std::shared_ptr<ccu::fw::someip::Client> client,
    DiagInstalledVersionRegistrarConfig cfg)
    : client_(std::move(client)), cfg_(cfg) {}

bool DiagInstalledVersionRegistrar::available() {
    if (!cfg_.enabled) return false;
    if (cfg_.methodId == 0) return false;
    if (!client_) return false;
    return client_->availability() == Availability::Available;
}

std::string DiagInstalledVersionRegistrar::encodeRequest(
    const ota::InstalledVersionsReport& report) {
    json items = json::array();
    for (const auto& it : report.items) {
        items.push_back(json{{"ecuId", it.ecuId},
                             {"packageId", it.packageId},
                             {"packageRevision", it.packageRevision},
                             {"installedVersion", it.installedVersion}});
    }
    json j{{"schemaVersion", kSchemaVersionWire},
           {"vehicleTaskId", report.vehicleTaskId},
           {"taskRevision", report.taskRevision},
           {"executionId", report.executionId},
           {"attemptNo", report.attemptNo},
           {"idempotencyKey", report.idempotencyKey},
           {"appliedAtMs", report.appliedAtMs},
           {"items", std::move(items)}};
    return j.dump();
}

ota::RegistrationResult DiagInstalledVersionRegistrar::decodeResponse(
    std::string_view payload) {
    if (payload.empty()) {
        return failed(ota::RegistrationDisposition::RetryableFailed,
                      ota::inventory_error::kRegistrationFailed,
                      "empty response payload");
    }

    json j;
    try {
        j = json::parse(payload.begin(), payload.end());
    } catch (const json::exception& e) {
        return failed(ota::RegistrationDisposition::RetryableFailed,
                      ota::inventory_error::kRegistrationFailed,
                      std::string("json parse failed: ") + e.what());
    }
    if (!j.is_object()) {
        return failed(ota::RegistrationDisposition::RetryableFailed,
                      ota::inventory_error::kRegistrationFailed,
                      "response is not a json object");
    }

    const json* schema = findField(j, {"schemaVersion", "schema_version"});
    if (schema != nullptr) {
        std::string ver;
        if (schema->is_string()) {
            ver = schema->get<std::string>();
        } else if (schema->is_number_integer()) {
            ver = std::to_string(schema->get<long long>());
        }
        if (!ver.empty() && ver != kSchemaVersion) {
            return failed(ota::RegistrationDisposition::RetryableFailed,
                          ota::inventory_error::kRegistrationFailed,
                          "unsupported response schemaVersion: " + ver);
        }
    }

    const json* disposition = findField(j, {"disposition"});
    if (disposition == nullptr || !disposition->is_string()) {
        return failed(ota::RegistrationDisposition::RetryableFailed,
                      ota::inventory_error::kRegistrationFailed,
                      "missing field: disposition");
    }
    const std::string d = normalizeEnum(disposition->get<std::string>());

    ota::RegistrationResult r;
    if (const json* reg = findField(j, {"registrationId", "registration_id"})) {
        if (reg->is_string()) r.registrationId = reg->get<std::string>();
    }
    if (const json* cnt = findField(j, {"appliedCount", "applied_count"})) {
        if (cnt->is_number_unsigned() || cnt->is_number_integer()) {
            r.appliedCount = cnt->get<std::uint32_t>();
        }
    }
    if (const json* rev = findField(j, {"effectiveRevision", "effective_revision"})) {
        if (rev->is_number_unsigned() || rev->is_number_integer()) {
            r.effectiveRevision = rev->get<std::uint64_t>();
        }
    }
    if (const json* dig = findField(j, {"effectiveDigestHex", "effective_digest_hex"})) {
        if (dig->is_string()) r.effectiveDigestHex = dig->get<std::string>();
    }

    if (const json* rejected = findField(j, {"rejectedItems", "rejected_items"})) {
        if (rejected->is_array()) {
            for (const auto& item : *rejected) {
                ota::RejectedRegistrationItem out;
                if (item.is_string()) {
                    out.reasonCode = item.get<std::string>();
                } else if (item.is_object()) {
                    if (const json* id = findField(item, {"ecuId", "ecu_id"})) {
                        if (id->is_string()) out.ecuId = id->get<std::string>();
                    }
                    if (const json* rc = findField(
                            item, {"reasonCode", "reason_code", "reason", "errorCode",
                                   "error_code"})) {
                        if (rc->is_string()) out.reasonCode = rc->get<std::string>();
                    }
                } else {
                    out.reasonCode = "UNSPECIFIED_REJECTION";
                }
                r.rejectedItems.push_back(std::move(out));
            }
        }
    }

    if (d == "APPLIED") {
        r.disposition = ota::RegistrationDisposition::Applied;
    } else if (d == "DUPLICATE") {
        r.disposition = ota::RegistrationDisposition::Duplicate;
    } else if (d == "REJECTED_INVALID") {
        r.disposition = ota::RegistrationDisposition::RejectedInvalid;
        r.errorCode = ota::inventory_error::kRegistrationRejected;
        r.errorDetail = "DIAG rejected the registration as invalid";
    } else if (d == "RETRYABLE_FAILED") {
        r.disposition = ota::RegistrationDisposition::RetryableFailed;
        r.errorCode = ota::inventory_error::kRegistrationFailed;
        r.errorDetail = "DIAG reported retryable failure";
    } else {
        // 未知处置不得当作成功：按可重试失败处理（DIAG 幂等保证重试安全）。
        return failed(ota::RegistrationDisposition::RetryableFailed,
                      ota::inventory_error::kRegistrationFailed,
                      "unknown disposition: " +
                          disposition->get<std::string>());
    }

    // 收口必要条件：APPLIED/DUPLICATE 必须带 registrationId。
    if (r.completed() && r.registrationId.empty()) {
        return failed(ota::RegistrationDisposition::RetryableFailed,
                      ota::inventory_error::kRegistrationFailed,
                      "response missing registrationId");
    }
    return r;
}

ota::RegistrationResult DiagInstalledVersionRegistrar::registerInstalledVersions(
    const ota::InstalledVersionsReport& report, ota::CallContext& ctx) {
    if (!cfg_.enabled) {
        return failed(ota::RegistrationDisposition::Unavailable,
                      ota::inventory_error::kRegistrationUnavailable,
                      "installed version registration disabled");
    }
    if (cfg_.methodId == 0) {
        // Registry 未分配登记 Method ID：能力关闭，禁止猜测 ID（D7）。
        return failed(ota::RegistrationDisposition::Unavailable,
                      ota::inventory_error::kRegistrationIdUnallocated,
                      "registration method id not allocated by Registry");
    }
    if (!client_ || client_->availability() != Availability::Available) {
        return failed(ota::RegistrationDisposition::Unavailable,
                      ota::inventory_error::kRegistrationUnavailable,
                      "DIAG service unavailable");
    }

    std::string payload;
    try {
        payload = encodeRequest(report);
    } catch (const std::exception& e) {
        return failed(ota::RegistrationDisposition::RetryableFailed,
                      ota::inventory_error::kRegistrationFailed,
                      std::string("encode failed: ") + e.what());
    }

    CallOptions opts;
    opts.timeout = cfg_.timeout.count() > 0 ? cfg_.timeout : ctx.timeout;
    opts.retry = RetryMode::None;  // 幂等由业务身份保证，framework 不自动重试

    FotaLogAdapter::diag_client().info(
        fota_events::DIAG_REGISTRATION_REQUESTED,
        "Installed version registration requested",
        {flog::f_str("execution_id", report.executionId),
         flog::f_int("attempt_no", static_cast<int64_t>(report.attemptNo)),
         flog::f_int("item_count",
                     static_cast<int64_t>(report.items.size())),
         flog::f_str("idempotency_key", report.idempotencyKey)});

    const PayloadView view{reinterpret_cast<const std::uint8_t*>(payload.data()),
                           payload.size()};
    CallResult result = client_->call(cfg_.methodId, view, opts);
    if (!result.success) {
        MappedError me = mapDiagCallError(result);
        FotaLogAdapter::diag_client().warn(
            fota_events::DIAG_REGISTRATION_REJECTED,
            "Installed version registration call failed",
            {flog::f_str("execution_id", report.executionId),
             flog::f_int("attempt_no", static_cast<int64_t>(report.attemptNo)),
             flog::f_str("disposition",
                         registrationDispositionToString(
                             ota::RegistrationDisposition::RetryableFailed)),
             flog::f_str("framework_error", result.code),
             flog::f_str("error_code", errorCodeToString(me.code))});
        return failed(ota::RegistrationDisposition::RetryableFailed,
                      ota::inventory_error::kRegistrationFailed,
                      "framework call failed: " + result.code);
    }

    auto decoded = decodeResponse(
        std::string_view(reinterpret_cast<const char*>(result.response.data()),
                         result.response.size()));

    if (decoded.disposition == ota::RegistrationDisposition::Applied) {
        FotaLogAdapter::diag_client().info(
            fota_events::DIAG_REGISTRATION_ACCEPTED,
            "Installed versions registered",
            {flog::f_str("execution_id", report.executionId),
             flog::f_int("attempt_no", static_cast<int64_t>(report.attemptNo)),
             flog::f_int("applied_count",
                         static_cast<int64_t>(decoded.appliedCount))});
    } else if (decoded.disposition == ota::RegistrationDisposition::Duplicate) {
        FotaLogAdapter::diag_client().info(
            fota_events::DIAG_REGISTRATION_DUPLICATE,
            "Installed versions already registered (idempotent replay)",
            {flog::f_str("execution_id", report.executionId),
             flog::f_int("attempt_no", static_cast<int64_t>(report.attemptNo))});
    } else if (decoded.disposition == ota::RegistrationDisposition::RejectedInvalid) {
        std::string reasons;
        for (const auto& it : decoded.rejectedItems) {
            if (!reasons.empty()) reasons += ",";
            reasons += it.ecuId + ":" + it.reasonCode;
        }
        FotaLogAdapter::diag_client().error(
            fota_events::DIAG_REGISTRATION_REJECTED,
            "Installed version registration permanently rejected",
            {flog::f_str("execution_id", report.executionId),
             flog::f_int("attempt_no", static_cast<int64_t>(report.attemptNo)),
             flog::f_str("disposition",
                         registrationDispositionToString(decoded.disposition)),
             flog::f_str("error_code", decoded.errorCode),
             flog::f_str("rejected", reasons)});
    } else {
        FotaLogAdapter::diag_client().warn(
            fota_events::DIAG_REGISTRATION_REJECTED,
            "Installed version registration failed, will retry with same identity",
            {flog::f_str("execution_id", report.executionId),
             flog::f_int("attempt_no", static_cast<int64_t>(report.attemptNo)),
             flog::f_str("disposition",
                         registrationDispositionToString(decoded.disposition)),
             flog::f_str("error_code", decoded.errorCode),
             flog::f_str("reason", decoded.errorDetail)});
    }
    return decoded;
}

} // namespace someip
} // namespace ccu_fota
