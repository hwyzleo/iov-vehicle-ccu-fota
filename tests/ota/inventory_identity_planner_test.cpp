// =============================================================================
// tests/ota/inventory_identity_planner_test.cpp
// 清单身份决策器单元测试 (CCU-FOTA-DSN-CR-012 §身份决策规则)
// =============================================================================

#include "ccu/fota/ota/inventory_identity_planner.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"
#include "ccu/fota/store/fota_state_store.hpp"

#include <ccu/fw/hash/sha256.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace ccu_fota::ota;
using namespace ccu_fota::store;
using namespace ccu_fota::store::fota;
using ::vehicle::fota::v1::INVENTORY_MODE_DIGEST;
using ::vehicle::fota::v1::INVENTORY_MODE_FULL;
using ::vehicle::fota::v1::INVENTORY_DISPOSITION_ALGORITHM_UNSUPPORTED;
using ::vehicle::fota::v1::INVENTORY_DISPOSITION_DIGEST_MISMATCH;
using ::vehicle::fota::v1::INVENTORY_DISPOSITION_FULL_REQUIRED;
using ::vehicle::fota::v1::INVENTORY_DISPOSITION_REVISION_CONFLICT;

namespace {

fs::path makeUniqueRoot() {
    static int counter = 0;
    auto root = fs::temp_directory_path() /
                ("fota-inv-id-" + std::to_string(getpid()) + "-" +
                 std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

// 与生产同构的稳定内容：内容标识 contentTag 变化 → 指纹/digest 变化。
CollectedInventory makeInventory(const std::string& contentTag,
                                 const std::string& swVersion = "1.0.0") {
    CollectedInventory inv;
    ::vehicle::fota::v1::EcuVersion e;
    e.set_ecu_id("VCU-001");
    e.set_hardware_part_number("P-VCU-001");
    e.set_software_part_number("S-VCU-001");
    e.set_sw_version(swVersion);
    inv.ecuList.push_back(e);
    inv.baselineCode = "BASE-001";
    inv.fotaMasterVersion = "1.0.0";
    inv.collectedAtMs = 1000;
    inv.fingerprintCanonicalization = "cgw-fota-version-v1";
    inv.fingerprintHex = ccu::fw::hash::sha256_hex("fp|" + contentTag);
    inv.ecuListDigest.set_algorithm("sha-256");
    inv.ecuListDigest.set_value_hex(ccu::fw::hash::sha256_hex("digest|" + contentTag));
    inv.collected = true;
    return inv;
}

struct PlannerHarness {
    fs::path root;
    std::shared_ptr<FotaStateStore> fotaStore;
    std::unique_ptr<FotaCloudStateStore> cloudStore;
    std::unique_ptr<InventoryIdentityPlanner> planner;
    std::uint64_t allocCalls = 0;

    explicit PlannerHarness(InventoryIdentityConfig cfg = {}) {
        root = makeUniqueRoot();
        auto s = FotaStateStore::open(root, 100, 3600000);
        fotaStore = std::make_shared<FotaStateStore>(std::move(s));
        cloudStore = std::make_unique<FotaCloudStateStore>(fotaStore->underlyingStore());
        planner = std::make_unique<InventoryIdentityPlanner>(
            *cloudStore,
            [this]() {
                ++allocCalls;
                return fotaStore->allocateSnapshotSeq();
            },
            cfg);
    }

    // 模拟一次完整“决策 -> PREPARED -> ACCEPTED”。
    void acceptCycle(const CollectedInventory& inv) {
        auto id = planner->resolve(inv);
        auto rec = planner->markPrepared(inv, id);
        ::vehicle::fota::v1::TaskCheckResponse resp;
        resp.set_inventory_disposition(
            ::vehicle::fota::v1::INVENTORY_DISPOSITION_ACCEPTED);
        resp.set_accepted_inventory_revision(rec.inventoryRevision);
        resp.add_supported_inventory_digest_algorithms("sha-256");
        planner->onAccepted(id, resp);
    }
};

std::uint64_t highestSeq(FotaStateStore& s) {
    auto seq = s.loadSequence();
    return seq.has_value() ? seq->highestAllocated : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 规则 1：无记录 -> FULL + 新 revision（建立基线）
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, FirstBaselineAllocatesRevision) {
    PlannerHarness h;
    auto inv = makeInventory("v1");

    auto id = h.planner->resolve(inv);
    EXPECT_EQ(id.mode, INVENTORY_MODE_FULL);
    EXPECT_TRUE(id.freshRevision);
    EXPECT_EQ(id.reason, InventoryChangeReason::FirstBaseline);

    auto rec = h.planner->markPrepared(inv, id);
    EXPECT_EQ(rec.status, "PREPARED");
    EXPECT_EQ(rec.mode, "FULL");
    EXPECT_EQ(rec.inventoryRevision, 1u);
    EXPECT_EQ(rec.attempt, 1u);
    EXPECT_EQ(rec.fingerprintHex, inv.fingerprintHex);
    EXPECT_EQ(highestSeq(*h.fotaStore), 1u);
}

// ---------------------------------------------------------------------------
// 规则 6：内容未变化 -> 复用 revision + DIGEST（不新增版本）
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, UnchangedContentReusesRevisionAndSendsDigest) {
    PlannerHarness h;
    auto inv = makeInventory("v1");
    h.acceptCycle(inv);

    for (int i = 0; i < 5; ++i) {
        auto id = h.planner->resolve(inv);
        EXPECT_EQ(id.mode, INVENTORY_MODE_DIGEST);
        EXPECT_FALSE(id.freshRevision);
        EXPECT_EQ(id.revision, 1u);
        EXPECT_EQ(id.reason, InventoryChangeReason::ContentReused);
        auto rec = h.planner->markPrepared(inv, id);
        EXPECT_EQ(rec.inventoryRevision, 1u);
        // 尝试次数随同一身份重试递增，但不产生新版本。
        EXPECT_EQ(rec.attempt, static_cast<std::uint32_t>(i + 2));
        // 模拟云端接受（否则下一轮会视为 PREPARED 重试）。
        ::vehicle::fota::v1::TaskCheckResponse resp;
        resp.set_inventory_disposition(
            ::vehicle::fota::v1::INVENTORY_DISPOSITION_ACCEPTED);
        resp.set_accepted_inventory_revision(rec.inventoryRevision);
        h.planner->onAccepted(id, resp);
    }
    EXPECT_EQ(highestSeq(*h.fotaStore), 1u);
}

// ---------------------------------------------------------------------------
// 规则 7：内容变化 -> FULL + 新 revision
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, ContentChangeAllocatesNewRevision) {
    PlannerHarness h;
    h.acceptCycle(makeInventory("v1"));

    auto inv2 = makeInventory("v2", "2.0.0");
    auto id = h.planner->resolve(inv2);
    EXPECT_EQ(id.mode, INVENTORY_MODE_FULL);
    EXPECT_TRUE(id.freshRevision);
    EXPECT_EQ(id.reason, InventoryChangeReason::ContentChanged);

    auto rec = h.planner->markPrepared(inv2, id);
    EXPECT_EQ(rec.inventoryRevision, 2u);
    EXPECT_EQ(highestSeq(*h.fotaStore), 2u);
}

// ---------------------------------------------------------------------------
// 规则 2：PREPARED（结果未知）-> 同身份重试
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, PreparedRetriesSameIdentity) {
    PlannerHarness h(InventoryIdentityConfig{std::chrono::milliseconds(5000), 3, true});
    auto inv = makeInventory("v1");
    auto id = h.planner->resolve(inv);
    auto rec = h.planner->markPrepared(inv, id);

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    h.planner->onTransportFailure(id, "FOTA-INVENTORY-TRANSPORT");

    auto retry = h.planner->resolve(inv);
    EXPECT_EQ(retry.reason, InventoryChangeReason::Retry);
    EXPECT_FALSE(retry.freshRevision);
    EXPECT_EQ(retry.revision, rec.inventoryRevision);
    EXPECT_EQ(retry.mode, INVENTORY_MODE_FULL);   // 沿用 PREPARED 的 mode
    EXPECT_FALSE(h.planner->retryReady(now));     // 退避未到
    EXPECT_TRUE(h.planner->retryReady(now + 6000));
    EXPECT_EQ(highestSeq(*h.fotaStore), 1u);      // 失败不消耗新版本
}

