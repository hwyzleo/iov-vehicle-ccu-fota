#pragma once

// =============================================================================
// include/ccu/fota/ota/ports/inventory_provider.hpp
// CCU-FOTA 清单提供者端口 (CCU-FOTA-DSN-CR-009 §任务检测 / CR-011 类型校准
//                                        / CR-012 §清单身份)
// =============================================================================
// InventoryProvider 只负责“采集”：产生稳定排序后的完整 ECU 清单、wire digest
// 与本地版本指纹。它不决定 inventoryMode、inventoryRevision，也不分配序号
// （CCU-FOTA-DSN-CR-012 D2/D3：分配点只在上报决策点，由
// InventoryIdentityPlanner 唯一负责）。
//
// 采集失败、被节流、校验不过或清单为空时 collected=false：调用方不得上报、不得
// 分配 revision，也不得产生 revision=0 或空清单版本（CR-012 D7）。
// =============================================================================

#include "vehicle/fota/v1/types.pb.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {

// 采集到的清单内容（不含 mode/revision；身份由 InventoryIdentityPlanner 决定）。
struct CollectedInventory {
    // true 表示采集成功且内容可上报。false 时其余字段无意义。
    bool collected = false;
    // wire 摘要：对稳定排序 + 规范化（inventory-ecu-list-v1）后的 ECU 列表计算。
    ::vehicle::fota::v1::Digest ecuListDigest;             // algorithm + value_hex
    // 稳定排序后的完整 ECU 清单（FULL 上报使用；DIGEST 上报时不入报文）。
    std::vector<::vehicle::fota::v1::EcuVersion> ecuList;
    std::string baselineCode;
    std::string fotaMasterVersion;
    std::int64_t collectedAtMs = 0;
    // 本地内容变化判据：versionFingerprint（cgw-fota-version-v1）。
    std::string fingerprintCanonicalization;
    std::string fingerprintHex;
};

class InventoryProvider {
public:
    virtual ~InventoryProvider() = default;

    // 采集完整清单内容。实现负责稳定排序、wire digest 与版本指纹的规范化，
    // 但不得决定 mode/revision，也不得分配序号。
    virtual CollectedInventory collectInventory() = 0;
};

} // namespace ota
} // namespace ccu_fota
