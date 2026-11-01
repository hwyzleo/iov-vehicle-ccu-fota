// =============================================================================
// src/fingerprint/inventory_fingerprint.cpp
// CCU-FOTA 业务摘要实现 (CCU-FOTA-DSN-CR-006 §10.1/10.3/10.4/10.5)
// =============================================================================

#include "ccu/fota/fingerprint/inventory_fingerprint.hpp"
#include "ccu/fota/fingerprint/canonical_encoder.hpp"

#include <ccu/fw/hash/sha256.hpp>

#include <algorithm>
#include <tuple>
#include <vector>

namespace ccu_fota {
namespace fingerprint {

using ccu::fw::hash::ByteView;

namespace {

// ===========================================================================
// 枚举 -> 稳定数值映射（不使用编译器 enum 内存值）
// ===========================================================================
std::uint32_t baselineSourceCode(BaselineSource s) {
    switch (s) {
        case BaselineSource::FACTORY:  return codes::BASELINE_FACTORY;
        case BaselineSource::LAST_OTA: return codes::BASELINE_LAST_OTA;
        case BaselineSource::UNKNOWN:  return codes::BASELINE_UNKNOWN;
    }
    return codes::BASELINE_UNKNOWN;
}

std::uint32_t versionSourceCode(VersionSource s) {
    switch (s) {
        case VersionSource::UDS_0x22:           return codes::VERSION_UDS_0x22;
        case VersionSource::SOMEIP_GET_VERSION: return codes::VERSION_SOMEIP_GET_VERSION;
    }
    return codes::VERSION_UDS_0x22;
}

std::uint32_t ecuStatusCode(EcuStatus s) {
    switch (s) {
        case EcuStatus::OK:          return codes::ECU_OK;
        case EcuStatus::MISSING:     return codes::ECU_MISSING;
        case EcuStatus::NRC:         return codes::ECU_NRC;
        case EcuStatus::TIMEOUT:     return codes::ECU_TIMEOUT;
        case EcuStatus::UNREACHABLE: return codes::ECU_UNREACHABLE;
        case EcuStatus::PARSE_ERROR: return codes::ECU_PARSE_ERROR;
    }
    return codes::ECU_OK;
}

std::uint32_t collectionResultCode(CollectionStatus s) {
    switch (s) {
        case CollectionStatus::ALL_OK:  return codes::RESULT_ALL_OK;
        case CollectionStatus::PARTIAL: return codes::RESULT_PARTIAL;
        case CollectionStatus::FAILED:  return codes::RESULT_FAILED;
    }
    return codes::RESULT_ALL_OK;
}

std::uint32_t vinSourceCode(VinSource s) {
    switch (s) {
        case VinSource::PROVISIONED: return codes::VIN_PROVISIONED;
        case VinSource::UNKNOWN:     return codes::VIN_UNKNOWN;
    }
    return codes::VIN_UNKNOWN;
}

// ===========================================================================
// VIN 规范化：大写 ASCII（仅 a-z -> A-Z，不做 locale case-fold / Unicode 归一化）
// ===========================================================================
std::string normalizeVin(std::string_view vin) {
    std::string out(vin);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

const char* identifierKindSuffix(IdentifierKind kind) {
    return kind == IdentifierKind::Vin ? "vin" : "device_sn";
}

// ===========================================================================
// ECU 稳定排序：按 (ecuId, partNumber, swVersion, 原始索引) 全序排序；
// 若 (ecuId, partNumber, swVersion) 仍重复 -> CanonicalError，不回退输入顺序。
// ===========================================================================
std::vector<std::size_t> sortEcuIndices(const std::vector<EcuVersionEntry>& ecus) {
    std::vector<std::size_t> order(ecus.size());
    for (std::size_t i = 0; i < ecus.size(); ++i) {
        order[i] = i;
    }
    auto stableKey = [&](std::size_t i) {
        return std::tuple<std::string, std::string, std::string, std::size_t>(
            ecus[i].ecu_id,
            ecus[i].part_number.value_or(""),
            ecus[i].sw_version.value_or(""),
            i);
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) {
                         return stableKey(a) < stableKey(b);
                     });
    // 重复稳定键检测
    for (std::size_t i = 1; i < order.size(); ++i) {
        auto ka = std::tuple<std::string, std::string, std::string>(
            ecus[order[i - 1]].ecu_id,
            ecus[order[i - 1]].part_number.value_or(""),
            ecus[order[i - 1]].sw_version.value_or(""));
        auto kb = std::tuple<std::string, std::string, std::string>(
            ecus[order[i]].ecu_id,
            ecus[order[i]].part_number.value_or(""),
            ecus[order[i]].sw_version.value_or(""));
        if (ka == kb) {
            throw CanonicalError("CCU-FOTA-2001",
                                 "duplicate ECU stable key: ecuId=" +
                                     ecus[order[i]].ecu_id);
        }
    }
    return order;
}

// versionFingerprint ECU 子记录字段：ecuId/partNumber/swVersion/hwVersion
std::vector<std::uint8_t> encodeVersionEcu(const EcuVersionEntry& e) {
    CanonicalEncoder sub("");
    sub.writeStringField(0x0001, e.ecu_id);
    sub.writeOptionalStringField(0x0002, e.part_number);
    sub.writeOptionalStringField(0x0003, e.sw_version);
    sub.writeOptionalStringField(0x0004, e.hw_version);
    return sub.finalize();
}

// snapshotFingerprint ECU 子记录字段：ecuId/partNumber/swVersion/hwVersion/source/status/errorCode
std::vector<std::uint8_t> encodeSnapshotEcu(const EcuVersionEntry& e) {
    CanonicalEncoder sub("");
    sub.writeStringField(0x0001, e.ecu_id);
    sub.writeOptionalStringField(0x0002, e.part_number);
    sub.writeOptionalStringField(0x0003, e.sw_version);
    sub.writeOptionalStringField(0x0004, e.hw_version);
    sub.writeEnumField(0x0005, versionSourceCode(e.source));
    sub.writeEnumField(0x0006, ecuStatusCode(e.status));
    sub.writeOptionalStringField(0x0007, e.error_code);
    return sub.finalize();
}

// 计算 framework SHA-256 并构造 Fingerprint（调用 sha256 + sha256_hex）。
// 任一失败抛 ccu::fw::hash::HashException(CCU-FW-0401/0402)，fail-closed。
Fingerprint makeFingerprint(std::string_view canonicalization,
                            const std::vector<std::uint8_t>& bytes) {
    ByteView view{bytes.data(), bytes.size()};
    Fingerprint fp;
    fp.algorithm = "sha-256";
    fp.canonicalization = std::string(canonicalization);
    fp.digest = ccu::fw::hash::sha256(view);
    fp.hexStr = ccu::fw::hash::sha256_hex(view);
    return fp;
}

} // namespace

// ===========================================================================
// versionFingerprint v1
// ===========================================================================
Fingerprint buildVersionFingerprint(const VehicleSoftwareSnapshot& snapshot) {
    CanonicalEncoder enc(VERSION_DOMAIN);
    enc.writeEnumField(0x0001, baselineSourceCode(snapshot.baseline_source));
    enc.writeOptionalStringField(0x0002, snapshot.baseline_id);
    enc.writeStringField(0x0003, snapshot.registry_version);

    std::vector<std::size_t> order = sortEcuIndices(snapshot.ecu_list);
    std::vector<std::vector<std::uint8_t>> ecuBytes;
    ecuBytes.reserve(order.size());
    for (std::size_t idx : order) {
        ecuBytes.push_back(encodeVersionEcu(snapshot.ecu_list[idx]));
    }
    enc.writeListField(0x0004, ecuBytes);

    return makeFingerprint(VERSION_DOMAIN, enc.finalize());
}

// ===========================================================================
// snapshotFingerprint v1
// ===========================================================================
Fingerprint buildSnapshotFingerprint(const VehicleSoftwareSnapshot& snapshot) {
    // VIN 使用 identifierDigest(vin)，不放入明文 VIN
    std::string vinDigest = buildIdentifierDigest(IdentifierKind::Vin,
                                                  normalizeVin(snapshot.vin));

    CanonicalEncoder enc(SNAPSHOT_DOMAIN);
    enc.writeStringField(0x0001, vinDigest);
    enc.writeEnumField(0x0002, vinSourceCode(snapshot.vin_source));
    enc.writeEnumField(0x0003, baselineSourceCode(snapshot.baseline_source));
    enc.writeOptionalStringField(0x0004, snapshot.baseline_id);
    enc.writeStringField(0x0005, snapshot.registry_version);
    enc.writeEnumField(0x0006, collectionResultCode(snapshot.overall_result));

    std::vector<std::size_t> order = sortEcuIndices(snapshot.ecu_list);
    std::vector<std::vector<std::uint8_t>> ecuBytes;
    ecuBytes.reserve(order.size());
    for (std::size_t idx : order) {
        ecuBytes.push_back(encodeSnapshotEcu(snapshot.ecu_list[idx]));
    }
    enc.writeListField(0x0007, ecuBytes);

    return makeFingerprint(SNAPSHOT_DOMAIN, enc.finalize());
}

// ===========================================================================
// dedupeKey v1
// ===========================================================================
std::string buildDedupeKey(const Fingerprint& snapshotFingerprint,
                           std::string_view destination) {
    CanonicalEncoder enc(DEDUPE_DOMAIN);
    enc.writeStringField(0x0001, destination);
    enc.writeStringField(0x0002, snapshotFingerprint.canonicalization);
    enc.writeBytesField(0x0003, snapshotFingerprint.digest.data(),
                        snapshotFingerprint.digest.size());
    std::vector<std::uint8_t> bytes = enc.finalize();
    return ccu::fw::hash::sha256_hex(ByteView{bytes.data(), bytes.size()});
}

// ===========================================================================
// identifierDigest
// ===========================================================================
std::string buildIdentifierDigest(IdentifierKind kind,
                                  std::string_view normalizedValue) {
    std::string domain = std::string(ID_DOMAIN_PREFIX) + identifierKindSuffix(kind);
    CanonicalEncoder enc(domain);
    enc.writeStringField(0x0001, normalizedValue);
    std::vector<std::uint8_t> bytes = enc.finalize();
    return ccu::fw::hash::sha256_hex(ByteView{bytes.data(), bytes.size()});
}

} // namespace fingerprint
} // namespace ccu_fota
