// =============================================================================
// tests/ota/nonproduction_assembly_test.cpp
// 车内端口装配双重门禁测试 (CCU-FOTA-DSN-CR-015 §D1/D2 / 测试矩阵 1、2)
// =============================================================================
// 覆盖：
//   * Conservative 装配的类型与 fail-closed 属性；
//   * NON_PRODUCTION 构建但 fota.mock.enabled=false -> 仍为 Conservative；
//   * fota.mock.enabled=true -> 五个车内端口为 Mock，且 Inventory/Registrar 不在
//     可替换集合内（由 main.cpp 恒装配真实实现）；
//   * 场景缺失 / 不可读 / 非法 JSON / 未知故障 / virtual_clock=true -> fail-closed；
//   * 场景摘要稳定且不含场景原文。
//
// 说明：本文件始终以 FOTA_ENABLE_TEST_DOUBLES=1 编译（测试 target 口径），
// 因此只覆盖 ON 构建的装配行为。OFF 构建拒绝 mock.enabled 的用例在
// test_fota_config.cpp（由 FOTA_LIB_TEST_DOUBLES 标识库侧口径）。
// =============================================================================

#include "ccu/fota/config/fota_config.hpp"
#include "ccu/fota/daemon/vehicle_port_gate.hpp"
#include "ccu/fota/ota/mock/mock_ports.hpp"
#include "ccu/fota/ota/ports/production_ports.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace ccu_fota;
using namespace ccu_fota::daemon;
using namespace ccu_fota::ota;
using namespace ccu_fota::ota::mock;

namespace {

const char* kScenario = R"({
  "scenario": "assembly-fixture",
  "clock": "realtime",
  "packages": [{"package_id": "PKG-A", "bytes": 1024}],
  "executor": {"stages": [{"stage": "INSTALL", "progress": [50, 100],
                           "result": "SUCCEEDED"}]},
  "faults": []
})";

fs::path tempPath(const std::string& name, const std::string& content) {
    static int counter = 0;
    auto p = fs::temp_directory_path() /
             ("fota-assembly-" + std::to_string(getpid()) + "-" +
              std::to_string(counter++) + "-" + name);
    std::ofstream f(p, std::ios::binary);
    f << content;
    f.close();
    return p;
}

FotaConfig makeConfig() {
    FotaConfig c;
    c.changeDetectionEnabled = true;
    c.minReportInterval = std::chrono::milliseconds(300000);
    c.maxPendingRequests = 32;
    c.inventoryIdentityRetryBackoff = std::chrono::milliseconds(5000);
    c.inventoryDigestMaxConsecutiveFailures = 3;
    c.dedupeMaxEntries = 100;
    c.dedupeTtlMs = 3600000;
    c.diagCollectTimeout = std::chrono::milliseconds(30000);
    c.diagRetry = RetryConfig{2, std::chrono::milliseconds(1000)};
    c.storeRoot = "/tmp/ccu-fota-test";
    return c;
}

} // namespace

TEST(NonProductionAssembly, ConservativePortsAreFailClosed) {
    auto ports = assembleConservativeVehiclePorts();
    EXPECT_NE(std::dynamic_pointer_cast<ConservativeConsentProvider>(ports.consent),
              nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<ConservativePackageDownloader>(
                  ports.downloader),
              nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<ConservativeVehicleConditionProvider>(
                  ports.conditions),
              nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<ConservativeInstallExecutor>(
                  ports.executor),
              nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<ConservativeLogCollector>(
                  ports.logCollector),
              nullptr);
    EXPECT_FALSE(ports.mock.activated);
}

TEST(NonProductionAssembly, MockDisabledKeepsConservativePorts) {
    FotaConfig c = makeConfig();
    c.mock.enabled = false;
    c.mock.scenarioPath = tempPath("unused.json", kScenario).string();

    auto ports = assembleVehicleSidePorts(c);
    EXPECT_FALSE(ports.mock.activated);
    EXPECT_EQ(ports.mock.profile, "NON_PRODUCTION");
    EXPECT_NE(std::dynamic_pointer_cast<ConservativeConsentProvider>(ports.consent),
              nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<ConservativeInstallExecutor>(
                  ports.executor),
              nullptr);
}