// PREPARED 期间内容变化 -> 旧身份弃用，新 revision
TEST(InventoryIdentityPlanner, PreparedWithChangedContentAllocatesNewRevision) {
    PlannerHarness h;
    auto inv = makeInventory("v1");
    auto id = h.planner->resolve(inv);
    h.planner->markPrepared(inv, id);

    auto inv2 = makeInventory("v2", "2.0.0");
    auto next = h.planner->resolve(inv2);
    EXPECT_EQ(next.reason, InventoryChangeReason::ContentChanged);
    EXPECT_TRUE(next.freshRevision);
    auto rec = h.planner->markPrepared(inv2, next);
    EXPECT_EQ(rec.inventoryRevision, 2u);
}

// ---------------------------------------------------------------------------
// 规则 4：FULL_REQUIRED -> FULL 复用 revision
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, FullRequiredReusesRevision) {
    PlannerHarness h;
    auto inv = makeInventory("v1");
    auto id = h.planner->resolve(inv);
    h.planner->markPrepared(inv, id);

    ::vehicle::fota::v1::TaskCheckResponse resp;
    resp.set_inventory_disposition(INVENTORY_DISPOSITION_FULL_REQUIRED);
    resp.set_required_inventory_mode(INVENTORY_MODE_FULL);
    h.planner->onRejected(id, resp);

    auto rec = h.planner->current();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->status, "REQUIRED_FULL");
    EXPECT_TRUE(rec->fullRequired);
    EXPECT_EQ(rec->requiredMode, "FULL");
    EXPECT_EQ(rec->lastErrorCode, "INVENTORY_DISPOSITION_FULL_REQUIRED");

    auto next = h.planner->resolve(inv);
    EXPECT_EQ(next.mode, INVENTORY_MODE_FULL);
    EXPECT_FALSE(next.freshRevision);
    EXPECT_EQ(next.revision, 1u);
    EXPECT_EQ(next.reason, InventoryChangeReason::FullRequired);
}

