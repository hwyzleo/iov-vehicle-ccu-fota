// =============================================================================
// src/config/fota_config.cpp - CCU-FOTA 配置映射实现 (CCU-FOTA-DSN-CR-004)
// =============================================================================
// 从 ccu-framework-config 的 ConfigSnapshot 读取 fota.* 并完成类型/范围/跨字段
// 校验；从同一快照读取 common.log.* / fota.log.* 构建 LogConfig。不包含 yaml-cpp
// 直接访问。
// =============================================================================

#include "ccu/fota/config/fota_config.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace ccu_fota {

namespace {

using ccu::fw::config::ConfigSnapshot;
using ccu::fw::log::LogConfig;
using ccu::fw::log::LogLevel;
using ccu::fw::log::logLevelFromString;
using ccu::fw::someip::SomeIpConfig;

// 范围契约（CCU-FOTA-REQ-CR-004 / DSN-CR-004）
constexpr std::uint32_t kMaxPendingLo = 1;
constexpr std::uint32_t kMaxPendingHi = 1024;
constexpr std::uint32_t kRetryAttemptsHi = 10;

void throwRange(const char* key, std::int64_t value,
                std::int64_t lo, std::int64_t hi) {
    throw FotaConfigException(
        std::string("config range violation: ") + key + " (" +
        std::to_string(value) + ") not in [" + std::to_string(lo) + "," +
        std::to_string(hi) + "]");
}

void requireRange(const char* key, std::int64_t value,
                  std::int64_t lo, std::int64_t hi) {
    if (value < lo || value > hi) {
        throwRange(key, value, lo, hi);
    }
}

// additionalProperties:false 的运行时等价：拒绝未知子键。
void rejectUnknown(const ConfigSnapshot& snap, const char* section,
                   const std::vector<std::string>& allowed) {
    for (const auto& k : snap.keys(section)) {
        if (std::find(allowed.begin(), allowed.end(), k) == allowed.end()) {
            throw FotaConfigException(
                std::string("unknown config key: ") + section + "." + k);
        }
    }
}

} // namespace

FotaConfigException::FotaConfigException(const std::string& message)
    : std::runtime_error(message) {}

