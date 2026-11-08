// =============================================================================
// tests/someip/diag_inventory_codec_test.cpp
// CCU-DIAG 清单 payload 解码器单元测试 (CCU-FOTA-DSN-CR-013 §测试矩阵 单元 1
//                                          / CCU-FOTA-DSN-CR-014 D1)
// -----------------------------------------------------------------------------
// 覆盖：payload v1/v2 解码（v2 独立软件件号）、serialNumber 不进入件号字段、
// 字段名 camelCase/snake_case 别名、缺字段、空 ecuList、未知 schemaVersion、
// 非法枚举、重复 ecuId、非 JSON -> 全部 fail-closed。
// =============================================================================

#include "ccu/fota/someip/diag_inventory_codec.hpp"

#include "ccu/fota/ota/inventory_error_codes.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace ccu_fota;
using namespace ccu_fota::someip;

namespace {

std::string validPayload() {
    return R"({
      "schemaVersion": 1,
      "inventorySource": "hardware",
      "vin": "VINTEST0000000001",
      "vinSource": "PROVISIONED",
      "baselineId": "BASE-001",
      "baselineSource": "factory",
      "registryVersion": "2026.07.1",
      "collectedAt": "2026-11-08T10:00:00Z",
      "overallResult": "all_ok",
      "ecuList": [
        {"ecuId":"BMS","partNumber":"PN-BMS","swVersion":"1.0.0","hwVersion":"HW-BMS",
         "source":"UDS_0x22","status":"ok","errorCode":""},
        {"ecuId":"VCU","partNumber":"PN-VCU","swVersion":"2.0.0",
         "source":"SOMEIP_GET_VERSION","status":"ok"}
      ]
    })";
}

// payload v2（CCU-DIAG-DSN-CR-012 §5.2）：独立软件件号 + 序列号 + 硬件版本。
std::string validPayloadV2() {
    return R"({
      "schemaVersion": 2,
      "inventorySource": "hardware",
      "vin": "VINTEST0000000001",
      "vinSource": "PROVISIONED",
      "baselineId": "BASE-001",
      "baselineSource": "factory",
      "registryVersion": "2026.11.1",
      "collectedAt": "2026-11-08T10:00:00Z",
      "overallResult": "all_ok",
      "ecuList": [
        {"ecuId":"BMS","partNumber":"PN-BMS","softwarePartNumber":"SW-BMS",
         "serialNumber":"SN-BMS","swVersion":"1.0.0","hwVersion":"HW-BMS",
         "source":"UDS_0x22","status":"ok","errorCode":""},
        {"ecuId":"VCU","partNumber":"PN-VCU","softwarePartNumber":"SW-VCU",
         "serialNumber":"SN-VCU","swVersion":"2.0.0","hwVersion":"HW-VCU",
         "source":"SOMEIP_GET_VERSION","status":"ok"}
      ]
    })";
}

// 用给定 JSON 文本做一次解码并返回错误码（成功返回空串）。
std::string decodeError(const std::string& payload) {
    auto r = DiagInventoryCodec::decode(payload);
    return r.ok ? std::string() : r.errorCode;
}

// 精确子串替换（长度由 needle 自身决定，避免手数偏移量）。
void replaceOnce(std::string& hay, const std::string& needle,
                 const std::string& repl) {
    const auto pos = hay.find(needle);
    ASSERT_NE(pos, std::string::npos) << "needle not found: " << needle;
    hay.replace(pos, needle.size(), repl);
}

} // namespace

