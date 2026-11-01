// =============================================================================
// tests/test_inventory_fingerprint.cpp
// CCU-FOTA 业务摘要单元测试 (CCU-FOTA-DSN-CR-006 §10.1-10.5 / §测试设计)
// 覆盖：golden digest（与独立 Python 参考实现互校）、ECU 排序扰动、字段参与/排除、
//       snapshot 稳定性、identifierDigest 域隔离与归一化、dedupeKey、重复稳定键
//       fail-closed、指纹格式。
// =============================================================================

#include "ccu/fota/fingerprint/inventory_fingerprint.hpp"
#include "ccu/fota/fingerprint/canonical_encoder.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace ccu_fota;
using namespace ccu_fota::fingerprint;

namespace {

// 与 /tmp/golden_fingerprint.py 完全一致的固定快照（golden 来源）。
VehicleSoftwareSnapshot makeGoldenSnapshot() {
    VehicleSoftwareSnapshot s;
    s.vin = "LSJAAAAAAAAAAAAAA";
    s.vin_source = VinSource::PROVISIONED;
    s.baseline_id = "bl-001";
    s.baseline_source = BaselineSource::FACTORY;
    s.registry_version = "1.0.0";
    s.collected_at = "2026-08-07T10:00:00Z";
    s.overall_result = CollectionStatus::ALL_OK;
    s.snapshot_seq = 42;

    EcuVersionEntry e1;
    e1.ecu_id = "ECU1";
    e1.part_number = "P001";
    e1.sw_version = "1.2.3";
    e1.hw_version = "HW1";
    e1.source = VersionSource::UDS_0x22;
    e1.status = EcuStatus::OK;

    EcuVersionEntry e2;
    e2.ecu_id = "ECU2";
    e2.part_number = "P002";
    e2.sw_version = "2.0";
    e2.source = VersionSource::SOMEIP_GET_VERSION;
    e2.status = EcuStatus::OK;

    s.ecu_list = {e1, e2};
    return s;
}

bool isLowerHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

} // namespace

// ===========================================================================
// Golden digest（独立 Python 参考实现互校验）
// ===========================================================================
TEST(InventoryFingerprintTest, VersionFingerprintGolden) {
    auto s = makeGoldenSnapshot();
    Fingerprint fp = buildVersionFingerprint(s);
    EXPECT_EQ(fp.algorithm, "sha-256");
    EXPECT_EQ(fp.canonicalization, VERSION_DOMAIN);
    EXPECT_TRUE(isLowerHex64(fp.hex()));
    EXPECT_EQ(fp.hex(), "2a863b8ec731ff205006b4aa712606cbaca56d00a36348106acf5254aebdfcf0");
}

TEST(InventoryFingerprintTest, SnapshotFingerprintGolden) {
    auto s = makeGoldenSnapshot();
    Fingerprint fp = buildSnapshotFingerprint(s);
    EXPECT_EQ(fp.algorithm, "sha-256");
    EXPECT_EQ(fp.canonicalization, SNAPSHOT_DOMAIN);
    EXPECT_TRUE(isLowerHex64(fp.hex()));
    EXPECT_EQ(fp.hex(), "525192dc0b08b947123b3364e0ee2c4b49a39c81563d73fecb553f49cb87371b");
}

TEST(InventoryFingerprintTest, DedupeKeyGolden) {
    auto s = makeGoldenSnapshot();
    Fingerprint sf = buildSnapshotFingerprint(s);
    std::string key = buildDedupeKey(sf, DESTINATION_TBOX_SOFTWARE_INVENTORY);
    EXPECT_TRUE(isLowerHex64(key));
    EXPECT_EQ(key, "cb06b60e7425cf987c936350e5ac9f2f882015b85544636d57f33ba11aabe7c3");
}

TEST(InventoryFingerprintTest, IdentifierDigestVinGolden) {
    std::string d = buildIdentifierDigest(IdentifierKind::Vin, "LSJAAAAAAAAAAAAAA");
    EXPECT_TRUE(isLowerHex64(d));
    EXPECT_EQ(d, "33395e726f50d9e4bf89386d7e82c6bd881609a0dbaf8005ba5024562379d00f");
}

TEST(InventoryFingerprintTest, IdentifierDigestDeviceSnGolden) {
    std::string d = buildIdentifierDigest(IdentifierKind::DeviceSn, "SN12345");
    EXPECT_EQ(d, "86cc1487d60bc8517deb85ea8f496851ddbd5d9304897348b259e9537afcfd00");
}

