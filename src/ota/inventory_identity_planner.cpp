// =============================================================================
// src/ota/inventory_identity_planner.cpp
// CCU-FOTA 清单身份决策器实现 (CCU-FOTA-DSN-CR-012 §身份决策规则)
// =============================================================================

#include "ccu/fota/ota/inventory_identity_planner.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace ccu_fota {
namespace ota {

using namespace ccu_fota::store::fota;

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

constexpr const char* kStatusPrepared = "PREPARED";
constexpr const char* kStatusAccepted = "ACCEPTED";
constexpr const char* kStatusRequiredFull = "REQUIRED_FULL";
constexpr const char* kStatusConflict = "CONFLICT";
constexpr const char* kStatusAlgorithmUnsupported = "ALGORITHM_UNSUPPORTED";

constexpr const char* kModeFull = "FULL";
constexpr const char* kModeDigest = "DIGEST";

::vehicle::fota::v1::InventoryMode modeFromString(const std::string& mode) {
    return mode == kModeDigest ? ::vehicle::fota::v1::INVENTORY_MODE_DIGEST
                               : ::vehicle::fota::v1::INVENTORY_MODE_FULL;
}

std::string modeToString(::vehicle::fota::v1::InventoryMode mode) {
    return mode == ::vehicle::fota::v1::INVENTORY_MODE_DIGEST ? kModeDigest
                                                              : kModeFull;
}

// 从云端 supported_inventory_digest_algorithms 选择一个本端可计算的算法。
// 当前只实现 sha-256；无可选项时保持当前算法（fail-closed：仍发 FULL）。
std::string pickSupportedAlgorithm(const ::vehicle::fota::v1::TaskCheckResponse& resp,
                                   const std::string& current) {
    for (const auto& a : resp.supported_inventory_digest_algorithms()) {
        if (a == "sha-256") return a;
    }
    return current;
}

} // namespace

const char* inventoryChangeReasonToString(InventoryChangeReason r) {
    switch (r) {
        case InventoryChangeReason::FirstBaseline:        return "first_baseline";
        case InventoryChangeReason::ContentChanged:       return "content_changed";
        case InventoryChangeReason::FullRequired:         return "full_required";
        case InventoryChangeReason::Conflict:             return "conflict";
        case InventoryChangeReason::AlgorithmUnsupported: return "algorithm_unsupported";
        case InventoryChangeReason::Retry:                return "retry";
        case InventoryChangeReason::ContentReused:        return "content_reused";
        case InventoryChangeReason::DetectionDisabled:    return "detection_disabled";
    }
    return "unknown";
}

InventoryIdentityPlanner::InventoryIdentityPlanner(
    store::fota::FotaCloudStateStore& store, RevisionAllocator allocator,
    InventoryIdentityConfig cfg)
    : store_(store), allocator_(std::move(allocator)), cfg_(cfg) {}

