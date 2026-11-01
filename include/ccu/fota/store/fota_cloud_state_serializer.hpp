#pragma once

// =============================================================================
// include/ccu/fota/store/fota_cloud_state_serializer.hpp
// CCU-FOTA 车云 FOTA 状态序列化器 (CCU-FOTA-DSN-CR-011 §Store)
// =============================================================================
// 将 fota_cloud_state.hpp 中的记录序列化为版本化 FOTA envelope（复用既有 FTSE
// 帧头 + JSON payload），供 ccu-framework-store 以 std::string 持久化。复杂 proto
// 消息以 proto-binary hex 内嵌。未知新版本 fail-closed。
// =============================================================================

#include "ccu/fota/store/fota_cloud_state.hpp"

#include <string>

namespace ccu_fota {
namespace store {
namespace fota {

// proto-binary <-> hex 辅助（供 orchestrator 在 record 字段与 proto 消息间转换）。
std::string protoBinaryToHex(const std::string& binary);
std::string hexToProtoBinary(const std::string& hex);

// 每类记录的 encode（record -> envelope string）
std::string encodeVehicleTask(const FotaVehicleTaskRecord& r);
std::string encodeInventory(const FotaInventoryRecord& r);
std::string encodeConsent(const FotaConsentRecord& r);
std::string encodeDownload(const FotaDownloadRecord& r);
std::string encodeExecution(const FotaExecutionRecord& r);
std::string encodeEventOutboxMeta(const FotaEventOutboxMeta& r);
std::string encodeEventOutbox(const FotaEventOutboxRecord& r);
std::string encodeControl(const FotaControlRecord& r);
std::string encodePolicy(const FotaPolicyRecord& r);
std::string encodeLogJob(const FotaLogJobRecord& r);

// 每类记录的 decode（envelope string -> record），含 schemaVersion 校验
FotaVehicleTaskRecord decodeVehicleTask(const std::string& bytes);
FotaInventoryRecord    decodeInventory(const std::string& bytes);
FotaConsentRecord      decodeConsent(const std::string& bytes);
FotaDownloadRecord     decodeDownload(const std::string& bytes);
FotaExecutionRecord    decodeExecution(const std::string& bytes);
FotaEventOutboxMeta    decodeEventOutboxMeta(const std::string& bytes);
FotaEventOutboxRecord  decodeEventOutbox(const std::string& bytes);
FotaControlRecord      decodeControl(const std::string& bytes);
FotaPolicyRecord       decodePolicy(const std::string& bytes);
FotaLogJobRecord       decodeLogJob(const std::string& bytes);

} // namespace fota
} // namespace store
} // namespace ccu_fota
