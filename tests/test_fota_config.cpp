// =============================================================================
// tests/test_fota_config.cpp - FotaConfig 映射单元测试 (CCU-FOTA-DSN-CR-004)
// =============================================================================
// 覆盖每个键的缺省值、类型、边界、未知字段与跨字段校验；logConfigFrom 的
// common.log.* / fota.log.* 映射。通过 ccu-framework-config 的 Config::load 构造
// 不可变快照，再经 FotaConfig::from / logConfigFrom 验证。

#include "ccu/fota/config/fota_config.hpp"
#include "config.h"
#include "config_types.h"
#include "log_types.h"
#include "config_test_util.h"

#include <gtest/gtest.h>

using namespace ccu_fota;
using ccu::fw::config::Config;
using ccu::fw::config::ConfigException;
using ccu::fw::config::LoadOptions;
using ccu::fw::config::ConfigSnapshot;
using ccu::fw::log::LogConfig;
using ccu::fw::log::LogLevel;
using ccu_fota_test::TempDir;

namespace {

const char* kFullFota = R"(
fota:
  inventory:
    change_detection_enabled: false
    min_report_interval_ms: 60000
    max_pending_requests: 64
  diag:
    collect_timeout_ms: 15000
    retry_max_attempts: 1
    retry_backoff_ms: 500
  log:
    level: WARN
    modules:
      orchestrator: WARN
      diag_client: DEBUG
)";

// 构造一个含 common.yaml + conf.d/fota.yaml 的临时根并加载快照。
std::shared_ptr<const ConfigSnapshot> loadFota(TempDir& root, const std::string& fotaYaml) {
    root.writeCommonYaml();
    root.writeFile("conf.d/fota.yaml", fotaYaml);
    return Config::load("fota", LoadOptions{{root.path}, TempDir().path});
}

} // namespace

// ---------------------------------------------------------------------------
// 缺省值：fota 仅含 log:{}，其余键缺省。
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, DefaultsWhenKeysAbsent) {
    TempDir root;
    auto snap = loadFota(root, "fota:\n  log: {}\n");
    FotaConfig c = FotaConfig::from(*snap);

    EXPECT_TRUE(c.changeDetectionEnabled);
    EXPECT_EQ(c.minReportInterval.count(), 300000);
    EXPECT_EQ(c.maxPendingRequests, 32u);
    EXPECT_EQ(c.diagCollectTimeout.count(), 30000);
    EXPECT_EQ(c.diagRetry.maxAttempts, 2u);
    EXPECT_EQ(c.diagRetry.backoff.count(), 1000);
}

// ---------------------------------------------------------------------------
// 覆盖值正确映射到类型化结构。
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, OverridesMapped) {
    TempDir root;
    auto snap = loadFota(root, kFullFota);
    FotaConfig c = FotaConfig::from(*snap);

    EXPECT_FALSE(c.changeDetectionEnabled);
    EXPECT_EQ(c.minReportInterval.count(), 60000);
    EXPECT_EQ(c.maxPendingRequests, 64u);
    EXPECT_EQ(c.diagCollectTimeout.count(), 15000);
    EXPECT_EQ(c.diagRetry.maxAttempts, 1u);
    EXPECT_EQ(c.diagRetry.backoff.count(), 500);
}