// ---------------------------------------------------------------------------
// 决策规则（CCU-FOTA-DSN-CR-012 §身份决策规则，自上而下先命中先生效）
// ---------------------------------------------------------------------------
InventoryIdentity InventoryIdentityPlanner::resolve(const CollectedInventory& inv) {
    InventoryIdentity id;
    id.algorithm = inv.ecuListDigest.algorithm().empty()
                       ? std::string("sha-256")
                       : inv.ecuListDigest.algorithm();
    id.digestHex = inv.ecuListDigest.value_hex();

    auto rec = store_.loadInventory();

    // 规则 1：无记录 / 身份不可用（旧格式、指纹缺失或 canonicalization 版本不同）
    // -> FULL + 新 revision（建立基线）。不跨版本比较指纹（§10.6）。
    if (!rec.has_value() || rec->fingerprintHex.empty() ||
        rec->fingerprintCanonicalization != inv.fingerprintCanonicalization) {
        id.mode = ::vehicle::fota::v1::INVENTORY_MODE_FULL;
        id.reason = InventoryChangeReason::FirstBaseline;
        id.freshRevision = true;
        return id;
    }

    // 规则 2：存在 PREPARED 未 ACCEPTED 记录（上次传输失败/结果未知）
    if (rec->status == kStatusPrepared) {
        if (rec->fingerprintHex == inv.fingerprintHex) {
            id.mode = modeFromString(rec->mode);
            id.revision = rec->inventoryRevision;
            id.algorithm = rec->algorithm.empty() ? id.algorithm : rec->algorithm;
            id.digestHex = rec->ecuListDigestHex;
            id.reason = InventoryChangeReason::Retry;
            return id;
        }
        // 内容已变化：旧 PREPARED 身份弃用，新 revision FULL。
        id.mode = ::vehicle::fota::v1::INVENTORY_MODE_FULL;
        id.reason = InventoryChangeReason::ContentChanged;
        id.freshRevision = true;
        return id;
    }

    // 规则 3：云端已将该 revision 绑定到不同内容 -> 新 revision FULL
    if (rec->status == kStatusConflict) {
        // 防御（digest_max_consecutive_failures）：连续 DIGEST 失配已达上限时，
        // 不再继续扩版本，改用云端已接受的 revision 重发 FULL（幂等重绑定）。
        if (rec->digestFailures >= cfg_.digestMaxConsecutiveFailures &&
            rec->acceptedInventoryRevision != 0) {
            id.mode = ::vehicle::fota::v1::INVENTORY_MODE_FULL;
            id.revision = rec->acceptedInventoryRevision;
            id.reason = InventoryChangeReason::FullRequired;
            return id;
        }
        id.mode = ::vehicle::fota::v1::INVENTORY_MODE_FULL;
        id.reason = InventoryChangeReason::Conflict;
        id.freshRevision = true;
        return id;
    }

    // 规则 4：云端要求下一次 FULL -> 复用 revision
    if (rec->status == kStatusRequiredFull) {
        id.mode = ::vehicle::fota::v1::INVENTORY_MODE_FULL;
        id.revision = rec->inventoryRevision;
        id.reason = InventoryChangeReason::FullRequired;
        return id;
    }

    // 规则 5：算法不支持 -> 换算法后 FULL，revision 保留
    if (rec->status == kStatusAlgorithmUnsupported) {
        id.mode = ::vehicle::fota::v1::INVENTORY_MODE_FULL;
        id.revision = rec->inventoryRevision;
        id.algorithm = rec->algorithm.empty() ? id.algorithm : rec->algorithm;
        id.reason = InventoryChangeReason::AlgorithmUnsupported;
        return id;
    }

    // 规则 6/7：已接受清单 -> 指纹决定 DIGEST 复用或 FULL 新版本
    if (!cfg_.changeDetectionEnabled) {
        // 变化检测关闭时也不得以“每周期新 revision”替代检测：
        // 固定复用已接受身份 + DIGEST，由云端失配驱动 FULL。
        id.mode = ::vehicle::fota::v1::INVENTORY_MODE_DIGEST;
        id.revision = rec->inventoryRevision;
        id.reason = InventoryChangeReason::DetectionDisabled;
        return id;
    }
    if (rec->fingerprintHex == inv.fingerprintHex) {
        id.mode = ::vehicle::fota::v1::INVENTORY_MODE_DIGEST;
        id.revision = rec->inventoryRevision;
        id.reason = InventoryChangeReason::ContentReused;
        return id;
    }
    id.mode = ::vehicle::fota::v1::INVENTORY_MODE_FULL;
    id.reason = InventoryChangeReason::ContentChanged;
    id.freshRevision = true;
    return id;
}

// ---------------------------------------------------------------------------
// PREPARED 检查点（必须早于云端调用写入）
// ---------------------------------------------------------------------------
FotaInventoryRecord
InventoryIdentityPlanner::markPrepared(const CollectedInventory& inv,
                                       const InventoryIdentity& id) {
    FotaInventoryRecord prev = store_.loadInventory().value_or(FotaInventoryRecord{});

    FotaInventoryRecord rec;
    rec.status = kStatusPrepared;
    rec.mode = modeToString(id.mode);
    if (id.freshRevision) {
        if (!allocator_) {
            // fail-closed：无 durable 序号分配器不得伪造版本。
            throw std::runtime_error("inventory revision allocator unavailable");
        }
        rec.inventoryRevision = allocator_();
    } else {
        rec.inventoryRevision = id.revision;
    }
    rec.acceptedInventoryRevision = prev.acceptedInventoryRevision;
    rec.requiredMode.clear();
    rec.algorithm = id.algorithm;
    rec.ecuListDigestHex = id.digestHex;
    rec.fingerprintCanonicalization = inv.fingerprintCanonicalization;
    rec.fingerprintHex = inv.fingerprintHex;
    rec.baselineCode = inv.baselineCode;
    rec.fotaMasterVersion = inv.fotaMasterVersion;
    rec.collectedAtMs = inv.collectedAtMs;
    rec.attempt = id.freshRevision ? 1u : prev.attempt + 1u;
    rec.nextRetryAtMs = 0;
    rec.lastErrorCode.clear();
    // 连续失配计数跨 FULL 重建保留，仅在 DIGEST 被接受时归零（onAccepted）。
    rec.digestFailures = prev.digestFailures;
    rec.fullRequired = false;

    store_.saveInventory(rec);
    return rec;
}

