// =============================================================================
// src/fingerprint/canonical_encoder.cpp
// CCU-FOTA 规范化编码器实现 (CCU-FOTA-DSN-CR-006 §10.2)
// =============================================================================

#include "ccu/fota/fingerprint/canonical_encoder.hpp"

#include <cstring>

namespace ccu_fota {
namespace fingerprint {

CanonicalError::CanonicalError(std::string code, const std::string& message)
    : std::runtime_error(message), errorCode(std::move(code)) {}

// ===========================================================================
// CanonicalEncoder
// ===========================================================================

CanonicalEncoder::CanonicalEncoder(std::string_view domain) {
    // domain: length:u32be || utf8_bytes（长度前缀，不依赖 \0）
    putU32Be(static_cast<std::uint32_t>(domain.size()));
    putString(domain);
    // field_count 占位（finalize 回填）
    fieldCountPos_ = buf_.size();
    putU32Be(0);
}

void CanonicalEncoder::putU8(std::uint8_t v) {
    buf_.push_back(v);
}

void CanonicalEncoder::putU16Be(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void CanonicalEncoder::putU32Be(std::uint32_t v) {
    buf_.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    buf_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void CanonicalEncoder::putBytes(const std::uint8_t* data, std::size_t size) {
    if (size > 0) {
        buf_.insert(buf_.end(), data, data + size);
    }
}

void CanonicalEncoder::putString(std::string_view s) {
    if (!s.empty()) {
        buf_.insert(buf_.end(),
                    reinterpret_cast<const std::uint8_t*>(s.data()),
                    reinterpret_cast<const std::uint8_t*>(s.data()) + s.size());
    }
}

void CanonicalEncoder::putFieldHeader(std::uint16_t fieldId, FieldState state,
                                      std::uint32_t length) {
    putU16Be(fieldId);
    putU8(static_cast<std::uint8_t>(state));
    putU32Be(length);
}

void CanonicalEncoder::writeStringField(std::uint16_t fieldId, std::string_view value) {
    putFieldHeader(fieldId, FieldState::Present,
                   static_cast<std::uint32_t>(value.size()));
    putString(value);
    ++fieldCount_;
}

void CanonicalEncoder::writeOptionalStringField(std::uint16_t fieldId,
                                        const std::optional<std::string>& value) {
    if (!value.has_value()) {
        // missing：state=0, length=0, 无 bytes
        putFieldHeader(fieldId, FieldState::Missing, 0);
    } else {
        putFieldHeader(fieldId, FieldState::Present,
                       static_cast<std::uint32_t>(value->size()));
        putString(*value);
    }
    ++fieldCount_;
}

void CanonicalEncoder::writeEnumField(std::uint16_t fieldId, std::uint32_t code) {
    // 枚举编码为 u32be（4 字节），state=present
    putFieldHeader(fieldId, FieldState::Present, 4);
    putU32Be(code);
    ++fieldCount_;
}

void CanonicalEncoder::writeBytesField(std::uint16_t fieldId,
                                       const std::uint8_t* data, std::size_t size) {
    putFieldHeader(fieldId, FieldState::Present, static_cast<std::uint32_t>(size));
    putBytes(data, size);
    ++fieldCount_;
}

void CanonicalEncoder::writeListField(std::uint16_t fieldId,
                                      const std::vector<std::vector<std::uint8_t>>& itemBytes) {
    // list bytes = item_count:u32be || item*
    std::uint32_t totalLen = 4;  // item_count
    for (const auto& item : itemBytes) {
        totalLen += static_cast<std::uint32_t>(item.size());
    }
    putFieldHeader(fieldId, FieldState::Present, totalLen);
    putU32Be(static_cast<std::uint32_t>(itemBytes.size()));
    for (const auto& item : itemBytes) {
        putBytes(item.data(), item.size());
    }
    ++fieldCount_;
}

std::vector<std::uint8_t> CanonicalEncoder::finalize() {
    // 回填 field_count
    std::uint32_t fc = fieldCount_;
    buf_[fieldCountPos_]     = static_cast<std::uint8_t>((fc >> 24) & 0xFF);
    buf_[fieldCountPos_ + 1] = static_cast<std::uint8_t>((fc >> 16) & 0xFF);
    buf_[fieldCountPos_ + 2] = static_cast<std::uint8_t>((fc >> 8) & 0xFF);
    buf_[fieldCountPos_ + 3] = static_cast<std::uint8_t>(fc & 0xFF);
    return std::move(buf_);
}

} // namespace fingerprint
} // namespace ccu_fota
