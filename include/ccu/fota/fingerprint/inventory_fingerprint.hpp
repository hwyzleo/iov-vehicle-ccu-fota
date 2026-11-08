#pragma once

// =============================================================================
// include/ccu/fota/fingerprint/inventory_fingerprint.hpp
// CCU-FOTA 业务摘要 (CCU-FOTA-DSN-CR-006 §10.1/10.3/10.4/10.5)
// =============================================================================
// 定义并版本化四类业务摘要，禁止用单一 hash 字段混用：
//   versionFingerprint  / cgw-fota-version-v2  - 自动版本变化检测（CR-014）
//   snapshotFingerprint / cgw-fota-snapshot-v2 - 重复快照判断、成功状态对账（CR-014）
//   dedupeKey           / cgw-fota-dedupe-v1   - 目标链路提交去重
//   identifierDigest    / cgw-fota-id-v1/<kind>- 敏感标识域隔离摘要
//   wire ecuListDigest  / inventory-ecu-list-v2 - 车云 wire 清单摘要（CR-014）
//
// CR-014 在同一 canonicalization 名称下修改了摘要语义（新增独立软件件号、新排序键），
// 因此 version/snapshot 升 v2；v1 常量仅保留供迁移/识别，不得与新域跨版本比较。
//
// FOTA 负责业务规范化（字段选择、排序、编码、格式版本、持久化与比较语义）；
// SHA-256 仅经 ccu-framework-hash（ccu::fw::hash::sha256 / sha256_hex）计算。
// 依赖方向固定：FOTA 业务规范化 -> framework-hash；不反向依赖 config/store/log。
// =============================================================================

#include "data_models.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ccu_fota {
namespace fingerprint {

// ---------------------------------------------------------------------------
// Fingerprint - 单个摘要结果
// digest 由 ccu-framework-hash 计算；hex 由 framework sha256_hex 生成。
// FOTA 不实现 SHA-256 或 Hex 编码。
// ---------------------------------------------------------------------------
struct Fingerprint {
    std::string algorithm{"sha-256"};
    std::string canonicalization;            // e.g. "cgw-fota-version-v2"
    std::array<std::uint8_t, 32> digest{};   // 32 字节原始摘要
    std::string hexStr;                      // 64 小写 hex（来自 framework）
    std::string hex() const { return hexStr; }
};

// 敏感标识类型（域隔离，防止跨类型关联）
enum class IdentifierKind : std::uint8_t {
    Vin,
    DeviceSn,
};

// canonicalization 域常量（变更语义必须递增版本号）
//
// 注意：这些字符串是 SHA-256 的域分隔前缀，直接参与摘要计算，并被写入持久化
// 状态的 canonicalization 字段、以及目标链路的去重键。CGW -> CCU 改名时**故意
// 保留** cgw 字样：改动任何一个字符都会让相同输入产生不同指纹。
// CR-014 D4：在同一域名称下不得改变摘要输入（新增独立软件件号、新排序键），
// 因此 version/snapshot 升 v2、wire 升 inventory-ecu-list-v2；旧 v1 保留供迁移
// 识别（inventory v3 回填），不得与新域直接比较。
constexpr const char* VERSION_DOMAIN   = "cgw-fota-version-v2";
constexpr const char* SNAPSHOT_DOMAIN  = "cgw-fota-snapshot-v2";
constexpr const char* DEDUPE_DOMAIN    = "cgw-fota-dedupe-v1";
constexpr const char* ID_DOMAIN_PREFIX = "cgw-fota-id-v1/";
// wire ecuListDigest 域（wire 与本地指纹独立版本化，CR-014 D4）。
constexpr const char* WIRE_DOMAIN      = "inventory-ecu-list-v2";

// 历史 v1 域（只用于迁移识别，不得用于新摘要或跨域比较）：
//   VERSION_DOMAIN_V1  - cgw-fota-version-v1（CR-006）
//   SNAPSHOT_DOMAIN_V1 - cgw-fota-snapshot-v1（CR-006）
//   WIRE_DOMAIN_V1     - inventory-ecu-list-v1（CR-012），inventory v3 迁移回填值
constexpr const char* VERSION_DOMAIN_V1  = "cgw-fota-version-v1";
constexpr const char* SNAPSHOT_DOMAIN_V1 = "cgw-fota-snapshot-v1";
constexpr const char* WIRE_DOMAIN_V1     = "inventory-ecu-list-v1";

// 默认目标链路
constexpr const char* DESTINATION_TBOX_SOFTWARE_INVENTORY = "tbox-software-inventory";

// ---------------------------------------------------------------------------
// versionFingerprint v2（CR-014）
// 覆盖 baseline 标识/来源、registryVersion 及稳定排序后的 ECU 标识、总成件号、
// 独立软件件号、软件版本、可选硬件版本；排除 source/status/errorCode/
// overallResult、时间、序号、请求/报告 ID、重试字段与 serialNumber（ECU 实例
// 更换不得伪装成软件版本变化）。
// ---------------------------------------------------------------------------
Fingerprint buildVersionFingerprint(const VehicleSoftwareSnapshot& snapshot);

// ---------------------------------------------------------------------------
// snapshotFingerprint v2（CR-014）
// 覆盖 VIN 的 identifierDigest(vin)、vinSource、baseline、registryVersion、
// overallResult 及 ECU 稳定件号/版本字段、source、status、稳定 errorCode；
// 排除时间、序号、请求/报告 ID 与本地重试元数据，使相同业务快照在重试/重启后
// 得到相同指纹。
// ---------------------------------------------------------------------------
Fingerprint buildSnapshotFingerprint(const VehicleSoftwareSnapshot& snapshot);

// ---------------------------------------------------------------------------
// dedupeKey v1（域不变）
// 由域、目标链路、snapshot canonicalization 与完整 snapshotFingerprint（32 raw
// bytes）再次规范化并计算 SHA-256。返回 64 小写 hex，不截断。
// 不同摘要类型/版本/目标不得共享命名空间。CR-014 不改此域：其输入已包含
// snapshot canonicalization 与新摘要值。
// ---------------------------------------------------------------------------
std::string buildDedupeKey(const Fingerprint& snapshotFingerprint,
                           std::string_view destination);

// ---------------------------------------------------------------------------
// identifierDigest
// 对 VIN、device_sn 等敏感标识生成域隔离摘要（cgw-fota-id-v1/<kind>）。
// normalizedValue 须为经业务校验的规范值（VIN 大写 ASCII，device_sn 规范形式）；
// 本函数不做未定义 trim、locale case-fold 或 Unicode 归一化。
// 返回 64 小写 hex。无密钥 SHA-256 不构成匿名化/认证/防篡改。
// ---------------------------------------------------------------------------
std::string buildIdentifierDigest(IdentifierKind kind,
                                  std::string_view normalizedValue);

} // namespace fingerprint
} // namespace ccu_fota