// ===========================================================================
// VIN 大写归一化：在 buildSnapshotFingerprint 内完成（buildIdentifierDigest 接收
// 已归一化值）。小写 VIN 与对应大写 VIN 产生相同快照指纹。
// ===========================================================================
TEST(InventoryFingerprintTest, SnapshotFingerprintVinNormalization) {
    auto sUpper = makeGoldenSnapshot();
    auto sLower = makeGoldenSnapshot();
    sLower.vin = "lsjaaaaaaaaaaaaaa";  // 小写
    EXPECT_EQ(buildSnapshotFingerprint(sUpper).hex(),
              buildSnapshotFingerprint(sLower).hex());
}

// ===========================================================================
// buildIdentifierDigest 不做 locale case-fold：接收已归一化值，大小写不同 -> 不同摘要
// ===========================================================================
TEST(InventoryFingerprintTest, IdentifierDigestNoLocaleCaseFold) {
    EXPECT_NE(buildIdentifierDigest(IdentifierKind::Vin, "ABC"),
              buildIdentifierDigest(IdentifierKind::Vin, "abc"));
}

// 域隔离：vin 与 device_sn 不同域，相同原值也不同摘要
TEST(InventoryFingerprintTest, IdentifierDigestDomainIsolation) {
    std::string vin = buildIdentifierDigest(IdentifierKind::Vin, "SAMEVALUE");
    std::string sn = buildIdentifierDigest(IdentifierKind::DeviceSn, "SAMEVALUE");
    EXPECT_NE(vin, sn);
}

// ===========================================================================
// 确定性：相同输入两次调用结果一致
// ===========================================================================
TEST(InventoryFingerprintTest, Determinism) {
    auto s = makeGoldenSnapshot();
    EXPECT_EQ(buildVersionFingerprint(s).hex(), buildVersionFingerprint(s).hex());
    EXPECT_EQ(buildSnapshotFingerprint(s).hex(), buildSnapshotFingerprint(s).hex());
}

// ===========================================================================
// ECU 排序扰动：打乱 ECU 顺序，指纹不变
// ===========================================================================
TEST(InventoryFingerprintTest, EcuOrderingPerturbation) {
    auto s = makeGoldenSnapshot();
    std::string vOrdered = buildVersionFingerprint(s).hex();
    std::string sOrdered = buildSnapshotFingerprint(s).hex();

    // 反转 ECU 顺序
    std::swap(s.ecu_list[0], s.ecu_list[1]);
    EXPECT_EQ(buildVersionFingerprint(s).hex(), vOrdered);
    EXPECT_EQ(buildSnapshotFingerprint(s).hex(), sOrdered);
}

// ===========================================================================
// versionFingerprint 字段敏感性：版本字段变化必然改变指纹
// ===========================================================================
TEST(InventoryFingerprintTest, VersionFieldSensitivity) {
    auto s = makeGoldenSnapshot();
    std::string base = buildVersionFingerprint(s).hex();

    auto s2 = s;
    s2.ecu_list[0].sw_version = "1.2.4";  // 软件版本变化
    EXPECT_NE(buildVersionFingerprint(s2).hex(), base);

    auto s3 = s;
    s3.registry_version = "2.0.0";
    EXPECT_NE(buildVersionFingerprint(s3).hex(), base);

    auto s4 = s;
    s4.baseline_id = "bl-002";
    EXPECT_NE(buildVersionFingerprint(s4).hex(), base);
}

// ===========================================================================
// versionFingerprint 排除瞬时字段：status/source/errorCode/overallResult/
// collectedAt/snapshotSeq 变化不改变版本指纹
// ===========================================================================
TEST(InventoryFingerprintTest, VersionExcludesTransientFields) {
    auto s = makeGoldenSnapshot();
    std::string base = buildVersionFingerprint(s).hex();

    auto s2 = s;
    s2.ecu_list[0].status = EcuStatus::TIMEOUT;       // 瞬时不可达
    s2.ecu_list[0].error_code = "NRC13";
    s2.ecu_list[0].source = VersionSource::SOMEIP_GET_VERSION;
    s2.overall_result = CollectionStatus::PARTIAL;
    s2.collected_at = "2026-09-01T00:00:00Z";
    s2.snapshot_seq = 999;
    EXPECT_EQ(buildVersionFingerprint(s2).hex(), base)
        << "versionFingerprint must exclude transient fields";
}