FotaConfig FotaConfig::from(const ConfigSnapshot& s) {
    FotaConfig c;

    // ---- fota 顶层仅允许 inventory / diag / someip / log / cloud / mock ----
    rejectUnknown(s, "fota", {"inventory", "diag", "someip", "log", "cloud", "mock"});

    // ---- inventory.* ----
    rejectUnknown(s, "fota.inventory",
                  {"change_detection_enabled",
                   "min_report_interval_ms", "max_pending_requests",
                   "dedupe_max_entries", "dedupe_ttl_ms",
                   "identity_retry_backoff_ms",
                   "digest_max_consecutive_failures"});

    c.changeDetectionEnabled =
        s.getOr<bool>("fota.inventory.change_detection_enabled", true);

    std::int64_t minInterval =
        s.getOr<std::int64_t>("fota.inventory.min_report_interval_ms", 300000);
    requireRange("fota.inventory.min_report_interval_ms", minInterval, 0,
                 INT64_MAX);
    c.minReportInterval = std::chrono::milliseconds(minInterval);

    std::int64_t maxPending =
        s.getOr<std::int64_t>("fota.inventory.max_pending_requests", 32);
    requireRange("fota.inventory.max_pending_requests", maxPending,
                 kMaxPendingLo, kMaxPendingHi);
    c.maxPendingRequests = static_cast<std::uint32_t>(maxPending);

    // dedupe 边界 (CCU-FOTA-DSN-CR-005)
    std::int64_t dedupeMaxEntries =
        s.getOr<std::int64_t>("fota.inventory.dedupe_max_entries", 100);
    requireRange("fota.inventory.dedupe_max_entries", dedupeMaxEntries,
                 1, kMaxPendingHi);
    c.dedupeMaxEntries = static_cast<std::uint32_t>(dedupeMaxEntries);

    std::int64_t dedupeTtl =
        s.getOr<std::int64_t>("fota.inventory.dedupe_ttl_ms", 3600000);
    requireRange("fota.inventory.dedupe_ttl_ms", dedupeTtl, 0, INT64_MAX);
    c.dedupeTtlMs = dedupeTtl;

    // ---- 清单身份 (CCU-FOTA-DSN-CR-012) ----
    // 同一清单身份的失败重试退避（不得因传输失败产生新版本）。
    std::int64_t invRetry =
        s.getOr<std::int64_t>("fota.inventory.identity_retry_backoff_ms", 5000);
    requireRange("fota.inventory.identity_retry_backoff_ms", invRetry, 0, 600000);
    c.inventoryIdentityRetryBackoff = std::chrono::milliseconds(invRetry);

    // 连续 DIGEST 失配上限（防御规范化实现不一致导致抖动）。
    std::int64_t digestMax = s.getOr<std::int64_t>(
        "fota.inventory.digest_max_consecutive_failures", 3);
    requireRange("fota.inventory.digest_max_consecutive_failures", digestMax, 1, 10);
    c.inventoryDigestMaxConsecutiveFailures = static_cast<std::uint32_t>(digestMax);

    // ---- common.store.root (CCU-FOTA-DSN-CR-005) ----
    c.storeRoot = s.getOr<std::string>("common.store.root", "/var/lib/ccu");

    // ---- diag.* ----
    rejectUnknown(s, "fota.diag",
                  {"collect_timeout_ms", "retry_max_attempts", "retry_backoff_ms",
                   "registration"});

    std::int64_t diagTimeout =
        s.getOr<std::int64_t>("fota.diag.collect_timeout_ms", 30000);
    requireRange("fota.diag.collect_timeout_ms", diagTimeout, 1, INT64_MAX);
    c.diagCollectTimeout = std::chrono::milliseconds(diagTimeout);

    std::int64_t diagAttempts =
        s.getOr<std::int64_t>("fota.diag.retry_max_attempts", 2);
    requireRange("fota.diag.retry_max_attempts", diagAttempts, 0,
                 kRetryAttemptsHi);
    c.diagRetry.maxAttempts = static_cast<std::uint32_t>(diagAttempts);

    std::int64_t diagBackoff =
        s.getOr<std::int64_t>("fota.diag.retry_backoff_ms", 1000);
    requireRange("fota.diag.retry_backoff_ms", diagBackoff, 0, INT64_MAX);
    c.diagRetry.backoff = std::chrono::milliseconds(diagBackoff);

    // ---- fota.diag.registration.*（CCU-FOTA-DSN-CR-013）----
    // 登记能力（Method ID 由 Registry 分配，0 = 未分配 -> 能力关闭）。
    rejectUnknown(s, "fota.diag.registration",
                  {"enabled", "method_id", "interface_major", "timeout_ms"});
    c.diagRegistration.enabled =
        s.getOr<bool>("fota.diag.registration.enabled", true);
    {
        std::int64_t v =
            s.getOr<std::int64_t>("fota.diag.registration.method_id", 0);
        // 0 = Registry 尚未分配（合法，能力关闭）；0x0001..0x7FFF 为可分配区。
        requireRange("fota.diag.registration.method_id", v, 0, 0x7FFF);
        c.diagRegistration.methodId = static_cast<std::uint16_t>(v);

        v = s.getOr<std::int64_t>("fota.diag.registration.interface_major", 1);
        requireRange("fota.diag.registration.interface_major", v, 1, 255);
        if (v != 1) {
            // 与 DIAG 接口 major 不一致 -> fail-closed（不得以不兼容版本调用）
            throw FotaConfigException(
                "fota.diag.registration.interface_major mismatch: expected 1");
        }
        c.diagRegistration.interfaceMajor = static_cast<std::uint8_t>(v);

        v = s.getOr<std::int64_t>("fota.diag.registration.timeout_ms", 30000);
        requireRange("fota.diag.registration.timeout_ms", v, 1, 600000);
        c.diagRegistration.timeout = std::chrono::milliseconds(v);
    }

    // ---- tbox.* 已移除：InventoryReporter/TboxInventoryClient 为死代码，
    // fota.tbox.submit_timeout_ms / retry_* 不再解析（见收尾清理）。 ----

    // ---- someip.* (CCU-FOTA-DSN-CR-007/010/011) ----
    rejectUnknown(s, "fota.someip",
                  {"transport", "generic_transport"});

    // ---- fota.someip.transport.* 契约 B 有界传输配置 (CR-010/011) ----
    // 缺省用安全默认；声明但非法/越界则 fail-closed。
    rejectUnknown(s, "fota.someip.transport",
                  {"exchange_timeout_ms", "max_envelope_bytes", "max_payload_bytes",
                   "max_in_flight", "downlink_queue_capacity", "downlink_workers",
                   "in_flight_ttl_ms", "availability_wait_ms"});
    {
        std::int64_t v = s.getOr<std::int64_t>("fota.someip.transport.exchange_timeout_ms", 10000);
        requireRange("fota.someip.transport.exchange_timeout_ms", v, 1, INT64_MAX);
        c.transport.exchangeTimeout = std::chrono::milliseconds(v);
        v = s.getOr<std::int64_t>("fota.someip.transport.max_envelope_bytes", 262144);
        requireRange("fota.someip.transport.max_envelope_bytes", v, 64, INT64_MAX);
        c.transport.maxEnvelopeBytes = static_cast<std::size_t>(v);
        v = s.getOr<std::int64_t>("fota.someip.transport.max_payload_bytes", 262144);
        requireRange("fota.someip.transport.max_payload_bytes", v, 1, INT64_MAX);
        c.transport.maxPayloadBytes = static_cast<std::size_t>(v);
        v = s.getOr<std::int64_t>("fota.someip.transport.max_in_flight", 64);
        requireRange("fota.someip.transport.max_in_flight", v, 1, 1024);
        c.transport.maxInFlight = static_cast<std::size_t>(v);
        v = s.getOr<std::int64_t>("fota.someip.transport.downlink_queue_capacity", 256);
        requireRange("fota.someip.transport.downlink_queue_capacity", v, 1, 1048576);
        c.transport.downlinkQueueCapacity = static_cast<std::size_t>(v);
        v = s.getOr<std::int64_t>("fota.someip.transport.downlink_workers", 1);
        requireRange("fota.someip.transport.downlink_workers", v, 1, 64);
        c.transport.downlinkWorkers = static_cast<std::size_t>(v);
        v = s.getOr<std::int64_t>("fota.someip.transport.in_flight_ttl_ms", 30000);
        requireRange("fota.someip.transport.in_flight_ttl_ms", v, 1, INT64_MAX);
        c.transport.inFlightTtl = std::chrono::milliseconds(v);
        v = s.getOr<std::int64_t>("fota.someip.transport.availability_wait_ms", 5000);
        requireRange("fota.someip.transport.availability_wait_ms", v, 1, INT64_MAX);
        c.transport.availabilityWait = std::chrono::milliseconds(v);
    }

    // ---- fota.someip.generic_transport.* Registry 寻址 (CR-010/011) ----
    // 全 0 = 尚未分配（契约 B blocked）；声明非 0 则必须是完整且合法的 Registry 分配，
    // 部分/越界由本层校验，冲突与版本一致性由 tbox_generic_transport_contract 校验。
    rejectUnknown(s, "fota.someip.generic_transport",
                  {"method_id", "event_id", "eventgroup_id", "interface_major"});
    {
        std::int64_t m = s.getOr<std::int64_t>("fota.someip.generic_transport.method_id", 0);
        std::int64_t e = s.getOr<std::int64_t>("fota.someip.generic_transport.event_id", 0);
        std::int64_t g = s.getOr<std::int64_t>("fota.someip.generic_transport.eventgroup_id", 0);
        std::int64_t iv = s.getOr<std::int64_t>("fota.someip.generic_transport.interface_major", 1);
        if (m != 0 || e != 0 || g != 0) {
            // 部分声明 -> fail-closed：Method/Event/Eventgroup 必须同时完整分配。
            if (m == 0 || e == 0 || g == 0) {
                throw FotaConfigException(
                    "fota.someip.generic_transport: partial Registry allocation "
                    "(method_id/event_id/eventgroup_id must all be non-zero)");
            }
            requireRange("fota.someip.generic_transport.method_id", m, 1, 0x7FFF);
            requireRange("fota.someip.generic_transport.event_id", e, 0x8000, 0xFFFE);
            requireRange("fota.someip.generic_transport.eventgroup_id", g, 1, 0xFFFE);
            requireRange("fota.someip.generic_transport.interface_major", iv, 1, 0xFF);
            c.genericTransport.methodId = static_cast<std::uint16_t>(m);
            c.genericTransport.eventId = static_cast<std::uint16_t>(e);
            c.genericTransport.eventgroupId = static_cast<std::uint16_t>(g);
            c.genericTransport.interfaceMajor = static_cast<std::uint32_t>(iv);
        }
    }

    // ---- fota.cloud.* (CCU-FOTA-DSN-CR-009 §13.8 / CR-011 由 fota.ota 收敛) ----
    // 旧 fota.ota.* 仅存在于开发环境 -> 直接拒绝；受控发布物由迁移检查器处理。
    if (s.has("fota.ota")) {
        throw FotaConfigException(
            "fota.ota.* is legacy; use fota.cloud.* (CCU-FOTA-DSN-CR-011)");
    }
    rejectUnknown(s, "fota.cloud",
                  {"enabled", "protocol_version", "task_check_interval_ms",
                   "reconcile_on_start", "event_outbox_max", "event_batch_max",
                   "control_ack_timeout_ms", "cloud_call_timeout_ms",
                   "require_clock_sync", "clock_sync_poll_interval_ms",
                   "clock_min_valid_epoch_ms"});
    c.cloud.enabled = s.getOr<bool>("fota.cloud.enabled", true);
    c.cloud.protocolVersion =
        s.getOr<std::string>("fota.cloud.protocol_version", "fota-v1");
    std::int64_t taskCheck =
        s.getOr<std::int64_t>("fota.cloud.task_check_interval_ms", 60000);
    requireRange("fota.cloud.task_check_interval_ms", taskCheck, 1, INT64_MAX);
    c.cloud.taskCheckInterval = std::chrono::milliseconds(taskCheck);
    c.cloud.reconcileOnStart = s.getOr<bool>("fota.cloud.reconcile_on_start", true);
    std::int64_t outboxMax =
        s.getOr<std::int64_t>("fota.cloud.event_outbox_max", 4096);
    requireRange("fota.cloud.event_outbox_max", outboxMax, 1, 1048576);
    c.cloud.eventOutboxMax = static_cast<std::uint32_t>(outboxMax);
    std::int64_t batchMax =
        s.getOr<std::int64_t>("fota.cloud.event_batch_max", 64);
    requireRange("fota.cloud.event_batch_max", batchMax, 1, 65535);
    c.cloud.eventBatchMax = static_cast<std::uint32_t>(batchMax);
    std::int64_t ctrlAck =
        s.getOr<std::int64_t>("fota.cloud.control_ack_timeout_ms", 5000);
    requireRange("fota.cloud.control_ack_timeout_ms", ctrlAck, 1, INT64_MAX);
    c.cloud.controlAckTimeout = std::chrono::milliseconds(ctrlAck);
    std::int64_t cloudCall =
        s.getOr<std::int64_t>("fota.cloud.cloud_call_timeout_ms", 10000);
    requireRange("fota.cloud.cloud_call_timeout_ms", cloudCall, 1, INT64_MAX);
    c.cloud.cloudCallTimeout = std::chrono::milliseconds(cloudCall);

    // 时钟门禁：上行 Envelope 的 timestamp_ms/expire_at_ms 由 CLOCK_REALTIME 推导，
    // 时钟未同步时 expire_at_ms 落在过去 -> 云侧判定 MESSAGE_EXPIRED 并丢入 DLQ。
    // 缺省要求同步后再上行；轮询间隔必须远小于 task_check_interval_ms。
    c.cloud.requireClockSync = s.getOr<bool>("fota.cloud.require_clock_sync", true);
    std::int64_t clockPoll =
        s.getOr<std::int64_t>("fota.cloud.clock_sync_poll_interval_ms", 1000);
    requireRange("fota.cloud.clock_sync_poll_interval_ms", clockPoll, 100, 60000);
    c.cloud.clockSyncPollInterval = std::chrono::milliseconds(clockPoll);
    std::int64_t clockFloor =
        s.getOr<std::int64_t>("fota.cloud.clock_min_valid_epoch_ms", 1735689600000LL);
    // 下限 2020-01-01T00:00:00Z：更早的值无法区分 RTC 回落，失去门禁意义。
    requireRange("fota.cloud.clock_min_valid_epoch_ms", clockFloor,
                 1577836800000LL, INT64_MAX);
    c.cloud.clockMinValidEpochMs = clockFloor;

    // ---- mock.* (CCU-FOTA-DSN-CR-009 §构建与运行隔离) ----
    // 量产 preset 必须为 OFF；CI 扫描量产链接产物和配置。
    rejectUnknown(s, "fota.mock", {"enabled", "scenario_path", "virtual_clock"});
    c.mock.enabled = s.getOr<bool>("fota.mock.enabled", false);
    c.mock.scenarioPath = s.getOr<std::string>("fota.mock.scenario_path", "");
    c.mock.virtualClock = s.getOr<bool>("fota.mock.virtual_clock", false);
#ifndef FOTA_ENABLE_TEST_DOUBLES
    if (c.mock.enabled) {
        throw FotaConfigException(
            "fota.mock.enabled=true rejected in production build "
            "(FOTA_ENABLE_TEST_DOUBLES not defined)");
    }
#else
    // CCU-FOTA-DSN-CR-015 §D2：NON_PRODUCTION 启用 Mock 时场景必填。可读性与
    // 场景校验由装配层（assembleMockVehiclePorts）在开放传输前 fail-closed。
    if (c.mock.enabled && c.mock.scenarioPath.empty()) {
        throw FotaConfigException(
            "fota.mock.enabled=true requires fota.mock.scenario_path");
    }
#endif

    return c;
}

