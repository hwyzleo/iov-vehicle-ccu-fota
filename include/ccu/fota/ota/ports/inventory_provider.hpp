#pragma once

// =============================================================================
// include/ccu/fota/ota/ports/inventory_provider.hpp
// CCU-FOTA 清单提供者端口 (CCU-FOTA-DSN-CR-009 §任务检测 / CR-011 类型校准)
// =============================================================================
// InventoryProvider 产生 FULL/DIGEST 清单（EcuVersion 列表 + 摘要 + revision），
// 规范化后计算 inventoryRevision + ecuListDigest + algorithm。FULL 时 ecuList
// 必须非空。Mock/真实实现共享同一端口，不分叉状态机或 payload。
// =============================================================================

#include "vehicle/fota/v1/types.pb.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {

// 采集到的清单（供 FotaOrchestrator 组装进 vehicle.fota.v1.TaskCheckRequest）。
struct CollectedInventory {
    ::vehicle::fota::v1::InventoryMode mode =
        ::vehicle::fota::v1::INVENTORY_MODE_FULL;
    std::uint64_t inventoryRevision = 0;
    ::vehicle::fota::v1::Digest ecuListDigest;          // algorithm + value_hex
    std::vector<::vehicle::fota::v1::EcuVersion> ecuList;  // FULL 时非空
    std::string baselineCode;
    std::string fotaMasterVersion;
    std::int64_t collectedAtMs = 0;
};

class InventoryProvider {
public:
    virtual ~InventoryProvider() = default;

    // 产生 ECU 清单。FULL 模式返回完整 ecuList + ecuListDigest；DIGEST 模式仅返回
    // 摘要（ecuList 可为空）。规范化、digest 由实现负责。
    virtual CollectedInventory
    collectInventory(::vehicle::fota::v1::InventoryMode mode) = 0;
};

} // namespace ota
} // namespace ccu_fota
