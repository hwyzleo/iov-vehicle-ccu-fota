#pragma once

// =============================================================================
// include/ccu/fota/someip/diag_inventory_codec.hpp
// CCU-DIAG 清单 payload 解码器 (CCU-FOTA-DSN-CR-013 §1 / US-036)
// =============================================================================
// 职责：把 CCU-DIAG `METHOD_COLLECT_VEHICLE_INVENTORY(0x0002)` 的响应 payload
// 解码为 VehicleSoftwareSnapshot。wire 形态隔离在本模块，业务侧只看到 DTO。
//
// 编码版本（CCU-FOTA-DSN-CR-014 D1 与 CCU-DIAG-DSN-CR-012 §5.2）：
//   * schemaVersion=1：版本化 JSON，字段集合冻结（必须含 ecuList 逐 ECU 明细）。
//     本项目决策（CCU-FOTA 侧放宽）：v1 若携带增量字段 softwarePartNumber 则照常
//     解码，不再因版本为 v1 而丢弃该事实；字段缺失/为空仍视为未提供，绝不
//     用 partNumber 补齐。
//   * schemaVersion=2：目标契约，逐 ECU 独立表达 partNumber、softwarePartNumber、
//     swVersion、hwVersion 与 serialNumber（解码语义与 v1 一致）。
//   * 未知/缺失 schemaVersion、缺字段、非法枚举 -> fail-closed（不猜测、不降级）。
//   * 字段名同时接受冻结的 camelCase 与同义 snake_case 别名：两侧字段集合完全
//     一致，不存在第二套语义，仅降低 FOTA/DIAG 发布顺序风险。
//
// 本解码器只做“解码 + 结构校验”，不做稳定排序/digest/指纹（由
// InventoryProviderFromAssembler 按 CR-012 规范化域统一计算）。
// =============================================================================

#include "data_models.h"

#include <string>
#include <string_view>

namespace ccu_fota {
namespace someip {

// 解码结果。ok=false 时 snapshot 内容不可用（调用方按采集失败处理）。
struct DiagInventoryDecodeResult {
    bool ok = false;
    VehicleSoftwareSnapshot snapshot;
    // FOTA-INVENTORY-* 错误码（见 inventory_error_codes.hpp）与可定位细节。
    // 细节只含字段名/枚举值，不含 VIN 或清单原值。
    std::string errorCode;
    std::string errorDetail;
};

// DIAG 清单 payload 解码器（无状态，可并发调用）。
class DiagInventoryCodec {
public:
    // 支持识别并可解码的编码版本（CCU-FOTA-DSN-CR-014 D1）。
    // v1/v2 均可解码；是否可上报只取决于必需事实是否齐备（由采集端口
    // 统一 fail-closed）。未知版本仍返回 kDiagPayloadInvalid。
    static constexpr const char* kSchemaVersionV1 = "1";
    static constexpr const char* kSchemaVersionV2 = "2";

    // 解码 DIAG 0x0002 响应 payload。任何结构/版本/枚举问题都返回 ok=false。
    static DiagInventoryDecodeResult decode(std::string_view payload);
};

} // namespace someip
} // namespace ccu_fota