// ===========================================================================
// snapshotFingerprint 排除时间/序号：collectedAt/snapshotSeq 变化指纹不变
// ===========================================================================
TEST(InventoryFingerprintTest, SnapshotExcludesTimeAndSeq) {
    auto s = makeGoldenSnapshot();
    std::string base = buildSnapshotFingerprint(s).hex();

    auto s2 = s;
    s2.collected_at = "1999-01-01T00:00:00Z";
    s2.snapshot_seq = 0;
    EXPECT_EQ(buildSnapshotFingerprint(s2).hex(), base);
}

// ===========================================================================
// snapshotFingerprint 包含 overallResult/status/source：语义变化改变指纹
// ===========================================================================
TEST(InventoryFingerprintTest, SnapshotIncludesSemantics) {
    auto s = makeGoldenSnapshot();
    std::string base = buildSnapshotFingerprint(s).hex();

    auto s2 = s;
    s2.overall_result = CollectionStatus::PARTIAL;
    EXPECT_NE(buildSnapshotFingerprint(s2).hex(), base);

    auto s3 = s;
    s3.ecu_list[0].status = EcuStatus::NRC;
    EXPECT_NE(buildSnapshotFingerprint(s3).hex(), base);

    auto s4 = s;
    s4.ecu_list[0].source = VersionSource::SOMEIP_GET_VERSION;
    EXPECT_NE(buildSnapshotFingerprint(s4).hex(), base);
}

// ===========================================================================
// snapshotFingerprint VIN 敏感性：VIN 变化（经 identifierDigest）改变指纹
// ===========================================================================
TEST(InventoryFingerprintTest, SnapshotVinSensitivity) {
    auto s = makeGoldenSnapshot();
    std::string base = buildSnapshotFingerprint(s).hex();

    auto s2 = s;
    s2.vin = "LSJBBBBBBBBBBBBBB";
    EXPECT_NE(buildSnapshotFingerprint(s2).hex(), base);
}

// ===========================================================================
// dedupeKey：目标链路敏感性、snapshot 敏感性、相同输入稳定
// ===========================================================================
TEST(InventoryFingerprintTest, DedupeKeyProperties) {
    auto s = makeGoldenSnapshot();
    Fingerprint sf = buildSnapshotFingerprint(s);
    std::string key = buildDedupeKey(sf, DESTINATION_TBOX_SOFTWARE_INVENTORY);

    // 稳定
    EXPECT_EQ(buildDedupeKey(sf, DESTINATION_TBOX_SOFTWARE_INVENTORY), key);

    // 目标链路变化 -> 不同
    EXPECT_NE(buildDedupeKey(sf, "other-destination"), key);

    // snapshot 变化 -> 不同
    auto s2 = s;
    s2.ecu_list[0].sw_version = "9.9.9";
    Fingerprint sf2 = buildSnapshotFingerprint(s2);
    EXPECT_NE(buildDedupeKey(sf2, DESTINATION_TBOX_SOFTWARE_INVENTORY), key);
}

// ===========================================================================
// 重复稳定键：相同 (ecuId, partNumber, swVersion) -> CanonicalError (fail-closed)
// ===========================================================================
TEST(InventoryFingerprintTest, DuplicateEcuStableKeyFails) {
    auto s = makeGoldenSnapshot();
    s.ecu_list.clear();

    EcuVersionEntry a;
    a.ecu_id = "ECU1";
    a.part_number = "P1";
    a.sw_version = "1";
    a.hw_version = "HW1";
    EcuVersionEntry b = a;
    b.hw_version = "HW2";  // 仅硬件版本不同，稳定键仍重复
    s.ecu_list = {a, b};

    EXPECT_THROW(buildVersionFingerprint(s), CanonicalError);
    EXPECT_THROW(buildSnapshotFingerprint(s), CanonicalError);
}

// ===========================================================================
// 指纹格式：digest 32 字节，hex 64 字符
// ===========================================================================
TEST(InventoryFingerprintTest, FingerprintFormat) {
    auto s = makeGoldenSnapshot();
    Fingerprint vf = buildVersionFingerprint(s);
    Fingerprint sf = buildSnapshotFingerprint(s);
    EXPECT_EQ(vf.digest.size(), 32u);
    EXPECT_EQ(sf.digest.size(), 32u);
    EXPECT_EQ(vf.hex().size(), 64u);
    EXPECT_EQ(sf.hex().size(), 64u);
}
