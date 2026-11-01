#include "fota_log_adapter.h"

namespace ccu_fota {

bool FotaLogAdapter::s_initialized = false;

ccu::fw::log::InitResult FotaLogAdapter::init(
    const std::string& service,
    const ccu::fw::log::LogConfig& config
) {
    auto result = ccu::fw::log::Logger::init(service, config);
    if (result.error == ccu::fw::log::LogError::kOk) {
        s_initialized = true;
    }
    return result;
}

ccu::fw::log::Logger FotaLogAdapter::orchestrator() {
    return ccu::fw::log::Logger::get("orchestrator");
}

ccu::fw::log::Logger FotaLogAdapter::snapshot_assembler() {
    return ccu::fw::log::Logger::get("snapshot_assembler");
}

ccu::fw::log::Logger FotaLogAdapter::diag_client() {
    return ccu::fw::log::Logger::get("diag_client");
}

ccu::fw::log::Logger FotaLogAdapter::inventory_reporter() {
    return ccu::fw::log::Logger::get("inventory_reporter");
}

ccu::fw::log::Logger FotaLogAdapter::store() {
    return ccu::fw::log::Logger::get("store");
}

bool FotaLogAdapter::isInitialized() {
    return s_initialized;
}

} // namespace ccu_fota
