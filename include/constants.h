#pragma once

#include <cstdint>
#include <string>

namespace ccu_fota {

// Default configuration values
constexpr uint32_t DEFAULT_MAX_ECU_COUNT = 100;
constexpr uint64_t DEFAULT_SNAPSHOT_SEQ_INITIAL = 1;
constexpr uint32_t DEFAULT_THROTTLE_INTERVAL_MS = 5000;

// ============================================================
// Store / 运行状态持久化 (CCU-FOTA-DSN-CR-005)
// 根目录由 common.store.root 注入，缺省 /var/lib/ccu；不得失败时切 /tmp。
// ============================================================
const std::string  DEFAULT_STORE_ROOT        = "/var/lib/ccu";
constexpr uint32_t DEFAULT_DEDUPE_MAX_ENTRIES = 100;
constexpr int64_t  DEFAULT_DEDUPE_TTL_MS      = 3600000; // 1h
constexpr uint64_t SNAPSHOT_SEQ_MAX           = UINT64_MAX; // 溢出边界

// ============================================================
// SOME/IP 寻址（CCU-FOTA-DSN-CR-004）
// 过渡 SSOT：在整车 SOME/IP Service Registry 落地前，寻址值由本常量供给；
// fota.yaml 不再承载 Service/Instance/Method ID、协议或端口。
// ============================================================
// CCU-DIAG service (CCU-DIAG-DSN-CR-007: 0x1110 / 0x0001 / TCP / 51110)
constexpr uint16_t DEFAULT_DIAG_SERVICE_ID  = 0x1110;
constexpr uint16_t DEFAULT_DIAG_INSTANCE_ID = 0x0001;
constexpr uint16_t DEFAULT_DIAG_PORT        = 51110;
const std::string  DEFAULT_DIAG_IP_ADDRESS  = "127.0.0.1";

// TBOX-SOMEIP service (CCU-FOTA-DSN-CR-002: 0x6101 / 0x0001 / TCP / 56101)
constexpr uint16_t DEFAULT_TBOX_SERVICE_ID  = 0x6101;
constexpr uint16_t DEFAULT_TBOX_INSTANCE_ID = 0x0001;

// FOTA Provider service (CCU-FOTA-DSN-CR-002: 0x1120 / 0x0001 / TCP / 51120)
constexpr uint16_t FOTA_PROVIDER_SERVICE_ID  = 0x1120;
constexpr uint16_t FOTA_PROVIDER_INSTANCE_ID = 0x0001;
constexpr uint16_t FOTA_PROVIDER_PORT        = 51120;
const std::string  FOTA_PROVIDER_IP_ADDRESS  = "0.0.0.0";

// FOTA Provider Method IDs (service-scoped)
constexpr uint16_t METHOD_REQUEST_SOFTWARE_INVENTORY = 0x0001;

// TBOX-SOMEIP Method IDs (service-scoped)
// Note: CCU-FOTA proactively reports software inventory to TBOX
constexpr uint16_t TBOX_METHOD_REPORT_SOFTWARE_INVENTORY = 0x0001;

// Method IDs for CCU-DIAG (DIAG service-scoped)
// Note: Cloud requests vehicle to collect and report
constexpr uint16_t METHOD_READ_VIN = 0x0001;
constexpr uint16_t METHOD_COLLECT_VEHICLE_INVENTORY = 0x0002;
constexpr uint16_t METHOD_GET_ECU_VERSION = 0x0003;
constexpr uint16_t METHOD_GET_REGISTRY_VERSION = 0x0004;
constexpr uint16_t METHOD_REPORT_SOFTWARE_INVENTORY = 0x0005;

// Event IDs for SOME/IP
constexpr uint16_t EVENT_VERSION_CHANGED = 0x0001;

// MQTT topic
const std::string MQTT_TOPIC_PREFIX = "vehicle/";
const std::string MQTT_TOPIC_SUFFIX = "/up/fota";

} // namespace ccu_fota