LogConfig FotaConfig::logConfigFrom(const ConfigSnapshot& s) {
    LogConfig lc;

    // common.log.*（CCU 公共配置，FOTA 只读消费）
    lc.schema_version =
        static_cast<std::uint32_t>(s.getOr<std::int64_t>("common.log.schema_version", 1));
    lc.level = logLevelFromString(s.getOr<std::string>("common.log.level", "INFO"));
    lc.strict = s.getOr<bool>("common.log.strict", false);

    lc.async_config.enabled = s.getOr<bool>("common.log.async.enabled", true);
    lc.async_config.queue_size =
        static_cast<std::uint32_t>(s.getOr<std::int64_t>("common.log.async.queue_size", 4096));
    lc.async_config.flush_interval_ms =
        static_cast<std::uint32_t>(s.getOr<std::int64_t>("common.log.async.flush_interval_ms", 1000));

    lc.console_config.enabled = s.getOr<bool>("common.log.console.enabled", true);

    lc.file_config.enabled = s.getOr<bool>("common.log.file.enabled", false);
    lc.file_config.root = s.getOr<std::string>("common.log.file.root", "/var/log/ccu");
    lc.file_config.max_file_size_mb =
        static_cast<std::uint32_t>(s.getOr<std::int64_t>("common.log.file.max_file_size_mb", 20));
    lc.file_config.max_files =
        static_cast<std::uint32_t>(s.getOr<std::int64_t>("common.log.file.max_files", 5));
    lc.file_config.total_budget_mb =
        static_cast<std::uint32_t>(s.getOr<std::int64_t>("common.log.file.total_budget_mb", 100));

    lc.redact_config.identifiers =
        s.getOr<std::string>("common.log.redact.identifiers", "mask");
    lc.redact_config.raw_payload_max_bytes =
        static_cast<std::uint32_t>(s.getOr<std::int64_t>("common.log.redact.payload_max_bytes", 256));

    lc.format = s.getOr<std::string>("common.log.format", "standard");

    // fota.log.* 服务级覆盖
    if (s.has("fota.log.level")) {
        lc.level = logLevelFromString(s.get<std::string>("fota.log.level"));
    }
    if (s.has("fota.log.modules")) {
        for (const auto& m : s.keys("fota.log.modules")) {
            lc.module_levels[m] =
                logLevelFromString(s.get<std::string>("fota.log.modules." + m));
        }
    }

    return lc;
}