// ---------------------------------------------------------------------------
// 规则 3：REVISION_CONFLICT -> 新 revision FULL（旧 revision 弃用）
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, RevisionConflictAllocatesNewRevision) {
    PlannerHarness h;
    auto inv = makeInventory("v1");
    auto id = h.planner->resolve(inv);
    h.planner->markPrepared(inv, id);

    ::vehicle::fota::v1::TaskCheckResponse resp;
    resp.set_inventory_disposition(INVENTORY_DISPOSITION_REVISION_CONFLICT);
    h.planner->onRejected(id, resp);

    auto next = h.planner->resolve(inv);
    EXPECT_EQ(next.mode, INVENTORY_MODE_FULL);
    EXPECT_TRUE(next.freshRevision);
    EXPECT_EQ(next.reason, InventoryChangeReason::Conflict);
    auto rec = h.planner->markPrepared(inv, next);
    EXPECT_EQ(rec.inventoryRevision, 2u);
}

// ---------------------------------------------------------------------------
// DIGEST_MISMATCH -> 同 CONFLICT 路径，并累计 digestFailures
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, DigestMismatchAllocatesNewRevisionAndCounts) {
    PlannerHarness h;
    auto inv = makeInventory("v1");
    h.acceptCycle(inv);

    auto id = h.planner->resolve(inv);
    EXPECT_EQ(id.mode, INVENTORY_MODE_DIGEST);
    h.planner->markPrepared(inv, id);

    ::vehicle::fota::v1::TaskCheckResponse resp;
    resp.set_inventory_disposition(INVENTORY_DISPOSITION_DIGEST_MISMATCH);
    h.planner->onRejected(id, resp);

    auto rec = h.planner->current();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->status, "CONFLICT");
    EXPECT_EQ(rec->digestFailures, 1u);

    auto next = h.planner->resolve(inv);
    EXPECT_TRUE(next.freshRevision);
    EXPECT_EQ(next.reason, InventoryChangeReason::Conflict);
}