// ---------------------------------------------------------------------------
// CCU-FOTA-DSN-CR-007: someip 配置与 SomeIpConfig 构建
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, SomeIpConfigFromSnapshot) {
    TempDir root;
    // common.yaml 带有 common.someip.* 覆盖
    root.writeFile("common.yaml",
        "common:\n"
        "  log:\n"
        "    schema_version: 1\n"
        "    level: INFO\n"
        "    async: { enabled: false }\n"
        "    console: { enabled: true }\n"
        "    file: { enabled: false }\n"
        "    redact: { identifiers: mask, payload_max_bytes: 256 }\n"
        "    format: standard\n"
        "  someip:\n"
        "    embedded_routing: true\n"
        "    max_payload_bytes: 65536\n"
        "    max_inflight_calls: 128\n"
        "    max_providers: 4\n"
        "    max_clients: 4\n"
        "    shutdown_timeout_ms: 3000\n"
        "    registry_profile: ccu-fota-prod\n");
    root.writeFile("conf.d/fota.yaml",
        "fota:\n  log: {}\n");
    auto snap = ccu::fw::config::Config::load("fota", ccu::fw::config::LoadOptions{{root.path}, TempDir().path});

    FotaConfig c = FotaConfig::from(*snap);

    auto someIpCfg = FotaConfig::someIpConfigFrom(*snap);
    EXPECT_EQ(someIpCfg.application, "ccu-fota");
    EXPECT_EQ(someIpCfg.routingMode, ccu::fw::someip::RoutingMode::Embedded);
    EXPECT_EQ(someIpCfg.maxPayloadBytes, 65536u);
    EXPECT_EQ(someIpCfg.maxInflightCalls, 128u);
    EXPECT_EQ(someIpCfg.maxProviders, 4u);
    EXPECT_EQ(someIpCfg.maxClients, 4u);
    EXPECT_EQ(someIpCfg.shutdownTimeout.count(), 3000);
    EXPECT_EQ(someIpCfg.registryProfile, "ccu-fota-prod");
}

// ---------------------------------------------------------------------------
// CCU-FOTA-DSN-CR-007: someip 未知键拒绝（fail-closed）
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, SomeIpUnknownKeyRejected) {
    TempDir root;
    auto snap = loadFota(root, "fota:\n  someip:\n    bogus_key: 1\n  log: {}\n");
    EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
}

// ---------------------------------------------------------------------------
// 边界：max_pending_requests 1..1024
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, RangeMaxPendingRequests) {
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  inventory:\n    max_pending_requests: 1\n");
        EXPECT_NO_THROW(FotaConfig::from(*snap));
    }
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  inventory:\n    max_pending_requests: 1024\n");
        EXPECT_NO_THROW(FotaConfig::from(*snap));
    }
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  inventory:\n    max_pending_requests: 0\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  inventory:\n    max_pending_requests: 2000\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
}

// ---------------------------------------------------------------------------
// 边界：retry_max_attempts 0..10（diag）
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, RangeRetryAttempts) {
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  diag:\n    retry_max_attempts: 0\n");
        EXPECT_NO_THROW(FotaConfig::from(*snap));
    }
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  diag:\n    retry_max_attempts: 11\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
}

// ---------------------------------------------------------------------------
// 边界：timeout 必须为正；backoff 可为 0；min_report_interval 非负
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, RangeTimeoutsAndBackoff) {
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n  diag:\n    collect_timeout_ms: 0\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n  diag:\n    retry_backoff_ms: 0\n");
        EXPECT_NO_THROW(FotaConfig::from(*snap));
    }
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n  inventory:\n    min_report_interval_ms: 0\n");
        EXPECT_NO_THROW(FotaConfig::from(*snap));
    }
}

// ---------------------------------------------------------------------------
// 未知字段 fail-closed（additionalProperties:false 等价）
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, UnknownFieldsRejected) {
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  snapshot:\n    max_ecu_count: 100\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n  inventory:\n    change_detection_enabled: true\n    bogus: 1\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  diag:\n    collect_timeout_ms: 1\n    extra: 9\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
    // tbox 段已随死代码清理移除 -> 顶层未知键拒绝。
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  tbox:\n    submit_timeout_ms: 1\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
}

// ---------------------------------------------------------------------------
// 类型错误 fail-closed（经 ConfigSnapshot::getInt64 抛 ConfigException CCU-FW-0004）
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, TypeMismatchFailClosed) {
    // 整数字段传入非数字字符串 -> getInt64 解析失败
    TempDir root;
    auto snap = loadFota(root,
        "fota:\n  inventory:\n    max_pending_requests: \"not-a-number\"\n");
    EXPECT_THROW(FotaConfig::from(*snap), ConfigException);
}

