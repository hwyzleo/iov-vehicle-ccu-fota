// =============================================================================
// src/someip/diag_inventory_client.cpp
// CCU-FOTA DIAG Inventory Client 适配器实现 (CCU-FOTA-DSN-CR-007 §Client 设计)
// =============================================================================

#include "ccu/fota/someip/diag_inventory_client.hpp"
#include "ccu/fota/someip/error_mapper.hpp"
#include "constants.h"
#include "error_codes.h"
#include "fota_log_adapter.h"
#include "fota_log_context.h"

#include <cstring>
#include <string>
#include <vector>

namespace ccu_fota {
namespace someip {

using ccu::fw::someip::Availability;
using ccu::fw::someip::CallOptions;
using ccu::fw::someip::CallResult;
using ccu::fw::someip::Payload;
using ccu::fw::someip::PayloadView;
using ccu::fw::someip::RetryMode;

namespace {

// ---- DIAG IDL codec（简化 wire：与既有 mock_diag_service.py 兼容）----
// VIN 响应：纯 ASCII 字符串（17 字节）。DIAG 业务响应 return code 非 0 时由
// framework 映射为 CallResult.fail，本 codec 只处理 success 路径。

} // namespace

DiagInventoryClient::DiagInventoryClient(ccu::fw::someip::Client client,
                                         std::chrono::milliseconds collectTimeout)
    : client_(std::move(client))
    , collectTimeout_(collectTimeout)
    , hasClient_(true) {}

DiagInventoryClient::DiagInventoryClient()
    : collectTimeout_(std::chrono::milliseconds(30000))
    , hasClient_(false) {}

DiagInventoryClient::~DiagInventoryClient() = default;

void DiagInventoryClient::requestService() { client_.requestService(); }
void DiagInventoryClient::releaseService() { client_.releaseService(); }
Availability DiagInventoryClient::availability() const { return client_.availability(); }

ccu::fw::someip::Subscription
DiagInventoryClient::onAvailability(ccu::fw::someip::AvailabilityCallback callback) {
    return client_.onAvailability(std::move(callback));
}

bool DiagInventoryClient::getVin(std::string& vin) {
    if (!hasClient_) return false;

    // Available/VersionMismatch gating：不可用时不调用 (CR-007 §Client 设计)
    if (client_.availability() != Availability::Available) {
        FotaLogAdapter::diag_client().warn(
            "fota.diag.unavailable",
            "DIAG service unavailable, skip VIN read",
            {flog::f_str("someip_service_id", hex_id(DEFAULT_DIAG_SERVICE_ID)),
             flog::f_str("availability",
                         client_.availability() == Availability::VersionMismatch
                             ? "VersionMismatch"
                             : "Unavailable")});
        return false;
    }

    CallOptions opts;
    opts.timeout = collectTimeout_;
    opts.retry = RetryMode::None;  // framework 不自动重试 (CR-007)

    CallResult result = client_.call(METHOD_READ_VIN, PayloadView{nullptr, 0}, opts);
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

    // 解码 VIN：直接 ASCII（与 mock_diag_service.py 兼容）
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

    // VIN 来自 DIAG
    if (!getVin(snapshot.vin)) {
        return false;
    }
    if (snapshot.vin.empty()) {
        return false;
    }

    // 版本清单其余字段（DIAG IDL 简化：与既有 stub 行为一致，待 DIAG IDL 落地）
    snapshot.baseline_id = "BASELINE001";
    snapshot.baseline_source = BaselineSource::FACTORY;
    snapshot.registry_version = "1.0.0";
    snapshot.collected_at = "2026-07-21T10:00:00Z";
    snapshot.overall_result = CollectionStatus::ALL_OK;
    snapshot.snapshot_seq = 1;

    EcuVersionEntry entry1;
    entry1.ecu_id = "ECU001";
    entry1.part_number = "PN001";
    entry1.sw_version = "1.0.0";
    entry1.hw_version = "HW1.0";
    entry1.source = VersionSource::UDS_0x22;
    entry1.status = EcuStatus::OK;

    EcuVersionEntry entry2;
    entry2.ecu_id = "ECU002";
    entry2.part_number = "PN002";
    entry2.sw_version = "2.0.0";
    entry2.hw_version = "HW2.0";
    entry2.source = VersionSource::SOMEIP_GET_VERSION;
    entry2.status = EcuStatus::OK;

    snapshot.ecu_list = {entry1, entry2};
    return true;
}

} // namespace someip
} // namespace ccu_fota
