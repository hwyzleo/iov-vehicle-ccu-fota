// =============================================================================
// src/someip/diag_inventory_client.cpp
// CCU-FOTA DIAG Inventory Client 适配器实现
// (CCU-FOTA-DSN-CR-007 §Client 设计 / CCU-FOTA-DSN-CR-013 §1 真实读取)
// =============================================================================
// 清单内容唯一来源是 CCU-DIAG（CR-013 D1/D2）：本适配器只做 IDL 解码与 DTO 映射，
// 不持有、不生成、不补全任何 ECU 清单内容（源码中不得出现 ECU 标识/件号/版本常量）。
//
// 调用语义：
//   * METHOD_COLLECT_VEHICLE_INVENTORY(0x0002) 单次 framework call，retry=None；
//   * availability 非 Available 时不发起调用（ReturnCode/版本不匹配由 framework
//     在 call 中判定，本层只做有界短路并记录）；
//   * payload 由 DiagInventoryCodec 按 schemaVersion 解码；失败即 fail-closed
//     （collected=false，不上报、不分配 inventoryRevision）。
// =============================================================================

#include "ccu/fota/someip/diag_inventory_client.hpp"

#include "ccu/fota/someip/diag_inventory_codec.hpp"
#include "ccu/fota/someip/error_mapper.hpp"
#include "constants.h"
#include "error_codes.h"
#include "fota_log_adapter.h"
#include "fota_log_context.h"

#include <string>
#include <string_view>

