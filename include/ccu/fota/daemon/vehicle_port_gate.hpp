#pragma once

// =============================================================================
// include/ccu/fota/daemon/vehicle_port_gate.hpp
// 车内端口双重门禁分发 (CCU-FOTA-DSN-CR-015 §D1/D2)
// =============================================================================
// 只有装配入口（main.cpp）与装配测试包含本头文件，避免同一 inline 函数在不同
// 编译口径的 TU 中产生不一致定义（ODR）。端口实现类型与工厂声明见
// vehicle_side_ports.hpp。
// =============================================================================

#include "ccu/fota/daemon/vehicle_side_ports.hpp"

namespace ccu_fota {
namespace daemon {

inline VehicleSidePorts assembleVehicleSidePorts(const FotaConfig& config) {
#ifdef FOTA_ENABLE_TEST_DOUBLES
    if (config.mock.enabled) {
        return assembleMockVehiclePorts(config.mock);
    }
    VehicleSidePorts ports = assembleConservativeVehiclePorts();
    ports.mock.activated = false;
    ports.mock.profile = "NON_PRODUCTION";
    return ports;
#else
    if (config.mock.enabled) {
        // 运行配置不能使 OFF 构建获得 Mock 能力（与 FotaConfig::from 的校验共同
        // 构成双重保险；此处为装配边界，绝不静默降级为 Conservative）。
        throw FotaConfigException(
            "fota.mock.enabled=true rejected in production build "
            "(FOTA_ENABLE_TEST_DOUBLES=OFF)");
    }
    VehicleSidePorts ports = assembleConservativeVehiclePorts();
    ports.mock.activated = false;
    ports.mock.profile = "PRODUCTION";
    return ports;
#endif
}

} // namespace daemon
} // namespace ccu_fota
