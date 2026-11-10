// =============================================================================
// src/daemon/mock_side_ports.cpp
// NON_PRODUCTION Mock 车内端口装配 (CCU-FOTA-DSN-CR-015 §D1/D2)
// =============================================================================
// 仅在 FOTA_ENABLE_TEST_DOUBLES=ON 的构建目录编译。量产构建不包含本文件、
// 场景源码与 Mock 符号（CR-015 §D1 量产隔离）。
// =============================================================================

#include "ccu/fota/daemon/vehicle_side_ports.hpp"

#ifndef FOTA_ENABLE_TEST_DOUBLES
#error "mock_side_ports.cpp must be compiled only with FOTA_ENABLE_TEST_DOUBLES (NON_PRODUCTION)"
#endif

#include "ccu/fota/ota/mock/mock_ports.hpp"
#include "ccu/fota/ota/mock/scenario_script.hpp"

#include "fota_log_adapter.h"

#include <stdexcept>
#include <string>

namespace ccu_fota {
namespace daemon {

namespace mock = ota::mock;

namespace {

mock::ScenarioScript loadValidatedScenario(const MockConfig& config) {
    if (config.scenarioPath.empty()) {
        throw FotaConfigException(
            "fota.mock.enabled=true requires fota.mock.scenario_path");
    }
    // 真实链路联调必须使用可信系统时间：虚拟时钟会污染 Envelope TTL、安装窗口
    // 与 DIAG 登记时间语义，直接拒绝（CR-015 §D2）。
    if (config.virtualClock) {
        throw FotaConfigException(
            "fota.mock.virtual_clock=true rejected for NON_PRODUCTION real-chain "
            "daemon; virtual clock is only allowed in isolated tests");
    }
    try {
        return mock::loadScenario(config.scenarioPath);
    } catch (const FotaConfigException&) {
        throw;
    } catch (const std::exception& e) {
        throw FotaConfigException(std::string("mock scenario invalid: ") + e.what());
    }
}

} // namespace

VehicleSidePorts assembleMockVehiclePorts(const MockConfig& config) {
    mock::ScenarioScript scenario = loadValidatedScenario(config);

    VehicleSidePorts ports;
    ports.consent = std::make_shared<mock::MockConsentProvider>();
    ports.downloader = std::make_shared<mock::MockPackageDownloader>(scenario);
    ports.conditions = std::make_shared<mock::MockVehicleConditionProvider>();
    ports.executor = std::make_shared<mock::MockInstallExecutor>(scenario);
    ports.logCollector = std::make_shared<mock::MockLogCollector>();
    ports.mock.activated = true;
    ports.mock.profile = "NON_PRODUCTION";
    ports.mock.scenarioName = scenario.name;
    ports.mock.scenarioDigestHex = mock::scenarioDigestHex(scenario);
    return ports;
}

} // namespace daemon
} // namespace ccu_fota