void InventoryIdentityPlanner::saveRequestPayload(const FotaInventoryRecord& rec,
                                                  std::string requestPbHex) {
    FotaInventoryRecord r = rec;
    r.requestPbHex = std::move(requestPbHex);
    store_.saveInventory(r);
}

// ---------------------------------------------------------------------------
// 云端回执
// ---------------------------------------------------------------------------
void InventoryIdentityPlanner::onAccepted(
    const InventoryIdentity& /*id*/,
    const ::vehicle::fota::v1::TaskCheckResponse& resp) {
    auto rec = store_.loadInventory();
    if (!rec.has_value()) return;

    rec->status = kStatusAccepted;
    rec->acceptedInventoryRevision = resp.has_accepted_inventory_revision()
                                         ? resp.accepted_inventory_revision()
                                         : rec->inventoryRevision;
    rec->requiredMode = resp.has_required_inventory_mode()
                            ? modeToString(resp.required_inventory_mode())
                            : std::string();
    rec->nextRetryAtMs = 0;
    rec->lastErrorCode.clear();
    rec->fullRequired = false;
    // DIGEST 被接受说明双方规范化一致 -> 失配计数归零。FULL 被接受不清零，
    // 使 digest_max_consecutive_failures 能约束反复失配时的版本扩散。
    if (rec->mode == kModeDigest) rec->digestFailures = 0;
    store_.saveInventory(*rec);
}

void InventoryIdentityPlanner::onRejected(
    const InventoryIdentity& /*id*/,
    const ::vehicle::fota::v1::TaskCheckResponse& resp) {
    auto rec = store_.loadInventory();
    if (!rec.has_value()) return;

    using ::vehicle::fota::v1::TaskCheckResponse;
    const auto disposition = resp.inventory_disposition();
    rec->attempt += 1;
    rec->nextRetryAtMs = 0;  // 失配后允许下一周期立即按新决策重试
    rec->lastErrorCode = ::vehicle::fota::v1::InventoryDisposition_Name(disposition);

    switch (disposition) {
        case ::vehicle::fota::v1::INVENTORY_DISPOSITION_FULL_REQUIRED:
            rec->status = kStatusRequiredFull;
            rec->requiredMode = kModeFull;
            rec->fullRequired = true;
            break;
        case ::vehicle::fota::v1::INVENTORY_DISPOSITION_REVISION_CONFLICT:
            rec->status = kStatusConflict;
            break;
        case ::vehicle::fota::v1::INVENTORY_DISPOSITION_DIGEST_MISMATCH:
            rec->status = kStatusConflict;
            rec->digestFailures += 1;
            break;        case ::vehicle::fota::v1::INVENTORY_DISPOSITION_ALGORITHM_UNSUPPORTED:
            rec->status = kStatusAlgorithmUnsupported;
            rec->algorithm = pickSupportedAlgorithm(
                resp, rec->algorithm.empty() ? std::string("sha-256") : rec->algorithm);
            break;
        default:
            // UNSPECIFIED/未知：保守按“要求完整清单”处理（不得继续发 DIGEST）。
            rec->status = kStatusRequiredFull;
            rec->requiredMode = kModeFull;
            rec->fullRequired = true;
            break;
    }
    store_.saveInventory(*rec);
}

void InventoryIdentityPlanner::onTransportFailure(const InventoryIdentity& id,
                                                  const std::string& errorCode) {
    auto rec = store_.loadInventory();
    if (!rec.has_value()) {
        // PREPARED 写入失败等极端情况：不伪造状态，交由上层记录错误。
        return;
    }
    rec->status = kStatusPrepared;
    if (rec->mode.empty()) rec->mode = modeToString(id.mode);
    rec->attempt += 1;
    rec->lastErrorCode = errorCode;
    rec->nextRetryAtMs =
        nowMs() + static_cast<std::int64_t>(cfg_.retryBackoff.count());
    store_.saveInventory(*rec);
}

// ---------------------------------------------------------------------------
bool InventoryIdentityPlanner::retryReady(std::int64_t now) {
    auto rec = store_.loadInventory();
    if (!rec.has_value()) return true;
    if (rec->status != kStatusPrepared) return true;
    return now >= rec->nextRetryAtMs;
}

std::optional<FotaInventoryRecord> InventoryIdentityPlanner::current() {
    return store_.loadInventory();
}

} // namespace ota
} // namespace ccu_fota
