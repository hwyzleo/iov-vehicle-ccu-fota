// =============================================================================
// tests/ota/inventory_provider_test.cpp
// 生产清单提供者单元测试 (CCU-FOTA-DSN-CR-012 §规范化 / D7 / D2
//                            / CCU-FOTA-DSN-CR-014 D2/D3/D4)
// -----------------------------------------------------------------------------
// 覆盖：四类字段精确映射、SINGLE_IMAGE 必需事实 fail-closed（不进入 planner）、
// ECU 稳定排序（顺序扰动不改 v2 摘要、完整键重复 fail-closed）、wire v2 域、
// 采集阶段不分配序号（Defer）。
// =============================================================================

#include "ccu/fota/ota/ports/production_ports.hpp"

#include "ccu/fota/ota/inventory_error_codes.hpp"
#include "ccu/fota/ota/mock/mock_ports.hpp"
#include "snapshot_assembler.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace ccu_fota;
using namespace ccu_fota::ota;

namespace {

// 测试用快照源：按给定顺序返回 ECU 列表（不触碰 DIAG）。
class StubSnapshotAssembler : public SnapshotAssembler {
public:
    StubSnapshotAssembler() : SnapshotAssembler(nullptr) {}

    void setEcuList(std::vector<EcuVersionEntry> list, bool ok = true) {
        list_ = std::move(list);
        ok_ = ok;
    }

    bool assembleSnapshot(VehicleSoftwareSnapshot& snapshot) override {
        return assembleSnapshot(snapshot, SeqPolicy::Allocate);
    }

    bool assembleSnapshot(VehicleSoftwareSnapshot& snapshot, SeqPolicy policy) override {
        policySeen_ = policy;
        if (!ok_) return false;
        snapshot = VehicleSoftwareSnapshot{};
        snapshot.vin = "VINTEST0000000001";
        snapshot.baseline_id = "BASE-001";
        snapshot.registry_version = "REG-1";
        snapshot.overall_result = CollectionStatus::ALL_OK;
        snapshot.ecu_list = list_;
        // Defer：不分配序号（保持 0）；Allocate：模拟分配。
        snapshot.snapshot_seq = (policy == SeqPolicy::Defer) ? 0 : 99;
        return true;
    }