// ---------------------------------------------------------------------------
// 合法 payload：逐 ECU 明细完整映射，不丢失 summary 之外的字段
// ---------------------------------------------------------------------------
TEST(DiagInventoryCodec, DecodesFrozenPayloadV1) {
    auto r = DiagInventoryCodec::decode(validPayload());
    ASSERT_TRUE(r.ok) << r.errorCode << " " << r.errorDetail;
    const auto& s = r.snapshot;
    EXPECT_EQ(s.schema_version, "1");
    EXPECT_EQ(s.inventory_source, InventorySource::HARDWARE);
    EXPECT_EQ(s.vin, "VINTEST0000000001");
    EXPECT_EQ(s.vin_source, VinSource::PROVISIONED);
    ASSERT_TRUE(s.baseline_id.has_value());
    EXPECT_EQ(*s.baseline_id, "BASE-001");
    EXPECT_EQ(s.baseline_source, BaselineSource::FACTORY);
    EXPECT_EQ(s.registry_version, "2026.07.1");
    EXPECT_EQ(s.collected_at, "2026-11-08T10:00:00Z");
    EXPECT_EQ(s.overall_result, CollectionStatus::ALL_OK);
    // 采集阶段不分配序号（CR-012 D2）。
    EXPECT_EQ(s.snapshot_seq, 0u);

    ASSERT_EQ(s.ecu_list.size(), 2u);
    EXPECT_EQ(s.ecu_list[0].ecu_id, "BMS");
    ASSERT_TRUE(s.ecu_list[0].part_number.has_value());
    EXPECT_EQ(*s.ecu_list[0].part_number, "PN-BMS");
    EXPECT_EQ(s.ecu_list[0].sw_version.value_or(""), "1.0.0");
    EXPECT_EQ(s.ecu_list[0].hw_version.value_or(""), "HW-BMS");
    EXPECT_EQ(s.ecu_list[0].source, VersionSource::UDS_0x22);
    EXPECT_EQ(s.ecu_list[0].status, EcuStatus::OK);
    // 空字符串按“未提供”处理。
    EXPECT_FALSE(s.ecu_list[0].error_code.has_value());

    EXPECT_EQ(s.ecu_list[1].ecu_id, "VCU");
    EXPECT_EQ(s.ecu_list[1].source, VersionSource::SOMEIP_GET_VERSION);
    EXPECT_FALSE(s.ecu_list[1].hw_version.has_value());
}

// ---------------------------------------------------------------------------
// seed 来源透出（US-038：wire 不新增字段，仅在本地标识）
// ---------------------------------------------------------------------------
TEST(DiagInventoryCodec, DecodesSeedInventorySource) {
    std::string payload = validPayload();
    payload.replace(payload.find("\"hardware\""), 10, "\"seed\"");
    auto r = DiagInventoryCodec::decode(payload);
    ASSERT_TRUE(r.ok) << r.errorCode;
    EXPECT_EQ(r.snapshot.inventory_source, InventorySource::SEED);
}

// ---------------------------------------------------------------------------
// snake_case 别名（同义字段名，不引入第二套语义）
// ---------------------------------------------------------------------------
TEST(DiagInventoryCodec, AcceptsSnakeCaseAliases) {
    const std::string payload = R"({
      "schema_version": 1,
      "inventory_source": "SEED",
      "vin": "VINTEST0000000002",
      "baseline_id": "BASE-002",
      "registry_version": "REG-9",
      "overall_result": "PARTIAL",
      "ecu_list": [
        {"ecu_id":"IDCM","part_number":"PN-IDCM","sw_version":"3.1.0","status":"NRC"}
      ]
    })";
    auto r = DiagInventoryCodec::decode(payload);
    ASSERT_TRUE(r.ok) << r.errorCode << " " << r.errorDetail;
    EXPECT_EQ(r.snapshot.inventory_source, InventorySource::SEED);
    EXPECT_EQ(r.snapshot.overall_result, CollectionStatus::PARTIAL);
    ASSERT_EQ(r.snapshot.ecu_list.size(), 1u);
    EXPECT_EQ(r.snapshot.ecu_list[0].ecu_id, "IDCM");
    EXPECT_EQ(r.snapshot.ecu_list[0].status, EcuStatus::NRC);
}

// ---------------------------------------------------------------------------
// 空清单：必须是独立错误码（-DIAG-LIST-EMPTY），不得上报空清单
// ---------------------------------------------------------------------------
TEST(DiagInventoryCodec, EmptyEcuListFailsClosed) {
    std::string payload = validPayload();
    const auto listStart = payload.find("\"ecuList\"");
    ASSERT_NE(listStart, std::string::npos);
    const auto open = payload.find('[', listStart);
    const auto close = payload.find(']', listStart);
    payload.replace(open, close - open + 1, "[]");
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagListEmpty);
}

