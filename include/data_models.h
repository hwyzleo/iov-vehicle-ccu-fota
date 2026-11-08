#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace ccu_fota {

enum class VersionSource {
    UDS_0x22,
    SOMEIP_GET_VERSION
};

enum class CollectionStatus {
    ALL_OK,
    PARTIAL,
    FAILED
};

enum class EcuStatus {
    OK,
    MISSING,
    NRC,
    TIMEOUT,
    UNREACHABLE,
    PARSE_ERROR
};

enum class BaselineSource {
    FACTORY,
    LAST_OTA,
    UNKNOWN
};

enum class VinSource {
    PROVISIONED,
    UNKNOWN
};

// 清单来源（CCU-FOTA-DSN-CR-013 D8 / US-038）。
// 权威性由 CCU-DIAG 内部决定（seed 兜底或真实硬件诊断），FOTA 只透出到日志/指标，
// 不在 wire 上新增私有字段。
//
// 注意：本枚举不是清单内容的一部分，也不参与 digest/指纹计算。
enum class InventorySource {
    HARDWARE,
    SEED,
    UNKNOWN
};

// 枚举转字符串（用于日志输出，CCU-FOTA-DSN-CR-003）
inline const char* versionSourceToString(VersionSource s) {
    switch (s) {
        case VersionSource::UDS_0x22:           return "UDS_0x22";
        case VersionSource::SOMEIP_GET_VERSION: return "SOMEIP_GET_VERSION";
        default:                                return "UNKNOWN";
    }
}
inline const char* collectionStatusToString(CollectionStatus s) {
    switch (s) {
        case CollectionStatus::ALL_OK:  return "ALL_OK";
        case CollectionStatus::PARTIAL: return "PARTIAL";
        case CollectionStatus::FAILED:  return "FAILED";
        default:                        return "UNKNOWN";
    }
}
inline const char* ecuStatusToString(EcuStatus s) {
    switch (s) {
        case EcuStatus::OK:          return "OK";
        case EcuStatus::MISSING:     return "MISSING";
        case EcuStatus::NRC:         return "NRC";
        case EcuStatus::TIMEOUT:     return "TIMEOUT";
        case EcuStatus::UNREACHABLE: return "UNREACHABLE";
        case EcuStatus::PARSE_ERROR: return "PARSE_ERROR";
        default:                     return "UNKNOWN";
    }
}
inline const char* baselineSourceToString(BaselineSource s) {
    switch (s) {
        case BaselineSource::FACTORY:  return "FACTORY";
        case BaselineSource::LAST_OTA: return "LAST_OTA";
        case BaselineSource::UNKNOWN:  return "UNKNOWN";
        default:                       return "UNKNOWN";
    }
}

inline const char* vinSourceToString(VinSource s) {
    switch (s) {
        case VinSource::PROVISIONED: return "PROVISIONED";
        case VinSource::UNKNOWN:     return "UNKNOWN";
        default:                     return "UNKNOWN";
    }
}

inline const char* inventorySourceToString(InventorySource s) {
    switch (s) {
        case InventorySource::HARDWARE: return "hardware";
        case InventorySource::SEED:     return "seed";
        default:                        return "unknown";
    }
}

struct EcuVersionEntry {
    std::string ecu_id;
    std::optional<std::string> part_number;
    std::optional<std::string> sw_version;
    std::optional<std::string> hw_version;
    VersionSource source = VersionSource::UDS_0x22;
    EcuStatus status = EcuStatus::OK;
    std::optional<std::string> error_code;
};

struct VehicleSoftwareSnapshot {
    // DIAG 清单 payload 的编码版本（CCU-FOTA-DSN-CR-013 §1）。
    // 未知版本 fail-closed：不得猜测字段或降级解码。
    std::string schema_version;
    // 清单来源（hardware | seed），仅用于观测（US-038）。
    InventorySource inventory_source = InventorySource::UNKNOWN;
    std::string vin;
    VinSource vin_source = VinSource::UNKNOWN;
    std::optional<std::string> baseline_id;
    BaselineSource baseline_source = BaselineSource::UNKNOWN;
    std::string registry_version;
    std::string collected_at;
    CollectionStatus overall_result = CollectionStatus::ALL_OK;
    uint64_t snapshot_seq = 0;
    std::vector<EcuVersionEntry> ecu_list;
};

} // namespace ccu_fota
