// =============================================================================
// src/daemon/vehicle_side_ports.cpp
// Conservative 车内端口装配 (CCU-FOTA-DSN-CR-015 §D1/D2)
// =============================================================================

#include "ccu/fota/daemon/vehicle_side_ports.hpp"

#include "ccu/fota/ota/ports/production_ports.hpp"

namespace ccu_fota {
namespace daemon {

VehicleSidePorts assembleConservativeVehiclePorts() {
    VehicleSidePorts ports;
    ports.consent =
        std::make_shared<ota::ConservativeConsentProvider>();
    ports.downloader =
        std::make_shared<ota::ConservativePackageDownloader>();
    ports.conditions =
        std::make_shared<ota::ConservativeVehicleConditionProvider>();
    ports.executor =
        std::make_shared<ota::ConservativeInstallExecutor>();
    ports.logCollector =
        std::make_shared<ota::ConservativeLogCollector>();
    ports.mock.activated = false;
    return ports;
}

} // namespace daemon
} // namespace ccu_fota