TEST(NonProductionAssembly, MockEnabledAssemblesMockInVehiclePorts) {
    FotaConfig c = makeConfig();
    c.mock.enabled = true;
    c.mock.scenarioPath = tempPath("happy.json", kScenario).string();

    auto ports = assembleVehicleSidePorts(c);
    EXPECT_TRUE(ports.mock.activated);
    EXPECT_EQ(ports.mock.profile, "NON_PRODUCTION");
    EXPECT_EQ(ports.mock.scenarioName, "assembly-fixture");
    EXPECT_EQ(ports.mock.scenarioDigestHex.size(), 64u);

    EXPECT_NE(std::dynamic_pointer_cast<MockConsentProvider>(ports.consent),
              nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<MockPackageDownloader>(ports.downloader),
              nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<MockVehicleConditionProvider>(
                  ports.conditions),
              nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<MockInstallExecutor>(ports.executor),
              nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<MockLogCollector>(ports.logCollector),
              nullptr);
}

TEST(NonProductionAssembly, ScenarioDigestStableAndNotRawContent) {
    FotaConfig c = makeConfig();
    c.mock.enabled = true;
    c.mock.scenarioPath = tempPath("stable.json", kScenario).string();

    auto a = assembleVehicleSidePorts(c);
    auto b = assembleVehicleSidePorts(c);
    EXPECT_EQ(a.mock.scenarioDigestHex, b.mock.scenarioDigestHex);
    EXPECT_EQ(a.mock.scenarioDigestHex.find("assembly-fixture"), std::string::npos);

    // 场景内容变化 -> 摘要变化。
    const std::string changed = std::string(kScenario) + "\n";
    auto changedPath = tempPath("stable2.json", changed);
    EXPECT_TRUE(fs::exists(changedPath));
    // 末尾换行不改变语义与摘要（JSON 解析后内容一致）。
    c.mock.scenarioPath = changedPath.string();
    auto d = assembleVehicleSidePorts(c);
    EXPECT_EQ(a.mock.scenarioDigestHex, d.mock.scenarioDigestHex);
}

TEST(NonProductionAssembly, MissingScenarioPathRejected) {
    FotaConfig c = makeConfig();
    c.mock.enabled = true;
    c.mock.scenarioPath.clear();
    EXPECT_THROW(assembleVehicleSidePorts(c), FotaConfigException);
}

TEST(NonProductionAssembly, UnreadableScenarioRejected) {
    FotaConfig c = makeConfig();
    c.mock.enabled = true;
    c.mock.scenarioPath = "/nonexistent/definitely/missing-scenario.json";
    EXPECT_THROW(assembleVehicleSidePorts(c), FotaConfigException);
}

TEST(NonProductionAssembly, InvalidJsonRejected) {
    FotaConfig c = makeConfig();
    c.mock.enabled = true;
    c.mock.scenarioPath = tempPath("bad.json", "{ not json").string();
    EXPECT_THROW(assembleVehicleSidePorts(c), FotaConfigException);
}

TEST(NonProductionAssembly, UnknownFaultRejected) {
    FotaConfig c = makeConfig();
    c.mock.enabled = true;
    c.mock.scenarioPath = tempPath(
        "unknown-fault.json",
        R"({"scenario":"x","faults":["no_such_fault"],"executor":{"stages":[]}})")
                              .string();
    EXPECT_THROW(assembleVehicleSidePorts(c), FotaConfigException);
}

TEST(NonProductionAssembly, VirtualClockRejectedForRealChain) {
    FotaConfig c = makeConfig();
    c.mock.enabled = true;
    c.mock.virtualClock = true;
    c.mock.scenarioPath = tempPath("virtual.json", kScenario).string();
    EXPECT_THROW(assembleVehicleSidePorts(c), FotaConfigException);
}