SomeIpConfig FotaConfig::someIpConfigFrom(const ConfigSnapshot& s) {
    using namespace ccu::fw::someip;
    SomeIpConfig cfg;

    // application identity 固定为 ccu-fota，每进程唯一 (CR-007 §总体架构)
    cfg.application = "ccu-fota";

    // common.someip.*（公共配置，FOTA 只读消费）
    cfg.routingMode = s.getOr<bool>("common.someip.embedded_routing", false)
        ? RoutingMode::Embedded : RoutingMode::External;
    cfg.maxPayloadBytes =
        static_cast<std::size_t>(s.getOr<std::int64_t>("common.someip.max_payload_bytes", 4 * 1024 * 1024));
    cfg.maxInflightCalls =
        static_cast<std::size_t>(s.getOr<std::int64_t>("common.someip.max_inflight_calls", 1024));
    cfg.callbackQueueSize =
        static_cast<std::size_t>(s.getOr<std::int64_t>("common.someip.callback_queue_size", 2048));
    cfg.shutdownTimeout = std::chrono::milliseconds(
        s.getOr<std::int64_t>("common.someip.shutdown_timeout_ms", 5000));
    cfg.callTimeout = std::chrono::milliseconds(
        s.getOr<std::int64_t>("common.someip.call_timeout_ms", 3000));
    cfg.maxProviders =
        static_cast<std::size_t>(s.getOr<std::int64_t>("common.someip.max_providers", 64));
    cfg.maxClients =
        static_cast<std::size_t>(s.getOr<std::int64_t>("common.someip.max_clients", 64));
    cfg.maxSubscriptionsPerClient =
        static_cast<std::size_t>(s.getOr<std::int64_t>("common.someip.max_subscriptions_per_client", 256));
    cfg.maxRetryTimers =
        static_cast<std::size_t>(s.getOr<std::int64_t>("common.someip.max_retry_timers", 512));
    cfg.registryProfile =
        s.getOr<std::string>("common.someip.registry_profile", "ccu-fota");

    // discovery
    cfg.discovery.enabled = s.getOr<bool>("common.someip.discovery.enabled", true);
    cfg.discovery.initialBackoff = std::chrono::milliseconds(
        s.getOr<std::int64_t>("common.someip.discovery.initial_backoff_ms", 100));
    cfg.discovery.maxBackoff = std::chrono::milliseconds(
        s.getOr<std::int64_t>("common.someip.discovery.max_backoff_ms", 5000));
    cfg.discovery.multiplier =
        s.getOr<double>("common.someip.discovery.multiplier", 2.0);
    cfg.discovery.jitterPercent =
        static_cast<int>(s.getOr<std::int64_t>("common.someip.discovery.jitter_percent", 20));

    return cfg;
}

} // namespace ccu_fota