TEST(DiagInventoryCodec, MissingEcuListFailsClosed) {
    const std::string payload = R"({
      "schemaVersion": 1,
      "inventorySource": "hardware",
      "vin": "VINTEST0000000003",
      "registryVersion": "2026.07.1",
      "overallResult": "all_ok"
    })";
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagListEmpty);
}

// ---------------------------------------------------------------------------
// payload v2：五类事实独立解码，partNumber 与 softwarePartNumber 保持不同值；
// serialNumber 不进入任何件号字段（CR-014 US-040）。
// ---------------------------------------------------------------------------
TEST(DiagInventoryCodec, DecodesPayloadV2WithIndependentSoftwarePartNumber) {
    auto r = DiagInventoryCodec::decode(validPayloadV2());
    ASSERT_TRUE(r.ok) << r.errorCode << " " << r.errorDetail;
    const auto& s = r.snapshot;
    EXPECT_EQ(s.schema_version, "2");
    EXPECT_EQ(s.registry_version, "2026.11.1");
    ASSERT_EQ(s.ecu_list.size(), 2u);

    EXPECT_EQ(s.ecu_list[0].ecu_id, "BMS");
    ASSERT_TRUE(s.ecu_list[0].part_number.has_value());
    EXPECT_EQ(*s.ecu_list[0].part_number, "PN-BMS");
    ASSERT_TRUE(s.ecu_list[0].software_part_number.has_value());
    EXPECT_EQ(*s.ecu_list[0].software_part_number, "SW-BMS");
    EXPECT_NE(*s.ecu_list[0].part_number,
              *s.ecu_list[0].software_part_number);
    EXPECT_EQ(s.ecu_list[0].sw_version.value_or(""), "1.0.0");
    EXPECT_EQ(s.ecu_list[0].hw_version.value_or(""), "HW-BMS");

    EXPECT_EQ(s.ecu_list[1].ecu_id, "VCU");
    EXPECT_EQ(*s.ecu_list[1].software_part_number, "SW-VCU");
    EXPECT_EQ(s.ecu_list[1].hw_version.value_or(""), "HW-VCU");

    // serialNumber 不入任何 PN 字段（本 DTO 无 SN 字段）。
    for (const auto& e : s.ecu_list) {
        if (e.part_number) EXPECT_NE(*e.part_number, "SN-BMS");
        if (e.part_number) EXPECT_NE(*e.part_number, "SN-VCU");
        if (e.software_part_number) {
            EXPECT_NE(*e.software_part_number, "SN-BMS");
            EXPECT_NE(*e.software_part_number, "SN-VCU");
        }
    }
}

// v2 未知软件件号输出空串（DIAG 契约）=> 按“未提供”处理，由采集端口 fail-closed。
TEST(DiagInventoryCodec, PayloadV2EmptySoftwarePartNumberIsMissing) {
    std::string payload = validPayloadV2();
    replaceOnce(payload, "\"softwarePartNumber\":\"SW-BMS\"",
                "\"softwarePartNumber\":\"\"");
    auto r = DiagInventoryCodec::decode(payload);
    ASSERT_TRUE(r.ok) << r.errorCode << " " << r.errorDetail;
    EXPECT_FALSE(r.snapshot.ecu_list[0].software_part_number.has_value());
    // 仍不得用 partNumber 补齐。
    EXPECT_EQ(*r.snapshot.ecu_list[0].part_number, "PN-BMS");
}

// payload v1：字段集冻结，无软件件号 => 保持缺失（即使对端意外提供也不读取）。
TEST(DiagInventoryCodec, PayloadV1NeverPopulatesSoftwarePartNumber) {
    auto r = DiagInventoryCodec::decode(validPayload());
    ASSERT_TRUE(r.ok) << r.errorCode << " " << r.errorDetail;
    ASSERT_EQ(r.snapshot.ecu_list.size(), 2u);
    EXPECT_FALSE(r.snapshot.ecu_list[0].software_part_number.has_value());
    EXPECT_FALSE(r.snapshot.ecu_list[1].software_part_number.has_value());
    // 总成件号仍然可用，且不被解释为软件件号。
    ASSERT_TRUE(r.snapshot.ecu_list[0].part_number.has_value());
    EXPECT_EQ(*r.snapshot.ecu_list[0].part_number, "PN-BMS");
}