// ---------------------------------------------------------------------------
// 规则 5：ALGORITHM_UNSUPPORTED -> 换算法 FULL，revision 保留
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, AlgorithmUnsupportedReusesRevision) {
    PlannerHarness h;
    auto inv = makeInventory("v1");
    auto id = h.planner->resolve(inv);
    h.planner->markPrepared(inv, id);

    ::vehicle::fota::v1::TaskCheckResponse resp;
    resp.set_inventory_disposition(INVENTORY_DISPOSITION_ALGORITHM_UNSUPPORTED);
    resp.add_supported_inventory_digest_algorithms("sha-256");
    h.planner->onRejected(id, resp);

    auto rec = h.planner->current();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->status, "ALGORITHM_UNSUPPORTED");
    EXPECT_EQ(rec->algorithm, "sha-256");

    auto next = h.planner->resolve(inv);
    EXPECT_EQ(next.mode, INVENTORY_MODE_FULL);
    EXPECT_FALSE(next.freshRevision);
    EXPECT_EQ(next.revision, 1u);
    EXPECT_EQ(next.reason, InventoryChangeReason::AlgorithmUnsupported);
}

// ---------------------------------------------------------------------------
// 变化检测关闭：内容变化也固定复用 + DIGEST（不得以新 revision 替代检测）
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, DetectionDisabledNeverGrowsRevision) {
    InventoryIdentityConfig cfg;
    cfg.changeDetectionEnabled = false;
    PlannerHarness h(cfg);

    h.acceptCycle(makeInventory("v1"));
    auto id = h.planner->resolve(makeInventory("v2", "2.0.0"));
    EXPECT_EQ(id.mode, INVENTORY_MODE_DIGEST);
    EXPECT_FALSE(id.freshRevision);
    EXPECT_EQ(id.revision, 1u);
    EXPECT_EQ(id.reason, InventoryChangeReason::DetectionDisabled);
}

// ---------------------------------------------------------------------------
// 连续 DIGEST 失配达上限 -> 不再扩版本，改用云端已接受 revision 重发 FULL
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, DigestFailureCeilingReusesAcceptedRevision) {
    InventoryIdentityConfig cfg;
    cfg.digestMaxConsecutiveFailures = 2;
    PlannerHarness h(cfg);
    auto inv = makeInventory("v1");
    h.acceptCycle(inv);                      // accepted revision = 1

    auto mismatch = [&]() {
        auto id = h.planner->resolve(inv);
        h.planner->markPrepared(inv, id);
        ::vehicle::fota::v1::TaskCheckResponse resp;
        resp.set_inventory_disposition(
            ::vehicle::fota::v1::INVENTORY_DISPOSITION_DIGEST_MISMATCH);
        h.planner->onRejected(id, resp);
        return id;
    };

    auto first = mismatch();                 // DIGEST -> CONFLICT（failures=1）
    EXPECT_EQ(first.mode, INVENTORY_MODE_DIGEST);
    EXPECT_EQ(h.planner->current()->digestFailures, 1u);

    auto second = mismatch();                // CONFLICT -> 新 revision FULL（failures=2）
    EXPECT_EQ(second.mode, INVENTORY_MODE_FULL);
    EXPECT_TRUE(second.freshRevision);
    auto after_second = h.planner->current();
    ASSERT_TRUE(after_second.has_value());
    EXPECT_EQ(after_second->inventoryRevision, 2u);
    EXPECT_EQ(after_second->digestFailures, 2u);

    // 已达上限：不再新分配，复用云端已接受 revision 重发 FULL。
    auto third = h.planner->resolve(inv);
    EXPECT_EQ(third.mode, INVENTORY_MODE_FULL);
    EXPECT_FALSE(third.freshRevision);
    EXPECT_EQ(third.revision, 1u);
    EXPECT_EQ(third.reason, InventoryChangeReason::FullRequired);

    // DIGEST 被接受后计数归零，恢复 DIGEST 复用。
    auto rec = h.planner->markPrepared(inv, third);
    ::vehicle::fota::v1::TaskCheckResponse ok;
    ok.set_inventory_disposition(
        ::vehicle::fota::v1::INVENTORY_DISPOSITION_ACCEPTED);
    ok.set_accepted_inventory_revision(rec.inventoryRevision);
    h.planner->onAccepted(third, ok);
    auto after_accept = h.planner->resolve(inv);
    EXPECT_EQ(after_accept.mode, INVENTORY_MODE_DIGEST);
    // FULL 接受不清零；下一次 DIGEST 成功接受时归零（由 onAccepted 保证）。
    EXPECT_EQ(h.planner->current()->digestFailures, 2u);
    h.planner->markPrepared(inv, after_accept);
    ::vehicle::fota::v1::TaskCheckResponse ok2;
    ok2.set_inventory_disposition(
        ::vehicle::fota::v1::INVENTORY_DISPOSITION_ACCEPTED);
    ok2.set_accepted_inventory_revision(rec.inventoryRevision);
    h.planner->onAccepted(after_accept, ok2);
    EXPECT_EQ(h.planner->current()->digestFailures, 0u);
}

