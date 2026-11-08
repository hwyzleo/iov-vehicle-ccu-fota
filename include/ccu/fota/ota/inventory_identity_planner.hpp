#pragma once

// =============================================================================
// include/ccu/fota/ota/inventory_identity_planner.hpp
// CCU-FOTA 清单身份决策器 (CCU-FOTA-DSN-CR-012 §清单身份)
// =============================================================================
// 清单身份 = (inventoryMode, inventoryRevision, digestAlgorithm, ecuListDigest)。
//
// 本类是该身份的唯一决策入口，也是唯一允许调用 revision 分配器的组件：
//   * inventoryRevision 表示“清单内容版本”，不是采集次数；
//   * 采集阶段不分配序号（见 SnapshotAssembler::SeqPolicy::Defer）；
//   * 内容未变化且已有已接受清单 -> 复用 revision + DIGEST；
//   * 传输失败/结果未知 -> 复用同一身份重试（不产生新版本）；
//   * 云端 REVISION_CONFLICT / DIGEST_MISMATCH -> revision 弃用，新 revision FULL；
//   * FULL_REQUIRED -> 复用 revision 发 FULL；ALGORITHM_UNSUPPORTED -> 换算法。
//
// 身份状态在 fota.inventory 中 durable 保存（PREPARED 先于云端调用写入）。
// 决策规则见 CCU-FOTA-DSN-CR-012 §身份决策规则（1..7，自上而下先命中先生效）。
// =============================================================================

#include "ccu/fota/ota/ports/inventory_provider.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"

#include "vehicle/fota/v1/task.pb.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ccu_fota {
namespace ota {

// durable 序号分配器（生产注入 FotaStateStore::allocateSnapshotSeq）。
// 返回新分配的 revision；分配/落盘失败必须抛异常（fail-closed）。
using RevisionAllocator = std::function<std::uint64_t()>;

// 本轮决策原因（日志/测试可观测，不进入 wire）。
enum class InventoryChangeReason : std::uint8_t {
    FirstBaseline = 0,      // 无记录 / 身份不可用：建立基线
    ContentChanged,         // 内容指纹变化
    FullRequired,           // 云端要求完整清单
    Conflict,               // 云端已把 revision 绑定到不同内容
    AlgorithmUnsupported,    // 云端不支持当前摘要算法
    Retry,                  // 同一身份重试（传输失败/结果未知）
    ContentReused,          // 内容未变化：复用 revision + DIGEST
    DetectionDisabled,      // 变化检测关闭：固定复用 + DIGEST
};

const char* inventoryChangeReasonToString(InventoryChangeReason r);

struct InventoryIdentity {
    ::vehicle::fota::v1::InventoryMode mode =
        ::vehicle::fota::v1::INVENTORY_MODE_FULL;
    std::uint64_t revision = 0;
    std::string algorithm = "sha-256";
    std::string digestHex;
    InventoryChangeReason reason = InventoryChangeReason::FirstBaseline;
    // true 表示需要新分配 revision（由 markPrepared 执行分配）。
    bool freshRevision = false;
};

struct InventoryIdentityConfig {
    // 同一身份重试退避（fota.inventory.identity_retry_backoff_ms）。
    std::chrono::milliseconds retryBackoff{5000};
    // 连续 DIGEST 失配上限（fota.inventory.digest_max_consecutive_failures）。
    std::uint32_t digestMaxConsecutiveFailures = 3;
    // 变化检测开关（fota.inventory.change_detection_enabled）。
    bool changeDetectionEnabled = true;
};

class InventoryIdentityPlanner {
public:
    InventoryIdentityPlanner(store::fota::FotaCloudStateStore& store,
                             RevisionAllocator allocator,
                             InventoryIdentityConfig cfg = {});

    // 决策本轮身份（只读 store，不分配、不落盘）。
    InventoryIdentity resolve(const CollectedInventory& inv);

    // durable 写入 PREPARED（必须早于云端调用），必要时分配新 revision。
    // 返回最终记录（含已分配的 revision）；失败抛异常（fail-closed）。
    store::fota::FotaInventoryRecord markPrepared(const CollectedInventory& inv,
                                                  const InventoryIdentity& id);

    // 记录本轮请求载荷（供重试/诊断；不得 INFO 级 dump）。
    void saveRequestPayload(const store::fota::FotaInventoryRecord& rec,
                            std::string requestPbHex);

    // 云端 disposition 回执处理（durable）。
    void onAccepted(const InventoryIdentity& id,
                    const ::vehicle::fota::v1::TaskCheckResponse& resp);
    void onRejected(const InventoryIdentity& id,
                    const ::vehicle::fota::v1::TaskCheckResponse& resp);
    // 传输失败/超时/结果未知：保持 PREPARED 并记录退避（不新增版本）。
    void onTransportFailure(const InventoryIdentity& id, const std::string& errorCode);

    // 是否已到同身份重试时间（PREPARED 且未到 nextRetryAtMs 时返回 false）。
    bool retryReady(std::int64_t nowMs);

    std::optional<store::fota::FotaInventoryRecord> current();

private:
    store::fota::FotaCloudStateStore& store_;
    RevisionAllocator allocator_;
    InventoryIdentityConfig cfg_;
};

} // namespace ota
} // namespace ccu_fota
