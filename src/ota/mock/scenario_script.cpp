// =============================================================================
// src/ota/mock/scenario_script.cpp
// CCU-FOTA Mock 场景脚本加载实现 (CCU-FOTA-DSN-CR-009)
// =============================================================================

#include "ccu/fota/ota/mock/scenario_script.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ccu_fota {
namespace ota {
namespace mock {

using json = nlohmann::json;

bool ScenarioScript::hasFault(const std::string& f) const {
    for (const auto& x : faults) if (x == f) return true;
    return false;
}

ScenarioScript parseScenario(const std::string& jsonText) {
    json j;
    try {
        j = json::parse(jsonText);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("scenario parse failed: ") + e.what());
    }

    ScenarioScript s;
    s.name = j.value("scenario", std::string("unnamed"));
    s.clock = j.value("clock", std::string("virtual"));
    if (j.contains("inventory")) {
        const auto& inv = j["inventory"];
        s.inventoryMode = inv.value("mode", std::string("FULL"));
        s.baseline = inv.value("baseline", std::string("BASE-001"));
        s.fotaMasterVersion = inv.value("fota_master_version", std::string("1.0.0"));
    }
    if (j.contains("packages")) {
        for (const auto& p : j["packages"]) {
            ScenarioPackage sp;
            sp.packageId = p.value("package_id", std::string());
            sp.bytes = p.value("bytes", std::int64_t(0));
            sp.failuresBeforeSuccess = p.value("failures_before_success", 0);
            s.packages.push_back(sp);
        }
    }
    if (j.contains("executor") && j["executor"].contains("stages")) {
        for (const auto& st : j["executor"]["stages"]) {
            ScenarioStage ss;
            ss.stage = st.value("stage", std::string());
            ss.result = st.value("result", std::string("SUCCEEDED"));
            if (st.contains("progress")) {
                for (const auto& pr : st["progress"]) {
                    ss.progress.push_back(pr.get<std::uint32_t>());
                }
            }
            s.stages.push_back(ss);
        }
    }
    if (j.contains("faults")) {
        for (const auto& f : j["faults"]) {
            s.faults.push_back(f.get<std::string>());
        }
    }
    return s;
}

ScenarioScript loadScenario(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("scenario file not readable: " + path);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parseScenario(ss.str());
}

} // namespace mock
} // namespace ota
} // namespace ccu_fota