    SeqPolicy policySeen() const { return policySeen_; }

private:
    std::vector<EcuVersionEntry> list_;
    bool ok_ = true;
    SeqPolicy policySeen_ = SeqPolicy::Allocate;
};

// CR-014 D2：四类字段取值互不相同，任何交叉映射都会被下面的断言捕获。
EcuVersionEntry ecu(const std::string& id, const std::string& sw,
                    const std::string& hw = "HWV-DEFAULT") {
    EcuVersionEntry e;
    e.ecu_id = id;
    e.hw_version = hw;                     // 硬件版本
    e.part_number = "HT-" + id;            // 总成/备件件号
    e.software_part_number = "SW-" + id;   // 软件件号
    e.sw_version = sw;
    e.status = EcuStatus::OK;
    return e;
}

// golden 夹具（与 scripts/golden_fingerprint.py 的 ECUS 完全一致）。
EcuVersionEntry goldenEcu1() {
    EcuVersionEntry e;
    e.ecu_id = "ECU1";
    e.part_number = "P001";
    e.software_part_number = "SP001";
    e.sw_version = "1.2.3";
    e.hw_version = "HW1";
    e.source = VersionSource::UDS_0x22;
    e.status = EcuStatus::OK;
    return e;
}

EcuVersionEntry goldenEcu2() {
    EcuVersionEntry e;
    e.ecu_id = "ECU2";
    e.part_number = "P002";
    e.software_part_number = "SP002";
    e.sw_version = "2.0";
    e.source = VersionSource::SOMEIP_GET_VERSION;
    e.status = EcuStatus::OK;
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
// 顺序扰动：内容相同 -> digest 与指纹一致（不得回退输入顺序）
// ---------------------------------------------------------------------------
TEST(InventoryProviderTest, StableSortKeepsDigestAndFingerprint) {
    auto assembler = std::make_shared<StubSnapshotAssembler>();
    InventoryProviderFromAssembler provider(assembler, "1.0.0");

    assembler->setEcuList({ecu("VCU", "1.0.0"), ecu("BMS", "2.0.0"),
                           ecu("MCU", "3.0.0")});
    auto a = provider.collectInventory();
    ASSERT_TRUE(a.collected);

    assembler->setEcuList({ecu("MCU", "3.0.0"), ecu("VCU", "1.0.0"),
                           ecu("BMS", "2.0.0")});
    auto b = provider.collectInventory();
    ASSERT_TRUE(b.collected);

    EXPECT_EQ(a.ecuListDigest.value_hex(), b.ecuListDigest.value_hex());
    EXPECT_EQ(a.fingerprintHex, b.fingerprintHex);
    EXPECT_EQ(a.fingerprintCanonicalization, "cgw-fota-version-v2");
    // CR-014 D5：wire 域随身份持久化，升 v2。
    EXPECT_EQ(a.wireCanonicalization, "inventory-ecu-list-v2");
    EXPECT_EQ(a.ecuListDigest.algorithm(), "sha-256");
    ASSERT_EQ(a.ecuList.size(), 3u);
    EXPECT_EQ(a.ecuList[0].ecu_id(), "BMS");  // 稳定排序结果

    // 内容变化 -> digest 变化
    assembler->setEcuList({ecu("VCU", "9.9.9"), ecu("BMS", "2.0.0"),
                           ecu("MCU", "3.0.0")});
    auto c = provider.collectInventory();
    ASSERT_TRUE(c.collected);
    EXPECT_NE(a.ecuListDigest.value_hex(), c.ecuListDigest.value_hex());
    EXPECT_NE(a.fingerprintHex, c.fingerprintHex);
}

// ---------------------------------------------------------------------------
// CR-014 D2：Proto 四类字段逐项精确映射（禁止交叉赋值）
// ---------------------------------------------------------------------------
TEST(InventoryProviderTest, MapsFourFactsOneToOne) {
    auto assembler = std::make_shared<StubSnapshotAssembler>();
    InventoryProviderFromAssembler provider(assembler, "1.0.0");

    auto src = ecu("VCU", "1.0.0", "HWV-VCU");
    assembler->setEcuList({src});
    auto inv = provider.collectInventory();
    ASSERT_TRUE(inv.collected) << inv.collectionErrorCode << " "
                               << inv.collectionErrorDetail;
    ASSERT_EQ(inv.ecuList.size(), 1u);

    const auto& ev = inv.ecuList[0];
    EXPECT_EQ(ev.ecu_id(), "VCU");
    EXPECT_EQ(ev.hardware_part_number(), "HT-VCU");   // partNumber
    EXPECT_EQ(ev.software_part_number(), "SW-VCU");   // softwarePartNumber
    EXPECT_EQ(ev.sw_version(), "1.0.0");              // swVersion
    EXPECT_EQ(ev.hw_version(), "HWV-VCU");            // hwVersion

    // 交叉映射反例：硬件件号不得等于硬件版本，软件件号不得等于总成件号。
    EXPECT_NE(ev.hardware_part_number(), src.hw_version.value());
    EXPECT_NE(ev.software_part_number(), src.part_number.value());
}

// ---------------------------------------------------------------------------
// CR-014 D3：SINGLE_IMAGE 必需事实缺失 -> collected=false + 可定位错误码；
// 细节只含字段名与 ECU 索引，不含件号/版本原值（不进入 planner）。
// ---------------------------------------------------------------------------
TEST(InventoryProviderTest, MissingRequiredFactFailsClosed) {
    auto assembler = std::make_shared<StubSnapshotAssembler>();
    InventoryProviderFromAssembler provider(assembler, "1.0.0");

    // 1) 缺少软件件号（DIAG payload v1 的典型形态）：不得用 partNumber 兜底。
    auto noSoftwarePn = ecu("BMS", "1.0.0");
    noSoftwarePn.software_part_number.reset();
    assembler->setEcuList({ecu("VCU", "1.0.0"), noSoftwarePn});
    auto missingPn = provider.collectInventory();
    EXPECT_FALSE(missingPn.collected);
    EXPECT_TRUE(missingPn.ecuList.empty());
    EXPECT_EQ(missingPn.collectionErrorCode,
              ota::inventory_error::kRequiredFactMissing);
    EXPECT_EQ(missingPn.collectionErrorDetail,
              "ecuList[1].softwarePartNumber");
    // 采集失败 => 无指纹/digest，调用方不得进入 InventoryIdentityPlanner。
    EXPECT_TRUE(missingPn.fingerprintHex.empty());
    EXPECT_TRUE(missingPn.ecuListDigest.value_hex().empty());

    // 2) 缺少软件版本
    assembler->setEcuList({ecu("BMS", "")});
    auto missingSw = provider.collectInventory();
    EXPECT_FALSE(missingSw.collected);
    EXPECT_EQ(missingSw.collectionErrorCode,
              ota::inventory_error::kRequiredFactMissing);
    EXPECT_EQ(missingSw.collectionErrorDetail, "ecuList[0].swVersion");

    // 3) 缺少 ecuId
    assembler->setEcuList({ecu("", "1.0.0")});
    auto missingId = provider.collectInventory();
    EXPECT_FALSE(missingId.collected);
    EXPECT_EQ(missingId.collectionErrorDetail, "ecuList[0].ecuId");

    // 4) 错误细节不含件号或版本原值（脱敏）
    EXPECT_EQ(missingPn.collectionErrorDetail.find("SW-"), std::string::npos);
    EXPECT_EQ(missingSw.collectionErrorDetail.find("HT-"), std::string::npos);
}

// ---------------------------------------------------------------------------
// CR-014 D4：完整稳定键重复 -> fail-closed（不以输入顺序兜底）
// ---------------------------------------------------------------------------
TEST(InventoryProviderTest, DuplicateFullStableKeyFailsClosed) {
    auto assembler = std::make_shared<StubSnapshotAssembler>();
    InventoryProviderFromAssembler provider(assembler, "1.0.0");

    auto a = ecu("VCU", "1.0.0");
    auto b = a;  // 五元组完全相同
    assembler->setEcuList({a, b});
    auto inv = provider.collectInventory();
    EXPECT_FALSE(inv.collected);
    EXPECT_FALSE(inv.collectionErrorCode.empty());

    // 仅硬件版本不同 => 完整键不重复 => 可上报。
    assembler->setEcuList({a, ecu("VCU", "1.0.0", "HWV-OTHER")});
    auto ok = provider.collectInventory();
    EXPECT_TRUE(ok.collected) << ok.collectionErrorCode;
}

// ---------------------------------------------------------------------------
// CR-014 D4：wire ecuListDigest v2 golden（与 Python 参考实现互校）
// ---------------------------------------------------------------------------
TEST(InventoryProviderTest, WireDigestGoldenV2) {
    auto assembler = std::make_shared<StubSnapshotAssembler>();
    InventoryProviderFromAssembler provider(assembler, "1.0.0");
    assembler->setEcuList({goldenEcu1(), goldenEcu2()});

    auto inv = provider.collectInventory();
    ASSERT_TRUE(inv.collected);
    EXPECT_EQ(inv.wireCanonicalization, "inventory-ecu-list-v2");
    EXPECT_EQ(inv.fingerprintCanonicalization, "cgw-fota-version-v2");
    EXPECT_EQ(inv.ecuListDigest.value_hex(),
              "00425e380ba9ccbf7ba001d754fb7dd50910643446e76aa89aefae7657ecc4b0");
}

// ---------------------------------------------------------------------------
// wire 摘要覆盖独立软件件号：只改软件件号也必须改变内容身份
// ---------------------------------------------------------------------------
TEST(InventoryProviderTest, WireDigestCoversSoftwarePartNumber) {
    auto assembler = std::make_shared<StubSnapshotAssembler>();
    InventoryProviderFromAssembler provider(assembler, "1.0.0");

    assembler->setEcuList({goldenEcu1(), goldenEcu2()});
    auto a = provider.collectInventory();
    ASSERT_TRUE(a.collected);

    assembler->setEcuList({goldenEcu2(), goldenEcu1()});  // 顺序扰动
    auto b = provider.collectInventory();
    ASSERT_TRUE(b.collected);
    EXPECT_EQ(a.ecuListDigest.value_hex(), b.ecuListDigest.value_hex());

    auto changed = goldenEcu2();
    changed.software_part_number = "SP002-NEW";  // 仅软件件号变化
    assembler->setEcuList({goldenEcu1(), changed});
    auto c = provider.collectInventory();
    ASSERT_TRUE(c.collected);
    EXPECT_NE(a.ecuListDigest.value_hex(), c.ecuListDigest.value_hex());
    EXPECT_NE(a.fingerprintHex, c.fingerprintHex);
}

// ---------------------------------------------------------------------------
// 采集失败 / 空清单：collected=false（不上报、不分配）
// ---------------------------------------------------------------------------
TEST(InventoryProviderTest, FailedOrEmptyCollectionIsNotReportable) {
    auto assembler = std::make_shared<StubSnapshotAssembler>();
    InventoryProviderFromAssembler provider(assembler, "1.0.0");

    assembler->setEcuList({}, /*ok=*/false);
    auto failed = provider.collectInventory();
    EXPECT_FALSE(failed.collected);
    EXPECT_TRUE(failed.ecuList.empty());

    assembler->setEcuList({}, /*ok=*/true);
    auto empty = provider.collectInventory();
    EXPECT_FALSE(empty.collected);
    EXPECT_TRUE(empty.ecuList.empty());

    // 无采集源（nullptr assembler）同样不可上报。
    InventoryProviderFromAssembler no_source(nullptr, "1.0.0");
    EXPECT_FALSE(no_source.collectInventory().collected);
}

// ---------------------------------------------------------------------------
// 采集阶段使用 Defer：不分配序号
// ---------------------------------------------------------------------------
TEST(InventoryProviderTest, CollectionUsesDeferSeqPolicy) {
    auto assembler = std::make_shared<StubSnapshotAssembler>();
    InventoryProviderFromAssembler provider(assembler, "1.0.0");
    assembler->setEcuList({ecu("VCU", "1.0.0")});

    auto inv = provider.collectInventory();
    ASSERT_TRUE(inv.collected);
    EXPECT_EQ(assembler->policySeen(), SeqPolicy::Defer);
}

// ---------------------------------------------------------------------------
// CR-014：Mock 与生产 InventoryProvider 遵循同一字段语义与双域常量
// （Mock 不得因旧映射而丢掉某个字段或使用旧域）。
// ---------------------------------------------------------------------------
TEST(InventoryProviderTest, MockAndProductionShareFieldSemantics) {
    auto assembler = std::make_shared<StubSnapshotAssembler>();
    InventoryProviderFromAssembler production(assembler, "1.0.0");
    assembler->setEcuList({ecu("VCU-001", "1.0.0", "HWV-VCU-001")});
    auto prod = production.collectInventory();
    ASSERT_TRUE(prod.collected);

    auto scenario = mock::parseScenario(R"({
      "scenario": "field-semantics",
      "inventory": {"mode": "FULL", "baseline": "BASE-001",
                    "fota_master_version": "1.0.0"},
      "packages": [], "executor": {"stages": []}, "faults": []
    })");
    mock::MockInventoryProvider mockProvider(scenario);
    auto m = mockProvider.collectInventory();
    ASSERT_TRUE(m.collected);

    ASSERT_EQ(prod.ecuList.size(), 1u);
    ASSERT_EQ(m.ecuList.size(), 1u);
    // 四类字段均非空且互不相同（两种实现均如此）。
    for (const auto* list : {&prod.ecuList, &m.ecuList}) {
        const auto& ev = list->at(0);
        EXPECT_FALSE(ev.hardware_part_number().empty());
        EXPECT_FALSE(ev.software_part_number().empty());
        EXPECT_FALSE(ev.sw_version().empty());
        EXPECT_FALSE(ev.hw_version().empty());
        EXPECT_NE(ev.hardware_part_number(), ev.software_part_number());
        EXPECT_NE(ev.hardware_part_number(), ev.hw_version());
    }
    // 双域常量一致（身份可比较的前提）。
    EXPECT_EQ(m.wireCanonicalization, prod.wireCanonicalization);
    EXPECT_EQ(m.fingerprintCanonicalization, prod.fingerprintCanonicalization);
    EXPECT_EQ(m.wireCanonicalization, "inventory-ecu-list-v2");
}
