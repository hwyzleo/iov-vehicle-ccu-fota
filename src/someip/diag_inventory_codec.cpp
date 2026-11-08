// =============================================================================
// src/someip/diag_inventory_codec.cpp
// CCU-DIAG 清单 payload 解码实现 (CCU-FOTA-DSN-CR-013 §1 / US-036)
// =============================================================================
// 解码是“全有或全无”：任一必填字段缺失/类型不符/枚举越界/清单为空都视为失败，
// 绝不返回“成功 + 部分清单”或补全默认内容。这样上游（SnapshotAssembler ->
// InventoryProviderFromAssembler）保持既有 fail-closed 语义：不上报、不分配
// inventoryRevision、不推进任务（CR-012 D7 / CR-013 D6）。
//
// 安全：错误细节只记录字段名与枚举原值，不记录 VIN、件号或版本原值。
// =============================================================================

#include "ccu/fota/someip/diag_inventory_codec.hpp"

#include "ccu/fota/ota/inventory_error_codes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace ccu_fota {
namespace someip {

namespace {

using json = nlohmann::json;

DiagInventoryDecodeResult invalid(const char* code, std::string detail) {
    DiagInventoryDecodeResult r;
    r.ok = false;
    r.errorCode = code;
    r.errorDetail = std::move(detail);
    return r;
}

DiagInventoryDecodeResult listEmpty(std::string detail) {
    return invalid(ota::inventory_error::kDiagListEmpty, std::move(detail));
}

// 大小写/分隔符无关的枚举规范化："all_ok" / "ALL_OK" / "AllOk" -> "ALL_OK"。
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

// 依次在多个候选键名下取字段（冻结 camelCase + snake_case 别名）。
const json* findField(const json& j, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = j.find(k);
        if (it != j.end() && !it->is_null()) return &(*it);
    }
    return nullptr;
}

bool getString(const json& j, std::initializer_list<const char*> keys,
               std::string& out, bool required, const char* what,
               std::string& err) {
    const json* f = findField(j, keys);
    if (f == nullptr) {
        if (!required) return true;
        err = std::string("missing field: ") + what;
        return false;
    }
    if (!f->is_string()) {
        err = std::string("field type mismatch (expected string): ") + what;
        return false;
    }
    out = f->get<std::string>();
    return true;
}

bool getOptionalString(const json& j, std::initializer_list<const char*> keys,
                       std::optional<std::string>& out, const char* what,
                       std::string& err) {
    const json* f = findField(j, keys);
    if (f == nullptr) {
        out.reset();
        return true;
    }
    if (!f->is_string()) {
        err = std::string("field type mismatch (expected string): ") + what;
        return false;
    }
    // 空字符串按“未提供”处理（DIAG 对可选字段的惯用表示）。
    const auto v = f->get<std::string>();
    if (v.empty()) {
        out.reset();
    } else {
        out = v;
    }
    return true;
}

bool parseInventorySource(std::string_view raw, InventorySource& out) {
    const std::string n = normalizeEnum(raw);
    if (n == "HARDWARE") { out = InventorySource::HARDWARE; return true; }
    if (n == "SEED") { out = InventorySource::SEED; return true; }
    return false;
}

bool parseVinSource(std::string_view raw, VinSource& out) {
    const std::string n = normalizeEnum(raw);
    if (n == "PROVISIONED") { out = VinSource::PROVISIONED; return true; }
    if (n == "UNKNOWN" || n == "NOT_PROVISIONED") {
        out = VinSource::UNKNOWN;
        return true;
    }
    return false;
}

bool parseBaselineSource(std::string_view raw, BaselineSource& out) {
    const std::string n = normalizeEnum(raw);
    if (n == "FACTORY") { out = BaselineSource::FACTORY; return true; }
    if (n == "LAST_OTA") { out = BaselineSource::LAST_OTA; return true; }
    if (n == "UNKNOWN") { out = BaselineSource::UNKNOWN; return true; }
    return false;
}

bool parseCollectionStatus(std::string_view raw, CollectionStatus& out) {
    const std::string n = normalizeEnum(raw);
    if (n == "ALL_OK" || n == "OK") { out = CollectionStatus::ALL_OK; return true; }
    if (n == "PARTIAL") { out = CollectionStatus::PARTIAL; return true; }
    if (n == "FAILED") { out = CollectionStatus::FAILED; return true; }
    return false;
}

bool parseVersionSource(std::string_view raw, VersionSource& out) {
    const std::string n = normalizeEnum(raw);
    if (n == "UDS_0X22" || n == "UDS22" || n == "UDS") {
        out = VersionSource::UDS_0x22;
        return true;
    }
    if (n == "SOMEIP_GET_VERSION" || n == "SOMEIP") {
        out = VersionSource::SOMEIP_GET_VERSION;
        return true;
    }
    return false;
}

bool parseEcuStatus(std::string_view raw, EcuStatus& out) {
    const std::string n = normalizeEnum(raw);
    if (n == "OK") { out = EcuStatus::OK; return true; }
    if (n == "MISSING") { out = EcuStatus::MISSING; return true; }
    if (n == "NRC") { out = EcuStatus::NRC; return true; }
    if (n == "TIMEOUT") { out = EcuStatus::TIMEOUT; return true; }
    if (n == "UNREACHABLE") { out = EcuStatus::UNREACHABLE; return true; }
    if (n == "PARSE_ERROR" || n == "PARSEERROR") {
        out = EcuStatus::PARSE_ERROR;
        return true;
    }
    return false;
}

// 解码单个 ECU 条目；失败细节定位到 ecuList[<index>].<field>（不含值）。
// payloadV2 为 true 时才解码独立 softwarePartNumber（CR-014 D1）：v1 冻结字段集
// 不含该字段，因此即使对端意外提供也不读取，保证不产生跨版本兜底语义。
bool decodeEcuEntry(const json& item, std::size_t index, bool payloadV2,
                    EcuVersionEntry& entry, std::string& err) {
    const std::string prefix = "ecuList[" + std::to_string(index) + "].";
    if (!item.is_object()) {
        err = prefix + "not an object";
        return false;
    }

    if (!getString(item, {"ecuId", "ecu_id"}, entry.ecu_id, /*required=*/true,
                   (prefix + "ecuId").c_str(), err)) {
        return false;
    }
    if (entry.ecu_id.empty()) {
        err = prefix + "ecuId is empty";
        return false;
    }
    if (!getOptionalString(item, {"partNumber", "part_number"},
                           entry.part_number, (prefix + "partNumber").c_str(), err)) {
        return false;
    }
    if (payloadV2 &&
        !getOptionalString(item, {"softwarePartNumber", "software_part_number"},
                           entry.software_part_number,
                           (prefix + "softwarePartNumber").c_str(), err)) {
        return false;
    }
    // serialNumber 是 ECU 实例身份，不属于任何件号字段（CR-014 US-040）。
    // 只做类型校验，不写入 DTO、不参与摘要。
    if (const json* sn = findField(item, {"serialNumber", "serial_number"})) {
        if (!sn->is_string()) {
            err = prefix + "field type mismatch (expected string): serialNumber";
            return false;
        }
    }
    if (!getOptionalString(item, {"swVersion", "sw_version"},
                           entry.sw_version, (prefix + "swVersion").c_str(), err)) {
        return false;
    }
    if (!getOptionalString(item, {"hwVersion", "hw_version"},
                           entry.hw_version, (prefix + "hwVersion").c_str(), err)) {
        return false;
    }
    if (!getOptionalString(item, {"errorCode", "error_code"},
                           entry.error_code, (prefix + "errorCode").c_str(), err)) {
        return false;
    }

    // source / status：缺省取安全默认（不改变清单身份域，仅观测/诊断语义）。
    entry.source = VersionSource::UDS_0x22;
    if (const json* f = findField(item, {"source"})) {
        if (!f->is_string()) {
            err = prefix + "field type mismatch (expected string): source";
            return false;
        }
        if (!parseVersionSource(f->get<std::string>(), entry.source)) {
            err = prefix + "unknown enum for source";
            return false;
        }
    }

    entry.status = EcuStatus::OK;
    if (const json* f = findField(item, {"status"})) {
        if (!f->is_string()) {
            err = prefix + "field type mismatch (expected string): status";
            return false;
        }
        if (!parseEcuStatus(f->get<std::string>(), entry.status)) {
            err = prefix + "unknown enum for status";
            return false;
        }
    }
    return true;
}

} // namespace

