// =============================================================================
// src/ota/fota_digest.cpp
// OTA 业务摘要规范化实现 (CCU-FOTA-DSN-CR-015 §D4)
// =============================================================================

#include "ccu/fota/ota/fota_digest.hpp"

#include "ccu/fota/fingerprint/canonical_encoder.hpp"

#include "ccu/fw/hash/sha256.hpp"

#include <algorithm>
#include <cstring>
#include <tuple>
#include <vector>

namespace ccu_fota {
namespace ota {
namespace digest {

namespace {

using ::vehicle::fota::v1::ConditionSnapshot;
using ::vehicle::fota::v1::ConditionValue;
using ::vehicle::fota::v1::EcuResult;
using ::vehicle::fota::v1::ErrorSummary;
using ::vehicle::fota::v1::ExecutionEvent;
using ::vehicle::fota::v1::FinalResultReport;
using ::vehicle::fota::v1::PackageSummary;
using ::vehicle::fota::v1::StageResultReport;
using ::vehicle::fota::v1::VehicleTaskSnapshot;

using fingerprint::CanonicalEncoder;

std::string hexOf(std::vector<std::uint8_t> bytes) {
    return ccu::fw::hash::sha256_hex(ccu::fw::hash::ByteView{bytes.data(), bytes.size()});
}

std::uint32_t enumCode(std::int64_t value) {
    // proto3 枚举是 int32 的有符号表示；规范化为稳定的 u32be。
    return static_cast<std::uint32_t>(value);
}

// 可选 presence + value 编码：missing 与 present(false/0) 必须产生不同字节。
void writeOptionalU32(CanonicalEncoder& e, std::uint16_t presenceField,
                      std::uint16_t valueField, bool present, std::uint32_t value) {
    e.writeEnumField(presenceField, present ? 1u : 0u);
    if (present) e.writeEnumField(valueField, value);
}

bool hasMessageDigest(const ::vehicle::fota::v1::Digest& d) {
    return !d.value_hex().empty() || !d.algorithm().empty();
}

// ---------------------------------------------------------------------------
// PackageSummary 子记录
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> encodePackage(const PackageSummary& p) {
    CanonicalEncoder e;
    e.writeStringField(1, p.package_id());
    e.writeStringField(2, p.package_revision());
    e.writeStringField(3, p.ecu_id());
    e.writeStringField(4, p.plan_node_id());
    e.writeOptionalStringField(
        5, p.has_component_id() ? std::optional<std::string>(p.component_id())
                                : std::nullopt);
    e.writeStringField(6, p.source_version());
    e.writeStringField(7, p.target_version());
    e.writeUint64Field(8, p.size_bytes());
    e.writeStringField(9, p.package_digest().algorithm());
    e.writeStringField(10, p.package_digest().value_hex());
    e.writeStringField(11, p.differential_type());
    e.writeEnumField(12, p.encrypted() ? 1u : 0u);
    return e.finalize();
}

// 稳定排序键（tie-break 到全字段，避免依赖输入顺序）。
std::tuple<std::string, std::string, std::string, std::string, std::string,
           std::string, std::string, std::uint64_t>
packageOrderKey(const PackageSummary& p) {
    return std::make_tuple(p.ecu_id(), p.package_id(), p.package_revision(),
                           p.plan_node_id(), p.source_version(), p.target_version(),
                           p.package_digest().value_hex(), p.size_bytes());
}

// ---------------------------------------------------------------------------
// EcuResult 子记录（最终结果）
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> encodeEcuResult(const EcuResult& r) {
    CanonicalEncoder e;
    e.writeStringField(1, r.ecu_id());
    e.writeStringField(2, r.source_version());
    e.writeStringField(3, r.target_version());
    e.writeStringField(4, r.plan_node_id());
    e.writeStringField(5, r.package_id());
    e.writeOptionalStringField(
        6, r.has_actual_version() ? std::optional<std::string>(r.actual_version())
                                  : std::nullopt);
    e.writeStringField(7, r.version_read_status());
    e.writeEnumField(8, enumCode(r.result()));
    e.writeOptionalStringField(
        9, r.has_failure_stage() ? std::optional<std::string>(r.failure_stage())
                                 : std::nullopt);
    writeOptionalU32(e, 10, 11, r.has_rollback_result(),
                     enumCode(r.rollback_result()));
    return e.finalize();
}

std::tuple<std::string, std::string, std::string, std::string>
ecuResultOrderKey(const EcuResult& r) {
    return std::make_tuple(r.ecu_id(), r.package_id(), r.plan_node_id(),
                           r.target_version());
}

// ---------------------------------------------------------------------------
// ErrorSummary 子记录
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> encodeErrorSummary(const ErrorSummary& s) {
    CanonicalEncoder e;
    e.writeStringField(1, s.ecu_id());
    e.writeStringField(2, s.stage());
    e.writeStringField(3, s.error_code());
    return e.finalize();
}

std::tuple<std::string, std::string, std::string> errorOrderKey(const ErrorSummary& s) {
    return std::make_tuple(s.ecu_id(), s.stage(), s.error_code());
}

// ---------------------------------------------------------------------------
// 条件扩展项子记录
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> encodeCondition(const ConditionValue& c) {
    CanonicalEncoder e;
    e.writeStringField(1, c.code());
    e.writeStringField(2, c.value());
    e.writeStringField(3, c.unit());
    e.writeStringField(4, c.quality());
    return e.finalize();
}

std::tuple<std::string, std::string> conditionOrderKey(const ConditionValue& c) {
    return std::make_tuple(c.code(), c.value());
}

} // namespace

std::string packageManifestDigestHex(const VehicleTaskSnapshot& task) {
    std::vector<const PackageSummary*> pkgs;
    pkgs.reserve(static_cast<std::size_t>(task.package_list_size()));
    for (const auto& p : task.package_list()) pkgs.push_back(&p);
    std::sort(pkgs.begin(), pkgs.end(),
              [](const PackageSummary* a, const PackageSummary* b) {
                  return packageOrderKey(*a) < packageOrderKey(*b);
              });

    std::vector<std::vector<std::uint8_t>> items;
    items.reserve(pkgs.size());
    for (const auto* p : pkgs) items.push_back(encodePackage(*p));

    CanonicalEncoder e(kPackageManifestDomain);
    e.writeListField(1, items);
    return hexOf(e.finalize());
}

std::string stageResultDigestHex(const StageResultReport& s) {
    CanonicalEncoder e(kStageResultDomain);
    e.writeStringField(1, s.package_id());
    e.writeStringField(2, s.verified_package_revision());
    e.writeEnumField(3, enumCode(s.result()));
    e.writeUint64Field(4, s.downloaded_bytes());
    const bool hasActual = hasMessageDigest(s.actual_package_digest());
    e.writeOptionalStringField(
        5, hasActual ? std::optional<std::string>(s.actual_package_digest().algorithm())
                     : std::nullopt);
    e.writeOptionalStringField(
        6, hasActual ? std::optional<std::string>(s.actual_package_digest().value_hex())
                     : std::nullopt);
    e.writeEnumField(7, s.hash_verified() ? 1u : 0u);
    e.writeEnumField(8, s.signature_verified() ? 1u : 0u);
    writeOptionalU32(e, 9, 10, s.has_decryption_succeeded(),
                     s.decryption_succeeded() ? 1u : 0u);
    e.writeOptionalStringField(
        11, s.has_error_code() ? std::optional<std::string>(s.error_code())
                               : std::nullopt);
    return hexOf(e.finalize());
}

std::string executionEventDigestHex(const ExecutionEvent& evt) {
    CanonicalEncoder e(kExecutionEventDomain);
    e.writeStringField(1, evt.event_id());
    e.writeEnumField(2, enumCode(evt.attempt_no()));
    e.writeOptionalStringField(
        3, evt.has_plan_node_id() ? std::optional<std::string>(evt.plan_node_id())
                                  : std::nullopt);
    e.writeOptionalStringField(
        4, evt.has_ecu_job_id() ? std::optional<std::string>(evt.ecu_job_id())
                                : std::nullopt);
    e.writeOptionalStringField(
        5, evt.has_package_id() ? std::optional<std::string>(evt.package_id())
                                : std::nullopt);
    e.writeOptionalStringField(
        6, evt.has_component_id() ? std::optional<std::string>(evt.component_id())
                                  : std::nullopt);
    writeOptionalU32(e, 7, 8, evt.has_node_attempt_no(), evt.node_attempt_no());
    e.writeStringField(9, evt.stage());
    e.writeStringField(10, evt.event_status());
    writeOptionalU32(e, 11, 12, evt.has_progress(), evt.progress());
    e.writeOptionalStringField(
        13, evt.has_progress_scope() ? std::optional<std::string>(evt.progress_scope())
                                     : std::nullopt);
    e.writeOptionalStringField(
        14, evt.has_ecu_id() ? std::optional<std::string>(evt.ecu_id()) : std::nullopt);
    e.writeOptionalStringField(
        15, evt.has_current_version()
                ? std::optional<std::string>(evt.current_version())
                : std::nullopt);
    e.writeOptionalStringField(
        16, evt.has_target_version() ? std::optional<std::string>(evt.target_version())
                                     : std::nullopt);
    e.writeOptionalStringField(
        17, evt.has_error_code() ? std::optional<std::string>(evt.error_code())
                                 : std::nullopt);
    e.writeOptionalStringField(
        18, evt.has_error_detail() ? std::optional<std::string>(evt.error_detail())
                                   : std::nullopt);
    return hexOf(e.finalize());
}

std::string finalResultDigestHex(const FinalResultReport& r) {
    std::vector<const EcuResult*> ecus;
    ecus.reserve(static_cast<std::size_t>(r.ecu_results_size()));
    for (const auto& er : r.ecu_results()) ecus.push_back(&er);
    std::sort(ecus.begin(), ecus.end(), [](const EcuResult* a, const EcuResult* b) {
        return ecuResultOrderKey(*a) < ecuResultOrderKey(*b);
    });
    std::vector<std::vector<std::uint8_t>> ecuItems;
    ecuItems.reserve(ecus.size());
    for (const auto* er : ecus) ecuItems.push_back(encodeEcuResult(*er));

    std::vector<const ErrorSummary*> errors;
    errors.reserve(static_cast<std::size_t>(r.error_summary_size()));
    for (const auto& es : r.error_summary()) errors.push_back(&es);
    std::sort(errors.begin(), errors.end(),
              [](const ErrorSummary* a, const ErrorSummary* b) {
                  return errorOrderKey(*a) < errorOrderKey(*b);
              });
    std::vector<std::vector<std::uint8_t>> errorItems;
    errorItems.reserve(errors.size());
    for (const auto* es : errors) errorItems.push_back(encodeErrorSummary(*es));

    CanonicalEncoder e(kFinalResultDomain);
    e.writeEnumField(1, enumCode(r.result()));
    e.writeOptionalStringField(
        2, r.has_actual_baseline_code()
               ? std::optional<std::string>(r.actual_baseline_code())
               : std::nullopt);
    e.writeStringField(3, r.baseline_status());
    e.writeEnumField(4, r.rollback_occurred() ? 1u : 0u);
    e.writeListField(5, ecuItems);
    e.writeListField(6, errorItems);
    return hexOf(e.finalize());
}

std::string localReadinessDigestHex(std::string_view conditionSetVersion,
                                    const ConditionSnapshot& s) {
    std::vector<const ConditionValue*> conds;
    conds.reserve(static_cast<std::size_t>(s.extended_conditions_size()));
    for (const auto& c : s.extended_conditions()) conds.push_back(&c);
    std::sort(conds.begin(), conds.end(),
              [](const ConditionValue* a, const ConditionValue* b) {
                  return conditionOrderKey(*a) < conditionOrderKey(*b);
              });
    std::vector<std::vector<std::uint8_t>> items;
    items.reserve(conds.size());
    for (const auto* c : conds) items.push_back(encodeCondition(*c));

    CanonicalEncoder e(kLocalReadinessDomain);
    e.writeStringField(1, conditionSetVersion);
    e.writeEnumField(2, s.traction_battery_soc());
    e.writeDoubleField(3, s.low_voltage_battery_voltage_v());
    e.writeStringField(4, s.ignition_state());
    e.writeStringField(5, s.gear());
    e.writeDoubleField(6, s.speed_kph());
    e.writeOptionalStringField(
        7, s.has_charging_state() ? std::optional<std::string>(s.charging_state())
                                  : std::nullopt);
    e.writeStringField(8, s.network_type());
    e.writeUint64Field(9, s.available_storage_bytes());
    e.writeListField(10, items);
    return hexOf(e.finalize());
}

} // namespace digest
} // namespace ota
} // namespace ccu_fota
