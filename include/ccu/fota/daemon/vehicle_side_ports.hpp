#pragma once

// =============================================================================
// include/ccu/fota/daemon/vehicle_side_ports.hpp
// 车内副作用端口装配门禁 (CCU-FOTA-DSN-CR-015 §D1/D2)
// =============================================================================
// 唯一装配入口：main.cpp 与 daemon/装配测试共用同一选择逻辑。
//
// 双重门禁（缺一不可）：
//   1. 构建门禁：FOTA_ENABLE_TEST_DOUBLES=ON（独立 NON_PRODUCTION 构建目录）；
//      OFF 构建在运行配置 fota.mock.enabled=true 时直接 fail-closed。
//   2. 运行门禁：fota.mock.enabled=true。
// ON 构建但 enabled=false 时仍装配 Conservative 端口（不隐式启用 Mock）。
//
// 本函数只选择 Consent / PackageDownloader / VehicleCondition / InstallExecutor /
// LogCollector 五个车内副作用端口。Inventory、InstalledVersionRegistrar、
// ClockSync、Cloud/Transport 始终由 main.cpp 装配为真实实现，不在此替换。
//
// 真实组件失败不得触发动态回退 Mock：选择在 daemon 装配前一次性完成。
// =============================================================================

#include "ccu/fota/config/fota_config.hpp"
#include "ccu/fota/ota/ports/consent_provider.hpp"
#include "ccu/fota/ota/ports/install_executor.hpp"
#include "ccu/fota/ota/ports/log_collector.hpp"
#include "ccu/fota/ota/ports/package_downloader.hpp"
#include "ccu/fota/ota/ports/vehicle_condition_provider.hpp"

#include <memory>
#include <string>

namespace ccu_fota {
namespace daemon {

// Mock 激活信息（供启动日志与测试断言；不包含场景敏感原文）。
struct MockActivation {
    bool activated = false;
    // "PRODUCTION" / "NON_PRODUCTION"：由构建 profile 决定。
    std::string profile;
    std::string scenarioName;
    std::string scenarioDigestHex;
};

struct VehicleSidePorts {
    std::shared_ptr<ota::ConsentProvider> consent;
    std::shared_ptr<ota::PackageDownloader> downloader;
    std::shared_ptr<ota::VehicleConditionProvider> conditions;
    std::shared_ptr<ota::InstallExecutor> executor;
    std::shared_ptr<ota::LogCollector> logCollector;
    MockActivation mock;
};

// Conservative（fail-closed）车内端口：量产唯一路径，也是 NON_PRODUCTION 未启用
// Mock 时的路径。永不伪造成功、永不自动同意、永不自动安装。
VehicleSidePorts assembleConservativeVehiclePorts();

#ifdef FOTA_ENABLE_TEST_DOUBLES
// NON_PRODUCTION Mock 车内端口。校验 fota.mock.enabled 约束：
//   * scenario_path 必填、可读、JSON/场景校验通过；
//   * fota.mock.virtual_clock=true 拒绝（真实链路必须使用可信系统时间）。
// 校验失败抛 FotaConfigException（在开放传输与业务触发前终止）。
VehicleSidePorts assembleMockVehiclePorts(const MockConfig& config);
#endif

} // namespace daemon
} // namespace ccu_fota
