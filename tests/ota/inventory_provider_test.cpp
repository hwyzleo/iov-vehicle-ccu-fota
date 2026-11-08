// =============================================================================
// tests/ota/inventory_provider_test.cpp
// 生产清单提供者单元测试 (CCU-FOTA-DSN-CR-012 §规范化 / D7 / D2)
// -----------------------------------------------------------------------------
// 覆盖：ECU 列表稳定排序（顺序扰动不改变 digest/指纹）、空清单不上报、
// 采集阶段不分配序号（Defer）。
// =============================================================================

#include "ccu/fota/ota/ports/production_ports.hpp"

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

EcuVersionEntry ecu(const std::string& id, const std::string& sw,
                    const std::string& hw = "P-HW") {
    EcuVersionEntry e;
    e.ecu_id = id;
    e.hw_version = hw;
    e.part_number = "S-PART";
    e.sw_version = sw;
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
    EXPECT_EQ(a.fingerprintCanonicalization, "cgw-fota-version-v1");
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