// ---------------------------------------------------------------------------
// 秘密字段在 fota.* 中无位置：写入即按未知字段/非法内容拒绝（运行时等价）
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, SecretFieldRejected) {
    TempDir root;
    auto snap = loadFota(root, "fota:\n  token: secret\n");
    EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
}

// ---------------------------------------------------------------------------
// logConfigFrom：common.log.* 基线 + fota.log.* 服务级覆盖与模块级别
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, LogConfigFromSnapshot) {
    TempDir root;
    auto snap = loadFota(root, kFullFota);
    LogConfig lc = FotaConfig::logConfigFrom(*snap);

    EXPECT_EQ(lc.schema_version, 1u);
    EXPECT_EQ(lc.level, LogLevel::kWarn);            // fota.log.level 覆盖 common
    EXPECT_EQ(lc.async_config.queue_size, 4096u);    // 来自 common
    EXPECT_TRUE(lc.console_config.enabled);
    EXPECT_EQ(lc.module_levels["orchestrator"], LogLevel::kWarn);
    EXPECT_EQ(lc.module_levels["diag_client"], LogLevel::kDebug);
}

// ---------------------------------------------------------------------------
// logConfigFrom：无 fota.log 覆盖时回退 common.log.*
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, LogConfigFallbackToCommon) {
    TempDir root;
    auto snap = loadFota(root, "fota:\n  log: {}\n");
    LogConfig lc = FotaConfig::logConfigFrom(*snap);

    EXPECT_EQ(lc.level, LogLevel::kInfo);            // 来自 common
    EXPECT_TRUE(lc.module_levels.empty());
}

// ---------------------------------------------------------------------------
// 契约 B：fota.someip.transport.* 缺省值 / 非法值 fail-closed
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, SomeIpTransportDefaultsAndValidation) {
    // 缺省：契约 B 传输有界配置用安全默认。
    {
        TempDir root;
        auto snap = loadFota(root, "fota:\n  log: {}\n");
        FotaConfig c = FotaConfig::from(*snap);
        EXPECT_EQ(c.transport.exchangeTimeout.count(), 10000);
        EXPECT_EQ(c.transport.maxEnvelopeBytes, 262144u);
        EXPECT_EQ(c.transport.maxPayloadBytes, 262144u);
        EXPECT_EQ(c.transport.maxInFlight, 64u);
        EXPECT_EQ(c.transport.downlinkQueueCapacity, 256u);
        EXPECT_EQ(c.transport.downlinkWorkers, 1u);
        EXPECT_EQ(c.transport.inFlightTtl.count(), 30000);
        EXPECT_EQ(c.transport.availabilityWait.count(), 5000);
    }
    // 声明非法 -> fail-closed。
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n"
            "  someip:\n"
            "    transport:\n"
            "      max_in_flight: 0\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n"
            "  someip:\n"
            "    transport:\n"
            "      exchange_timeout_ms: 0\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
    // 未知键拒绝。
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n"
            "  someip:\n"
            "    transport:\n"
            "      bogus: 1\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
}

// ---------------------------------------------------------------------------
// 契约 B：fota.someip.generic_transport.* 部分声明/越界 -> fail-closed
// ---------------------------------------------------------------------------
TEST(FotaConfigTest, GenericTransportPartialOrInvalidRejected) {
    // 部分声明（只有 method，缺 event/eventgroup）-> 拒绝。
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n"
            "  someip:\n"
            "    generic_transport:\n"
            "      method_id: 2\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
    // event 越出 SOME/IP event 范围（< 0x8000）-> 拒绝。
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n"
            "  someip:\n"
            "    generic_transport:\n"
            "      method_id: 2\n"
            "      event_id: 3\n"
            "      eventgroup_id: 1\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
    // method 保留值 0xFFFF -> 拒绝。
    {
        TempDir root;
        auto snap = loadFota(root,
            "fota:\n"
            "  someip:\n"
            "    generic_transport:\n"
            "      method_id: 65535\n"
            "      event_id: 32769\n"
            "      eventgroup_id: 1\n");
        EXPECT_THROW(FotaConfig::from(*snap), FotaConfigException);
    }
}
