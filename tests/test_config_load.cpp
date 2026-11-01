// =============================================================================
// tests/test_config_load.cpp - ccu-framework-config 加载/解析测试 (CCU-FOTA-DSN-CR-004)
// =============================================================================
// 验证六阶段顺序、first-match、ordered-overlay、来源标签、稳定 CWD，以及故障
// fail-closed：不可读文件、非法 YAML 均不静默回退。开发夹具 fota.yaml 可经
// stage 4（./config/<svc>.yaml）加载。

#include "ccu/fota/config/fota_config.hpp"
#include "config.h"
#include "config_types.h"
#include "config_test_util.h"

#include <gtest/gtest.h>

using ccu::fw::config::Config;
using ccu::fw::config::ConfigException;
using ccu::fw::config::LoadOptions;
using ccu::fw::config::ConfigSnapshot;
using ccu_fota::FotaConfig;
using ccu_fota_test::TempDir;

// 最小加载：common.yaml + conf.d/fota.yaml
TEST(ConfigLoadTest, MinimalLoadSucceeds) {
    TempDir root;
    root.writeCommonYaml();
    root.writeFile("conf.d/fota.yaml",
        "fota:\n  inventory:\n    max_pending_requests: 16\n");

    auto snap = Config::load("fota", LoadOptions{{root.path}, TempDir().path});
    ASSERT_NE(snap, nullptr);
    EXPECT_TRUE(snap->has("fota.inventory.max_pending_requests"));
    EXPECT_EQ(snap->get<std::int64_t>("fota.inventory.max_pending_requests"), 16);
}

// stage 1 缺失 common.yaml -> CCU-FW-0001（fail-closed）
TEST(ConfigLoadTest, MissingCommonYamlFailsClosed) {
    TempDir root;
    // 仅提供 fota.yaml，无 common.yaml
    root.writeFile("conf.d/fota.yaml", "fota:\n  inventory:\n    max_pending_requests: 1\n");

    bool threw = false;
    try {
        Config::load("fota", LoadOptions{{root.path}, TempDir().path});
    } catch (const ConfigException& e) {
        threw = true;
        EXPECT_EQ(e.code, "CCU-FW-0001");
    }
    EXPECT_TRUE(threw);
}

// 非法 YAML -> fail-closed（CCU-FW-0002）
TEST(ConfigLoadTest, IllegalYamlFailsClosed) {
    TempDir root;
    root.writeCommonYaml();
    root.writeFile("conf.d/fota.yaml",
        "fota:\n  inventory: [unterminated\n    : bad");

    bool threw = false;
    try {
        Config::load("fota", LoadOptions{{root.path}, TempDir().path});
    } catch (const ConfigException& e) {
        threw = true;
        EXPECT_EQ(e.code, "CCU-FW-0002");
    }
    EXPECT_TRUE(threw);
}

// found-but-unreadable：common.yaml 是目录而非常规文件 -> CCU-FW-0001，不回退
TEST(ConfigLoadTest, UnreadableCommonFailsClosedNoFallback) {
    TempDir root;
    root.mkdir("common.yaml");  // 目录而非常规文件
    root.writeFile("conf.d/fota.yaml", "fota:\n  log: {}\n");

    bool threw = false;
    try {
        Config::load("fota", LoadOptions{{root.path}, TempDir().path});
    } catch (const ConfigException& e) {
        threw = true;
        EXPECT_EQ(e.code, "CCU-FW-0001");
    }
    EXPECT_TRUE(threw);
}

// first-match：两个 configRoot，第一个含 common.yaml，使用第一个
TEST(ConfigLoadTest, FirstMatchCommonYaml) {
    TempDir r0;
    TempDir r1;
    r0.writeCommonYaml();
    r0.writeFile("conf.d/fota.yaml",
        "fota:\n  inventory:\n    max_pending_requests: 8\n");
    // r1 不含 common.yaml（first-match 应跳过 r1 的缺失，使用 r0）

    auto snap = Config::load("fota", LoadOptions{{r0.path, r1.path}, TempDir().path});
    ASSERT_NE(snap, nullptr);
    EXPECT_EQ(snap->get<std::int64_t>("fota.inventory.max_pending_requests"), 8);
}

// ordered-overlay：stage 5 多个 root/<svc>.yaml，后者覆盖前者
TEST(ConfigLoadTest, OrderedOverlayServiceYaml) {
    TempDir r0;
    TempDir r1;
    r0.writeCommonYaml();                 // common 仍由 r0 提供
    r0.writeFile("fota.yaml",
        "fota:\n  inventory:\n    min_report_interval_ms: 1000\n");
    r1.writeFile("fota.yaml",
        "fota:\n  inventory:\n    min_report_interval_ms: 2000\n");

    auto snap = Config::load("fota", LoadOptions{{r0.path, r1.path}, TempDir().path});
    ASSERT_NE(snap, nullptr);
    // 后者 r1 覆盖前者 r0
    EXPECT_EQ(snap->get<std::int64_t>("fota.inventory.min_report_interval_ms"), 2000);
}

// 端到端：Config::load + FotaConfig::from 产出可用的业务配置
TEST(ConfigLoadTest, EndToEndFotaConfigFromSnapshot) {
    TempDir root;
    root.writeCommonYaml();
    root.writeFile("conf.d/fota.yaml",
        "fota:\n"
        "  inventory:\n    max_pending_requests: 48\n"
        "  diag:\n    collect_timeout_ms: 12000\n    retry_max_attempts: 4\n"
        "  log:\n    level: ERROR\n");

    auto snap = Config::load("fota", LoadOptions{{root.path}, TempDir().path});
    FotaConfig c = FotaConfig::from(*snap);

    EXPECT_EQ(c.maxPendingRequests, 48u);
    EXPECT_EQ(c.diagCollectTimeout.count(), 12000);
    EXPECT_EQ(c.diagRetry.maxAttempts, 4u);
}

// 开发夹具 fota.yaml 经 stage 4（./config/<svc>.yaml）加载
TEST(ConfigLoadTest, DevFixtureLoadsViaStage4) {
    TempDir root;
    root.writeCommonYaml();                          // configRoot 提供 common
    root.writeFile("config/fota.yaml",              // stage 4: ./config/<svc>.yaml
        "fota:\n  inventory:\n    max_pending_requests: 7\n");

    // cwd = root，使 ./config/fota.yaml 命中 stage 4
    auto snap = Config::load("fota", LoadOptions{{root.path}, root.path});
    ASSERT_NE(snap, nullptr);
    EXPECT_EQ(snap->get<std::int64_t>("fota.inventory.max_pending_requests"), 7);
}

// 安全默认模板可独立加载并通过 FotaConfig::from 校验
TEST(ConfigLoadTest, DefaultTemplateLoadsAndValidates) {
    TempDir root;
    root.writeCommonYaml();
    // 复刻 config/fota.default.yaml 内容
    root.writeFile("conf.d/fota.yaml",
        "fota:\n"
        "  inventory:\n"
        "    change_detection_enabled: true\n"
        "    min_report_interval_ms: 300000\n"
        "    max_pending_requests: 32\n"
        "  diag:\n"
        "    collect_timeout_ms: 30000\n"
        "    retry_max_attempts: 2\n"
        "    retry_backoff_ms: 1000\n"
        "  log: {}\n");

    auto snap = Config::load("fota", LoadOptions{{root.path}, TempDir().path});
    FotaConfig c = FotaConfig::from(*snap);  // 不抛即通过
    EXPECT_EQ(c.minReportInterval.count(), 300000);
    EXPECT_EQ(c.maxPendingRequests, 32u);
}