TEST(DiagInventoryCodec, PayloadV1IgnoresUnexpectedSoftwarePartNumber) {
    std::string payload = validPayload();
    replaceOnce(payload, "\"partNumber\":\"PN-BMS\"",
                "\"partNumber\":\"PN-BMS\",\"softwarePartNumber\":\"SW-BMS\"");
    auto r = DiagInventoryCodec::decode(payload);
    ASSERT_TRUE(r.ok) << r.errorCode;
    // v1 不解码该字段：保持缺失，不建立跨版本兜底语义。
    EXPECT_FALSE(r.snapshot.ecu_list[0].software_part_number.has_value());
}

// serialNumber 类型非法 -> fail-closed（字段仍然要结构化校验）。
TEST(DiagInventoryCodec, PayloadV2InvalidSerialNumberFailsClosed) {
    std::string payload = validPayloadV2();
    replaceOnce(payload, "\"serialNumber\":\"SN-BMS\"",
                "\"serialNumber\":123");
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

// ---------------------------------------------------------------------------
// 未知/缺失 schemaVersion：不猜测字段集合
// ---------------------------------------------------------------------------
TEST(DiagInventoryCodec, UnknownSchemaVersionFailsClosed) {
    std::string payload = validPayloadV2();
    replaceOnce(payload, "\"schemaVersion\": 2", "\"schemaVersion\": 3");
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

TEST(DiagInventoryCodec, MissingSchemaVersionFailsClosed) {
    std::string payload = validPayload();
    payload.replace(payload.find("\"schemaVersion\": 1,\n"), 20, "");
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

// ---------------------------------------------------------------------------
// 缺必填字段 / 类型不符 / 非法枚举 / 重复 ecuId / 非 JSON
// ---------------------------------------------------------------------------
TEST(DiagInventoryCodec, MissingVinFailsClosed) {
    std::string payload = validPayload();
    payload.replace(payload.find("\"vin\": \"VINTEST0000000001\",\n"), 28, "");
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

TEST(DiagInventoryCodec, MissingRegistryVersionFailsClosed) {
    std::string payload = validPayload();
    payload.replace(payload.find("\"registryVersion\": \"2026.07.1\",\n"), 33, "");
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

TEST(DiagInventoryCodec, UnknownInventorySourceFailsClosed) {
    std::string payload = validPayload();
    payload.replace(payload.find("\"hardware\""), 10, "\"mixed\"");
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

TEST(DiagInventoryCodec, UnknownOverallResultFailsClosed) {
    std::string payload = validPayload();
    payload.replace(payload.find("\"all_ok\""), 8, "\"unknown_state\"");
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

TEST(DiagInventoryCodec, UnknownEcuStatusFailsClosed) {
    std::string payload = validPayload();
    payload.replace(payload.find("\"status\":\"ok\""), 13, "\"status\":\"weird\"");
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

TEST(DiagInventoryCodec, MissingEcuIdFailsClosed) {
    const std::string payload = R"({
      "schemaVersion": 1,
      "inventorySource": "hardware",
      "vin": "VINTEST0000000004",
      "registryVersion": "REG-1",
      "overallResult": "all_ok",
      "ecuList": [{"partNumber":"PN-X","swVersion":"1.0.0"}]
    })";
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

TEST(DiagInventoryCodec, DuplicateEcuIdFailsClosed) {
    const std::string payload = R"({
      "schemaVersion": 1,
      "inventorySource": "hardware",
      "vin": "VINTEST0000000005",
      "registryVersion": "REG-1",
      "overallResult": "all_ok",
      "ecuList": [
        {"ecuId":"BMS","swVersion":"1.0.0"},
        {"ecuId":"BMS","swVersion":"2.0.0"}
      ]
    })";
    EXPECT_EQ(decodeError(payload), ota::inventory_error::kDiagPayloadInvalid);
}

TEST(DiagInventoryCodec, NonJsonAndEmptyPayloadFailClosed) {
    EXPECT_EQ(decodeError("not-json"), ota::inventory_error::kDiagPayloadInvalid);
    EXPECT_EQ(decodeError(""), ota::inventory_error::kDiagPayloadInvalid);
    EXPECT_EQ(decodeError("[]"), ota::inventory_error::kDiagPayloadInvalid);
}