namespace ccu_fota {
namespace someip {

using ccu::fw::someip::Availability;
using ccu::fw::someip::CallOptions;
using ccu::fw::someip::CallResult;
using ccu::fw::someip::PayloadView;
using ccu::fw::someip::RetryMode;

namespace {

// framework availability -> 稳定日志标签。
const char* availabilityLabel(Availability a) {
    switch (a) {
        case Availability::Available:       return "Available";
        case Availability::Unavailable:     return "Unavailable";
        case Availability::VersionMismatch: return "VersionMismatch";
        case Availability::Stopping:        return "Stopping";
        default:                            return "Unknown";
    }
}

std::string_view payloadView(const ccu::fw::someip::Payload& payload) {
    return std::string_view(reinterpret_cast<const char*>(payload.data()),
                            payload.size());
}

} // namespace

DiagInventoryClient::DiagInventoryClient(
    std::shared_ptr<ccu::fw::someip::Client> client,
    std::chrono::milliseconds collectTimeout)
    : client_(std::move(client))
    , collectTimeout_(collectTimeout)
    , hasClient_(client_ != nullptr) {}

DiagInventoryClient::DiagInventoryClient()
    : collectTimeout_(std::chrono::milliseconds(30000))
    , hasClient_(false) {}

DiagInventoryClient::~DiagInventoryClient() = default;

void DiagInventoryClient::requestService() {
    if (client_) client_->requestService();
}
void DiagInventoryClient::releaseService() {
    if (client_) client_->releaseService();
}
Availability DiagInventoryClient::availability() const {
    return client_ ? client_->availability() : Availability::Unavailable;
}

ccu::fw::someip::Subscription
DiagInventoryClient::onAvailability(ccu::fw::someip::AvailabilityCallback callback) {
    return client_->onAvailability(std::move(callback));
}

bool DiagInventoryClient::getVin(std::string& vin) {
    if (!hasClient_) return false;

    // Available/VersionMismatch gating：不可用时不调用 (CR-007 §Client 设计)
    if (client_->availability() != Availability::Available) {
        FotaLogAdapter::diag_client().warn(
            "fota.diag.unavailable",
            "DIAG service unavailable, skip VIN read",
            {flog::f_str("someip_service_id", hex_id(DEFAULT_DIAG_SERVICE_ID)),
             flog::f_str("availability", availabilityLabel(client_->availability()))});
        return false;
    }

    CallOptions opts;
    opts.timeout = collectTimeout_;
    opts.retry = RetryMode::None;  // framework 不自动重试 (CR-007)

    CallResult result = client_->call(METHOD_READ_VIN, PayloadView{nullptr, 0}, opts);
    if (!result.success) {
        MappedError me = mapDiagCallError(result);
        FotaLogAdapter::diag_client().warn(
            "fota.diag.get_vin_failed",
            "DIAG read VIN failed",
            {flog::f_str("someip_service_id", hex_id(DEFAULT_DIAG_SERVICE_ID)),
             flog::f_str("someip_method_id", hex_id(METHOD_READ_VIN)),
             flog::f_str("framework_error", result.code),
             flog::f_str("error_code", errorCodeToString(me.code))});
        return false;
    }

    // 解码 VIN：DIAG READ_VIN 响应为纯 ASCII（17 字节）。
    if (result.response.empty()) {
        return false;
    }
    vin.assign(reinterpret_cast<const char*>(result.response.data()),
               result.response.size());
    if (vin.length() != 17) {
        FotaLogAdapter::diag_client().warn(
            "fota.diag.invalid_vin_length",
            "Invalid VIN length",
            {flog::f_int("actual_length", static_cast<int64_t>(vin.length())),
             flog::f_int("expected_length", 17)});
        return false;
    }
    return true;
}

bool DiagInventoryClient::collectVehicleInventory(VehicleSoftwareSnapshot& snapshot) {
    if (!hasClient_) return false;

    // 建立 DIAG child span (traceId + requestId + reportId + peer + method)
    auto diag_scope = make_someip_child_scope(
        hex_id(DEFAULT_DIAG_SERVICE_ID),
        hex_id(METHOD_COLLECT_VEHICLE_INVENTORY));

    if (client_->availability() != Availability::Available) {
        FotaLogAdapter::diag_client().warn(
            "fota.diag.unavailable",
            "DIAG service unavailable, skip inventory collect",
            {flog::f_str("someip_service_id", hex_id(DEFAULT_DIAG_SERVICE_ID)),
             flog::f_str("someip_method_id",
                         hex_id(METHOD_COLLECT_VEHICLE_INVENTORY)),
             flog::f_str("availability", availabilityLabel(client_->availability()))});
        return false;
    }

    CallOptions opts;
    opts.timeout = collectTimeout_;
    opts.retry = RetryMode::None;  // framework 不自动重试 (CR-007)

    CallResult result = client_->call(METHOD_COLLECT_VEHICLE_INVENTORY,
                                     PayloadView{nullptr, 0}, opts);
    if (!result.success) {
        MappedError me = mapDiagCallError(result);
        FotaLogAdapter::diag_client().warn(
            fota_events::DIAG_COLLECT_FAILED,
            "DIAG collectVehicleInventory call failed",
            {flog::f_str("someip_service_id", hex_id(DEFAULT_DIAG_SERVICE_ID)),
             flog::f_str("someip_method_id",
                         hex_id(METHOD_COLLECT_VEHICLE_INVENTORY)),
             flog::f_str("framework_error", result.code),
             flog::f_str("error_code", errorCodeToString(me.code))});
        return false;
    }

    // wire 形态解码（未知 schemaVersion / 缺字段 / 空清单 -> fail-closed）。
    DiagInventoryDecodeResult decoded =
        DiagInventoryCodec::decode(payloadView(result.response));
    if (!decoded.ok) {
        FotaLogAdapter::diag_client().error(
            fota_events::DIAG_INVENTORY_DECODE_FAILED,
            "DIAG inventory payload rejected, uplink skipped",
            {flog::f_str("someip_service_id", hex_id(DEFAULT_DIAG_SERVICE_ID)),
             flog::f_str("someip_method_id",
                         hex_id(METHOD_COLLECT_VEHICLE_INVENTORY)),
             flog::f_str("error_code", decoded.errorCode),
             flog::f_str("reason", decoded.errorDetail)});
        return false;
    }

    snapshot = std::move(decoded.snapshot);

    // 只记录数量/来源/digest 相关的安全计数，不输出清单原值与 VIN。
    FotaLogAdapter::diag_client().info(
        fota_events::DIAG_INVENTORY_DECODED,
        "DIAG inventory payload decoded",
        {flog::f_str("schema_version", snapshot.schema_version),
         flog::f_str("inventory_source",
                     inventorySourceToString(snapshot.inventory_source)),
         flog::f_int("ecu_count",
                     static_cast<int64_t>(snapshot.ecu_list.size())),
         flog::f_str("overall_result",
                     collectionStatusToString(snapshot.overall_result))});
    return true;
}

} // namespace someip
} // namespace ccu_fota
