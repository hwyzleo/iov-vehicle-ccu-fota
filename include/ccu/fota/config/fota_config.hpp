#pragma once

// =============================================================================
// include/ccu/fota/config/fota_config.hpp
// CCU-FOTA 类型化配置映射 (CCU-FOTA-DSN-CR-004)
// =============================================================================
// 将 ccu-framework-config 的不可变 ConfigSnapshot 映射为只读 FotaConfig。
// 业务代码不直接接触 yaml-cpp；所有类型/范围/跨字段校验在 from() 中完成，
// 任一失败抛 FotaConfigException（fail-closed，映射为 CCU-FW-00xx 配置错误）。
//
// fota.* 契约仅含 inventory / diag / someip / log。Service/Instance/Method ID、
// 协议与端口不进入本结构体，继续来自整车 SOME/IP Service Registry。
// =============================================================================

#include "config_types.h"   // ccu::fw::config::ConfigSnapshot
#include "log_types.h"      // ccu::fw::log::LogConfig
#include "ccu/fw/someip/types.hpp"  // ccu::fw::someip::SomeIpConfig (CR-007)

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace ccu_fota {

// ---------------------------------------------------------------------------
// FotaConfigException - 配置校验失败（fail-closed）
// ---------------------------------------------------------------------------
class FotaConfigException : public std::runtime_error {
public:
    explicit FotaConfigException(const std::string& message);
};

// ---------------------------------------------------------------------------
// RetryConfig - 采集/提交重试策略
// ---------------------------------------------------------------------------
struct RetryConfig {
    std::uint32_t maxAttempts;
    std::chrono::milliseconds backoff;
};

// ---------------------------------------------------------------------------
// CloudConfig - fota.cloud.* 车云 FOTA 编排配置 (CCU-FOTA-DSN-CR-009 §13.8 / CR-011)
// ---------------------------------------------------------------------------
struct CloudConfig {
    bool enabled = true;
    std::string protocolVersion = "fota-v1";
    std::chrono::milliseconds taskCheckInterval{60000};
    bool reconcileOnStart = true;
    std::uint32_t eventOutboxMax = 4096;
    std::uint32_t eventBatchMax = 64;
    std::chrono::milliseconds controlAckTimeout{5000};
    std::chrono::milliseconds cloudCallTimeout{10000};
    // 时钟门禁 (上行 Envelope 的 timestamp_ms/expire_at_ms 依赖可信 CLOCK_REALTIME)
    bool requireClockSync = true;
    std::chrono::milliseconds clockSyncPollInterval{1000};
    std::int64_t clockMinValidEpochMs = 1735689600000LL;  // 2025-01-01T00:00:00Z
};

// ---------------------------------------------------------------------------
// SomeIpTransportConfig - 契约 B SOME/IP 通用传输有界配置 (CCU-FOTA-DSN-CR-010/011)
// fota.someip.transport.*；配置缺失用缺省值，声明但非法则 fail-closed。
// ---------------------------------------------------------------------------
struct SomeIpTransportConfig {
    std::chrono::milliseconds exchangeTimeout{10000};  // 单次 exchange/publish 本地超时上限
    std::size_t maxEnvelopeBytes = 262144;             // 序列化 Envelope 上限（256 KiB）
    std::size_t maxPayloadBytes = 262144;              // payload bytes 上限（256 KiB）
    std::size_t maxInFlight = 64;                      // 有界 in-flight correlation 表容量
    std::size_t downlinkQueueCapacity = 256;           // 下行有界队列容量
    std::size_t downlinkWorkers = 1;                   // 下行独立 executor 线程数
    std::chrono::milliseconds inFlightTtl{30000};      // correlation 条目清理 TTL
    std::chrono::milliseconds availabilityWait{5000};  // start() 有界等待可用时间
};

// ---------------------------------------------------------------------------
// GenericTransportSpec - TBOX 通用消息服务 SOME/IP 寻址（Registry/IDL/运行配置）
// Service/Instance 已由 Registry 分配（0x6101/0x0001/TCP 56101，CR-002）；
// generic Method/Event/Eventgroup ID 必须来自 Registry 分配并经运行配置注入，
// 禁止在代码中猜测或硬编码尚未分配的 ID。全 0 = 未分配（契约 B blocked）。
// ---------------------------------------------------------------------------
struct GenericTransportSpec {
    std::uint16_t methodId = 0;      // Registry 分配的 generic exchange/publish Method
    std::uint16_t eventId = 0;       // Registry 分配的 generic 下行 Event
    std::uint16_t eventgroupId = 0;  // Registry 分配的 Eventgroup
    std::uint32_t interfaceMajor = 1;
    bool anyDeclared() const { return methodId != 0 || eventId != 0 || eventgroupId != 0; }
};

// ---------------------------------------------------------------------------
// MockConfig - fota.mock.* 测试桩配置 (CCU-FOTA-DSN-CR-009 §构建与运行隔离)
// 量产 profile 必须为 OFF；启用时健康状态/日志/指标标记 NON_PRODUCTION。
// ---------------------------------------------------------------------------
struct MockConfig {
    bool enabled = false;
    std::string scenarioPath;
    bool virtualClock = false;
};

// ---------------------------------------------------------------------------
// FotaConfig - 不可变 fota.* 业务配置
// ---------------------------------------------------------------------------
struct FotaConfig {
    // inventory.*
    bool changeDetectionEnabled;
    std::chrono::milliseconds minReportInterval;
    std::uint32_t maxPendingRequests;
    // 清单身份（CCU-FOTA-DSN-CR-012）
    std::chrono::milliseconds inventoryIdentityRetryBackoff;
    std::uint32_t inventoryDigestMaxConsecutiveFailures;
    // dedupe 边界 (CCU-FOTA-DSN-CR-005)
    std::uint32_t dedupeMaxEntries;
    std::int64_t dedupeTtlMs;

    // diag.*
    std::chrono::milliseconds diagCollectTimeout;
    RetryConfig diagRetry;

    // fota.someip.transport.* / fota.someip.generic_transport.* (CR-010/011 契约 B)
    SomeIpTransportConfig transport;
    GenericTransportSpec genericTransport;

    // ota.* -> fota.cloud.* (CCU-FOTA-DSN-CR-009 / CR-011 命名收敛)
    CloudConfig cloud;

    // mock.* (CCU-FOTA-DSN-CR-009)
    MockConfig mock;

    // store (CCU-FOTA-DSN-CR-005)：common.store.root，缺省 /var/lib/ccu
    std::string storeRoot;

    // 从不可变 ConfigSnapshot 构建已校验的 FotaConfig。
    // 抛 FotaConfigException：缺省值以外的不合法类型、越界值、未知字段或跨字段冲突。
    static FotaConfig from(const ccu::fw::config::ConfigSnapshot& snapshot);

    // 从同一快照读取 common.log.* 与 fota.log.*，构建 Logger 配置。
    // Logger 初始化与业务配置共享同一不可变视图。
    static ccu::fw::log::LogConfig
    logConfigFrom(const ccu::fw::config::ConfigSnapshot& snapshot);

    // CCU-FOTA-DSN-CR-007: 从 common.someip.* 与 fota.someip.* 构建 framework
    // SomeIpConfig。application 固定为 "ccu-fota"；registryProfile 来自 common。
    // someip 不读 YAML，host 注入不可变配置 (CR-007 §总体架构)。
    static ccu::fw::someip::SomeIpConfig
    someIpConfigFrom(const ccu::fw::config::ConfigSnapshot& snapshot);
};

} // namespace ccu_fota