// ---------------------------------------------------------------------------
// 无 durable 分配器 -> fail-closed（不伪造版本）
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, MissingAllocatorFailsClosed) {
    auto root = makeUniqueRoot();
    auto s = FotaStateStore::open(root, 100, 3600000);
    FotaStateStore fotaStore(std::move(s));
    FotaCloudStateStore cloudStore(fotaStore.underlyingStore());
    InventoryIdentityPlanner planner(cloudStore, nullptr);

    auto inv = makeInventory("v1");
    auto id = planner.resolve(inv);
    EXPECT_TRUE(id.freshRevision);
    EXPECT_THROW(planner.markPrepared(inv, id), std::runtime_error);
    EXPECT_FALSE(cloudStore.loadInventory().has_value());
}

// ---------------------------------------------------------------------------
// v1 记录迁移：无指纹 -> 身份不可用 -> 保守 FULL 建基线（保留已分配 revision）
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, LegacyRecordTreatedAsIdentityUnavailable) {
    PlannerHarness h;
    // 已部署设备已分配到 7（inventory.sequence），旧 fota.inventory 无指纹。
    for (int i = 0; i < 7; ++i) h.fotaStore->allocateSnapshotSeq();
    FotaInventoryRecord legacy;
    legacy.schemaVersion = 1;  // v1 envelope：无 status/fingerprint 字段
    legacy.mode = "FULL";
    legacy.inventoryRevision = 7;
    legacy.algorithm = "sha-256";
    legacy.ecuListDigestHex = std::string(64, 'a');
    legacy.fullRequired = true;
    h.cloudStore->saveInventory(legacy);

    auto loaded = h.cloudStore->loadInventory();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->fingerprintHex.empty());
    EXPECT_EQ(loaded->status, "REQUIRED_FULL");
    EXPECT_EQ(loaded->inventoryRevision, 7u);

    auto id = h.planner->resolve(makeInventory("v1"));
    EXPECT_EQ(id.reason, InventoryChangeReason::FirstBaseline);
    EXPECT_TRUE(id.freshRevision);
    auto rec = h.planner->markPrepared(makeInventory("v1"), id);
    EXPECT_EQ(rec.inventoryRevision, 8u);   // 序号继续递增，不回退
    EXPECT_EQ(highestSeq(*h.fotaStore), 8u);
}

// ---------------------------------------------------------------------------
// acceptedInventoryRevision 取自云端回执
// ---------------------------------------------------------------------------
TEST(InventoryIdentityPlanner, AcceptedRevisionComesFromCloudReceipt) {
    PlannerHarness h;
    auto inv = makeInventory("v1");
    auto id = h.planner->resolve(inv);
    auto rec = h.planner->markPrepared(inv, id);
    ASSERT_EQ(rec.inventoryRevision, 1u);

    ::vehicle::fota::v1::TaskCheckResponse resp;
    resp.set_inventory_disposition(
        ::vehicle::fota::v1::INVENTORY_DISPOSITION_ACCEPTED);
    resp.set_accepted_inventory_revision(42);
    h.planner->onAccepted(id, resp);

    auto cur = h.planner->current();
    ASSERT_TRUE(cur.has_value());
    EXPECT_EQ(cur->status, "ACCEPTED");
    EXPECT_EQ(cur->acceptedInventoryRevision, 42u);
    EXPECT_EQ(cur->inventoryRevision, 1u);
}
