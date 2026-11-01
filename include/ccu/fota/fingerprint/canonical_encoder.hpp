#pragma once

// =============================================================================
// include/ccu/fota/fingerprint/canonical_encoder.hpp
// CCU-FOTA 规范化编码器 (CCU-FOTA-DSN-CR-006 §10.2)
// =============================================================================
// 将 FOTA 业务对象规范化为唯一、确定、可测试的字节序列，供 ccu-framework-hash
// 计算 SHA-256。编码器只负责 FOTA 业务格式（长度前缀二进制、大端、字段排序），
// 不实现 SHA-256 或 Hex；摘要计算仅经 <ccu/fw/hash/sha256.hpp>。
//
// 编码格式（大端）：
//   record   := domain || field_count:u32be || field*
//   domain   := length:u32be || utf8_bytes            (长度前缀，不依赖 \0)
//   field    := field_id:u16be || state:u8 || length:u32be || bytes
//   state    := 0=missing, 1=null, 2=present
//   string   := present 字段，bytes 为 UTF-8（不做 locale 转换）
//   integer  := present 字段，bytes 为固定宽度大端（枚举用 u32be）
//   list     := present 字段，bytes = item_count:u32be || item*
//   item     := field_count:u32be || field*           (子记录，无 domain)
//
// 不变量：
//   - missing / null / 空字符串产生不同字节。
//   - 未知字段默认不参与既有版本；加入摘要须递增 canonicalization 版本。
//   - 枚举使用本文件定义的稳定数值，不使用编译器 enum 内存值。
// =============================================================================

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ccu_fota {
namespace fingerprint {

// ---------------------------------------------------------------------------
// CanonicalError - 规范化失败（重复稳定键 / 非法 UTF-8 / 长度溢出 / 不支持字段）
// message 仅含 stage / field / length 摘要，不含原始 canonical bytes、VIN 或快照。
// 映射为 FOTA 侧 fail-closed：不产生空/全零摘要，不跳过去重继续提交。
// ---------------------------------------------------------------------------
class CanonicalError : public std::runtime_error {
public:
    std::string errorCode;  // "CCU-FOTA-2001" 等
    CanonicalError(std::string code, const std::string& message);
};

// 字段状态码（稳定，不依赖编译器枚举布局）
enum class FieldState : std::uint8_t {
    Missing = 0,
    Null    = 1,
    Present = 2,
};

// ---------------------------------------------------------------------------
// 稳定枚举数值（CCU-FOTA-DSN-CR-006 §10.2：不使用编译器 enum 内存值）
// 这些数值是 canonicalization 契约的一部分，变更须递增摘要格式版本。
// ---------------------------------------------------------------------------
namespace codes {
// BaselineSource
constexpr std::uint32_t BASELINE_FACTORY  = 1;
constexpr std::uint32_t BASELINE_LAST_OTA = 2;
constexpr std::uint32_t BASELINE_UNKNOWN  = 3;
// VersionSource
constexpr std::uint32_t VERSION_UDS_0x22          = 1;
constexpr std::uint32_t VERSION_SOMEIP_GET_VERSION = 2;
// EcuStatus
constexpr std::uint32_t ECU_OK          = 1;
constexpr std::uint32_t ECU_MISSING     = 2;
constexpr std::uint32_t ECU_NRC         = 3;
constexpr std::uint32_t ECU_TIMEOUT     = 4;
constexpr std::uint32_t ECU_UNREACHABLE = 5;
constexpr std::uint32_t ECU_PARSE_ERROR = 6;
// CollectionStatus
constexpr std::uint32_t RESULT_ALL_OK  = 1;
constexpr std::uint32_t RESULT_PARTIAL = 2;
constexpr std::uint32_t RESULT_FAILED  = 3;
// VinSource
constexpr std::uint32_t VIN_PROVISIONED = 1;
constexpr std::uint32_t VIN_UNKNOWN     = 2;
} // namespace codes

// ---------------------------------------------------------------------------
// CanonicalEncoder - 构造时写入 domain（可为空，用于子记录）与 field_count 占位；
// finalize() 回填 field_count 并返回字节。field_count 由 writeXxxField 自动累计。
// ---------------------------------------------------------------------------
class CanonicalEncoder {
public:
    // domain 为空表示子记录（无 domain 前缀，仅有 field_count + 字段）。
    explicit CanonicalEncoder(std::string_view domain = "");

    CanonicalEncoder(const CanonicalEncoder&) = delete;
    CanonicalEncoder& operator=(const CanonicalEncoder&) = delete;

    // 字符串字段（present）。
    void writeStringField(std::uint16_t fieldId, std::string_view value);
    // 可选字符串字段（missing/present）。
    void writeOptionalStringField(std::uint16_t fieldId,
                                  const std::optional<std::string>& value);

    // 枚举/整数字段（u32be）。
    void writeEnumField(std::uint16_t fieldId, std::uint32_t code);

    // 原始字节字段（present）。
    void writeBytesField(std::uint16_t fieldId, const std::uint8_t* data, std::size_t size);

    // 列表字段：itemBytes 为每个子记录已 finalize 的字节（已按稳定键排序）。
    void writeListField(std::uint16_t fieldId,
                        const std::vector<std::vector<std::uint8_t>>& itemBytes);

    // 回填 field_count 并返回字节。
    std::vector<std::uint8_t> finalize();

private:
    std::vector<std::uint8_t> buf_;
    std::size_t fieldCountPos_ = 0;  // field_count 占位偏移
    std::uint32_t fieldCount_ = 0;

    void putU8(std::uint8_t v);
    void putU16Be(std::uint16_t v);
    void putU32Be(std::uint32_t v);
    void putBytes(const std::uint8_t* data, std::size_t size);
    void putString(std::string_view s);
    void putFieldHeader(std::uint16_t fieldId, FieldState state, std::uint32_t length);
};

} // namespace fingerprint
} // namespace ccu_fota