DiagInventoryDecodeResult DiagInventoryCodec::decode(std::string_view payload) {
    if (payload.empty()) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       "empty payload");
    }

    json j;
    try {
        j = json::parse(payload.begin(), payload.end());
    } catch (const json::exception& e) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       std::string("json parse failed: ") + e.what());
    }
    if (!j.is_object()) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       "payload is not a json object");
    }

    // ---- schemaVersion：未知/缺失版本 fail-closed（不猜测字段集合） ----
    // DIAG payload 以整数声明版本；同时接受等值字符串（同义表示）。
    std::string schemaVersion;
    std::string err;
    {
        const json* f = findField(j, {"schemaVersion", "schema_version"});
        if (f == nullptr) {
            return invalid(ota::inventory_error::kDiagPayloadInvalid,
                           "missing field: schemaVersion");
        }
        if (f->is_string()) {
            schemaVersion = f->get<std::string>();
        } else if (f->is_number_integer() || f->is_number_unsigned()) {
            schemaVersion = std::to_string(f->get<long long>());
        } else {
            return invalid(ota::inventory_error::kDiagPayloadInvalid,
                           "field type mismatch (expected integer/string): "
                           "schemaVersion");
        }
    }
    if (schemaVersion != kSchemaVersionV1 &&
        schemaVersion != kSchemaVersionV2) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       "unsupported schemaVersion: " + schemaVersion);
    }
    const bool payloadV2 = schemaVersion == kSchemaVersionV2;

    DiagInventoryDecodeResult out;
    out.snapshot.schema_version = schemaVersion;

    // ---- inventorySource（US-038：来源对 FOTA 透明，仅观测） ----
    std::string rawSource;
    if (!getString(j, {"inventorySource", "inventory_source"}, rawSource,
                   /*required=*/true, "inventorySource", err)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid, err);
    }
    if (!parseInventorySource(rawSource, out.snapshot.inventory_source)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       "unknown enum for inventorySource");
    }

    // ---- 车辆身份与基线 ----
    if (!getString(j, {"vin"}, out.snapshot.vin, /*required=*/true, "vin", err)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid, err);
    }
    if (out.snapshot.vin.empty()) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       "vin is empty");
    }

    std::string rawVinSource;
    if (!getString(j, {"vinSource", "vin_source"}, rawVinSource,
                   /*required=*/false, "vinSource", err)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid, err);
    }
    if (!rawVinSource.empty() &&
        !parseVinSource(rawVinSource, out.snapshot.vin_source)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       "unknown enum for vinSource");
    }

    std::optional<std::string> baselineId;
    if (!getOptionalString(j, {"baselineId", "baseline_id", "baselineCode",
                               "baseline_code"},
                           baselineId, "baselineId", err)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid, err);
    }
    out.snapshot.baseline_id = baselineId;

    std::string rawBaselineSource;
    if (!getString(j, {"baselineSource", "baseline_source"}, rawBaselineSource,
                   /*required=*/false, "baselineSource", err)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid, err);
    }
    if (!rawBaselineSource.empty() &&
        !parseBaselineSource(rawBaselineSource, out.snapshot.baseline_source)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       "unknown enum for baselineSource");
    }

    if (!getString(j, {"registryVersion", "registry_version"},
                   out.snapshot.registry_version, /*required=*/true,
                   "registryVersion", err)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid, err);
    }

    if (!getString(j, {"collectedAt", "collected_at"}, out.snapshot.collected_at,
                   /*required=*/false, "collectedAt", err)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid, err);
    }

    std::string rawOverall;
    if (!getString(j, {"overallResult", "overall_result"}, rawOverall,
                   /*required=*/true, "overallResult", err)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid, err);
    }
    if (!parseCollectionStatus(rawOverall, out.snapshot.overall_result)) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       "unknown enum for overallResult");
    }

    // ---- ecuList：必填且非空（空清单不得作为成功清单，D6） ----
    const json* ecuList = findField(j, {"ecuList", "ecu_list"});
    if (ecuList == nullptr) {
        return listEmpty("missing field: ecuList");
    }
    if (!ecuList->is_array()) {
        return invalid(ota::inventory_error::kDiagPayloadInvalid,
                       "field type mismatch (expected array): ecuList");
    }
    if (ecuList->empty()) {
        return listEmpty("ecuList is empty");
    }

    out.snapshot.ecu_list.reserve(ecuList->size());
    std::size_t index = 0;
    for (const auto& item : *ecuList) {
        EcuVersionEntry entry;
        if (!decodeEcuEntry(item, index, payloadV2, entry, err)) {
            return invalid(ota::inventory_error::kDiagPayloadInvalid, err);
        }
        // 重复 ecuId 无法稳定表达逐 ECU 事实 -> fail-closed。
        const bool duplicated = std::any_of(
            out.snapshot.ecu_list.begin(), out.snapshot.ecu_list.end(),
            [&entry](const EcuVersionEntry& e) { return e.ecu_id == entry.ecu_id; });
        if (duplicated) {
            return invalid(ota::inventory_error::kDiagPayloadInvalid,
                           "ecuList[" + std::to_string(index) +
                               "]: duplicate ecuId");
        }
        out.snapshot.ecu_list.push_back(std::move(entry));
        ++index;
    }

    // 采集阶段的序号分配被推迟到上报决策点（CR-012 D2）。
    out.snapshot.snapshot_seq = 0;
    out.ok = true;
    return out;
}

} // namespace someip
} // namespace ccu_fota
