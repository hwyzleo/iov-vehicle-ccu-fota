// =============================================================================
// tests/test_canonical_encoder.cpp
// CCU-FOTA 规范化编码器单元测试 (CCU-FOTA-DSN-CR-006 §10.2 / §测试设计)
// 覆盖：golden bytes、field_count 回填、missing/null/empty/present 互不混淆、
//       枚举 u32be、列表子记录、长度前缀域。
// =============================================================================

#include "ccu/fota/fingerprint/canonical_encoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace ccu_fota::fingerprint;

namespace {

// 字节向量比较失败时输出 hex，便于定位。
std::string toHex(const std::vector<std::uint8_t>& b) {
    static const char* h = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (auto x : b) {
        s.push_back(h[x >> 4]);
        s.push_back(h[x & 0xF]);
    }
    return s;
}

void expectBytes(const std::vector<std::uint8_t>& actual,
                 const std::vector<std::uint8_t>& expected) {
    EXPECT_EQ(toHex(actual), toHex(expected))
        << "actual=" << toHex(actual) << " expected=" << toHex(expected);
}

std::vector<std::uint8_t> bytes(const std::string& hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(
            std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

} // namespace

// ===========================================================================
// Golden bytes：空子记录
// ===========================================================================
TEST(CanonicalEncoderTest, EmptySubRecord) {
    CanonicalEncoder enc("");
    expectBytes(enc.finalize(), bytes("00000000" "00000000"));
}

// ===========================================================================
// Golden bytes：域 + 单字符串字段
//   domain="d" -> 00000001 64
//   field_count=1 -> 00000001
//   field id=1 present "ab" -> 0001 02 00000002 6162
// ===========================================================================
TEST(CanonicalEncoderTest, DomainWithOneStringField) {
    CanonicalEncoder enc("d");
    enc.writeStringField(0x0001, std::string_view("ab"));
    expectBytes(enc.finalize(),
                bytes("00000001" "64"
                      "00000001"
                      "0001" "02" "00000002" "6162"));
}

// ===========================================================================
// missing / null / 空字符串 / 非空字符串 互不混淆
// ===========================================================================
TEST(CanonicalEncoderTest, FieldStatesDiffer) {
    // missing=00, empty present length=0, present length=1 三者字节不同
    CanonicalEncoder missing("");
    missing.writeOptionalStringField(0x0001, std::optional<std::string>());
    CanonicalEncoder empty("");
    empty.writeStringField(0x0001, std::string(""));
    CanonicalEncoder present("");
    present.writeStringField(0x0001, std::string("x"));

    auto missingBytes = missing.finalize();
    auto emptyBytes = empty.finalize();
    auto presentBytes = present.finalize();

    // missing: ... 0001 00 00000000
    // empty : ... 0001 02 00000000
    // present: ... 0001 02 00000001 78
    EXPECT_NE(toHex(missingBytes), toHex(emptyBytes));
    EXPECT_NE(toHex(emptyBytes), toHex(presentBytes));
    EXPECT_NE(toHex(missingBytes), toHex(presentBytes));
    expectBytes(missingBytes, bytes("00000000" "00000001" "0001" "00" "00000000"));
    expectBytes(emptyBytes, bytes("00000000" "00000001" "0001" "02" "00000000"));
    expectBytes(presentBytes, bytes("00000000" "00000001" "0001" "02" "00000001" "78"));
}

// ===========================================================================
// 枚举字段：u32be
//   field id=5 present code=2 -> 0005 02 00000004 00000002
// ===========================================================================
TEST(CanonicalEncoderTest, EnumFieldU32Be) {
    CanonicalEncoder enc("");
    enc.writeEnumField(0x0005, 2u);
    expectBytes(enc.finalize(),
                bytes("00000000" "00000001" "0005" "02" "00000004" "00000002"));
}

// ===========================================================================
// field_count 回填：多字段计数正确
// ===========================================================================
TEST(CanonicalEncoderTest, FieldCountBackPatched) {
    CanonicalEncoder enc("");
    enc.writeStringField(0x0001, std::string("a"));
    enc.writeEnumField(0x0002, 1u);
    enc.writeOptionalStringField(0x0003, std::optional<std::string>());
    auto b = enc.finalize();
    // field_count 在 offset 4（domain_len 占 4 字节后）
    ASSERT_GE(b.size(), 8u);
    EXPECT_EQ(b[4], 0x00);
    EXPECT_EQ(b[5], 0x00);
    EXPECT_EQ(b[6], 0x00);
    EXPECT_EQ(b[7], 0x03);  // 3 fields
}

// ===========================================================================
// 列表字段：item_count + 子记录
//   一个 item（子记录：domain_len=0, field_count=1, field id=1 present "x"）
//   item = 00000000 00000001 0001 02 00000001 78  (16 bytes)
//   list field id=7 present length=20(0x14) -> 0007 02 00000014
//   item_count=1 -> 00000001
//   item bytes
// ===========================================================================
TEST(CanonicalEncoderTest, ListFieldWithSubRecord) {
    CanonicalEncoder item("");
    item.writeStringField(0x0001, std::string("x"));
    auto itemBytes = item.finalize();
    ASSERT_EQ(itemBytes.size(), 16u);

    CanonicalEncoder enc("");
    enc.writeListField(0x0007, {itemBytes});
    auto b = enc.finalize();
    // record = domain_len(4)=0 + field_count(4)=1 + list field
    // list field = id(2)+state(1)+length(4) + item_count(4) + item(16) = 7 + 4 + 16 = 27
    ASSERT_EQ(b.size(), 4u + 4u + 27u);
    // field_count
    EXPECT_EQ(b[7], 0x01);
    // list field header at offset 8: 0007 02 00000014
    EXPECT_EQ(b[8], 0x00); EXPECT_EQ(b[9], 0x07);
    EXPECT_EQ(b[10], 0x02);
    EXPECT_EQ(b[14], 0x14);  // length=20
    // item_count at offset 15
    EXPECT_EQ(b[18], 0x01);
}

// ===========================================================================
// 空列表：item_count=0, length=4
// ===========================================================================
TEST(CanonicalEncoderTest, EmptyList) {
    CanonicalEncoder enc("");
    enc.writeListField(0x0001, {});
    auto b = enc.finalize();
    // list field: 0001 02 00000004 00000000
    expectBytes(b, bytes("00000000" "00000001"
                         "0001" "02" "00000004" "00000000"));
}

// ===========================================================================
// 原始字节字段
// ===========================================================================
TEST(CanonicalEncoderTest, BytesField) {
    std::vector<std::uint8_t> raw = {0xde, 0xad, 0xbe, 0xef};
    CanonicalEncoder enc("");
    enc.writeBytesField(0x0003, raw.data(), raw.size());
    expectBytes(enc.finalize(),
                bytes("00000000" "00000001" "0003" "02" "00000004" "deadbeef"));
}
