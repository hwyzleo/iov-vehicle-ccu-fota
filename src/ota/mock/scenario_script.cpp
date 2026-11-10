// =============================================================================
// src/ota/mock/scenario_script.cpp
// CCU-FOTA Mock 场景脚本加载实现 (CCU-FOTA-DSN-CR-009 / CR-015 §D3)
// =============================================================================

#include "ccu/fota/ota/mock/scenario_script.hpp"

#include "ccu/fota/fingerprint/canonical_encoder.hpp"

#include "ccu/fw/hash/sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ccu_fota {
namespace ota {
namespace mock {

using json = nlohmann::json;

namespace {

// 已知故障点（与 §13.6 / §D3 一致）。未知故障名 fail-closed：避免场景拼写错误
// 被静默忽略而“看起来成功”。
const char* const kKnownFaults[] = {
    "cloud_timeout",
    "download_disconnect",
    "etag_changed",
    "hash_failed",
    "signature_failed",
    "decryption_failed",
    "guard_failed",
    "event_drop",
    "event_reorder",
    "pause_at_safe_point",
    "install_failed",
    "rollback_required",
    "restart_after_checkpoint",
    "state_conflict",
    "log_upload_failed",
    "final_retry_pending",
    "inventory_no_task",
};

bool isValidStageResult(const std::string& r) {
    return r == "SUCCEEDED" || r == "FAILED";
}

} // namespace

bool isKnownFault(const std::string& f) {
    for (const char* known : kKnownFaults) {
        if (f == known) return true;
    }
    return false;
}

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
    if (!j.is_object()) {
        throw std::runtime_error("scenario parse failed: root must be an object");
    }

    ScenarioScript s;
    s.name = j.value("scenario", std::string("unnamed"));
    s.clock = j.value("clock", std::string("virtual"));
    if (s.clock != "virtual" && s.clock != "realtime") {
        throw std::runtime_error("scenario invalid: clock must be virtual|realtime");
    }
    if (j.contains("inventory")) {
        const auto& inv = j["inventory"];
        s.inventoryMode = inv.value("mode", std::string("FULL"));
        s.baseline = inv.value("baseline", std::string("BASE-001"));
        s.fotaMasterVersion = inv.value("fota_master_version", std::string("1.0.0"));
    }
    if (j.contains("packages")) {
        if (!j["packages"].is_array()) {
            throw std::runtime_error("scenario invalid: packages must be an array");
        }
        for (const auto& p : j["packages"]) {
            ScenarioPackage sp;
            sp.packageId = p.value("package_id", std::string());
            if (sp.packageId.empty()) {
                throw std::runtime_error(
                    "scenario invalid: package_id must not be empty");
            }
            sp.bytes = p.value("bytes", std::int64_t(0));
            if (sp.bytes < 0) {
                throw std::runtime_error(
                    "scenario invalid: package bytes must be >= 0");
            }
            sp.ecuId = p.value("ecu_id", std::string("VCU-001"));
            sp.planNodeId = p.value("plan_node_id", std::string("NODE-1"));
            sp.componentId = p.value("component_id", std::string());
            sp.packageRevision = p.value("package_revision", std::string("prev-1"));
            sp.sourceVersion = p.value("source_version", std::string("1.0.0"));
            sp.targetVersion = p.value("target_version", std::string("2.0.0"));
            sp.packageDigestHex =
                p.value("package_digest", std::string(64, 'e'));
            sp.differentialType = p.value("differential_type", std::string("full"));
            sp.encrypted = p.value("encrypted", false);
            sp.failuresBeforeSuccess = p.value("failures_before_success", 0);
            if (sp.failuresBeforeSuccess < 0) {
                throw std::runtime_error(
                    "scenario invalid: failures_before_success must be >= 0");
            }
            s.packages.push_back(std::move(sp));
        }
    }
    if (j.contains("executor")) {
        if (!j["executor"].is_object()) {
            throw std::runtime_error("scenario invalid: executor must be an object");
        }
        if (j["executor"].contains("stages")) {
            if (!j["executor"]["stages"].is_array()) {
                throw std::runtime_error(
                    "scenario invalid: executor.stages must be an array");
            }
            for (const auto& st : j["executor"]["stages"]) {
                ScenarioStage ss;
                ss.stage = st.value("stage", std::string());
                if (ss.stage.empty()) {
                    throw std::runtime_error(
                        "scenario invalid: executor stage name must not be empty");
                }
                ss.result = st.value("result", std::string("SUCCEEDED"));
                if (!isValidStageResult(ss.result)) {
                    throw std::runtime_error(
                        "scenario invalid: executor stage result must be "
                        "SUCCEEDED|FAILED");
                }
                if (st.contains("progress")) {
                    for (const auto& pr : st["progress"]) {
                        if (!pr.is_number_unsigned() && !pr.is_number_integer()) {
                            throw std::runtime_error(
                                "scenario invalid: progress must be integers");
                        }
                        ss.progress.push_back(pr.get<std::uint32_t>());
                    }
                }
                s.stages.push_back(std::move(ss));
            }
        }
    }
    if (j.contains("faults")) {
        if (!j["faults"].is_array()) {
            throw std::runtime_error("scenario invalid: faults must be an array");
        }
        for (const auto& f : j["faults"]) {
            const std::string fault = f.get<std::string>();
            if (!isKnownFault(fault)) {
                throw std::runtime_error("scenario invalid: unknown fault: " + fault);
            }
            s.faults.push_back(fault);
        }
    }
    return s;
}

ScenarioScript loadScenario(const std::string& path) {
    if (path.empty()) {
        throw std::runtime_error("scenario path is empty");
    }
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("scenario file not readable: " + path);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parseScenario(ss.str());
}

std::string scenarioDigestHex(const ScenarioScript& s) {
    using fingerprint::CanonicalEncoder;

    std::vector<std::vector<std::uint8_t>> pkgItems;
    pkgItems.reserve(s.packages.size());
    for (const auto& p : s.packages) {
        CanonicalEncoder e;
        e.writeStringField(1, p.packageId);
        e.writeStringField(2, p.ecuId);
        e.writeStringField(3, p.planNodeId);
        e.writeOptionalStringField(
            4, p.componentId.empty() ? std::nullopt
                                     : std::optional<std::string>(p.componentId));
        e.writeStringField(5, p.packageRevision);
        e.writeStringField(6, p.sourceVersion);
        e.writeStringField(7, p.targetVersion);
        e.writeUint64Field(8, static_cast<std::uint64_t>(p.bytes));
        e.writeStringField(9, p.packageDigestHex);
        e.writeStringField(10, p.differentialType);
        e.writeEnumField(11, p.encrypted ? 1u : 0u);
        e.writeEnumField(12, static_cast<std::uint32_t>(p.failuresBeforeSuccess));
        pkgItems.push_back(e.finalize());
    }

    std::vector<std::vector<std::uint8_t>> stageItems;
    stageItems.reserve(s.stages.size());
    for (const auto& st : s.stages) {
        CanonicalEncoder e;
        e.writeStringField(1, st.stage);
        std::vector<std::vector<std::uint8_t>> progressItems;
        progressItems.reserve(st.progress.size());
        for (std::uint32_t pr : st.progress) {
            CanonicalEncoder pe;
            pe.writeEnumField(1, pr);
            progressItems.push_back(pe.finalize());
        }
        e.writeListField(2, progressItems);
        e.writeStringField(3, st.result);
        stageItems.push_back(e.finalize());
    }

    std::vector<std::string> faults = s.faults;
    std::sort(faults.begin(), faults.end());
    std::vector<std::vector<std::uint8_t>> faultItems;
    faultItems.reserve(faults.size());
    for (const auto& f : faults) {
        CanonicalEncoder e;
        e.writeStringField(1, f);
        faultItems.push_back(e.finalize());
    }

    CanonicalEncoder e("cgw-fota-mock-scenario-v1");
    e.writeStringField(1, s.name);
    e.writeStringField(2, s.clock);
    e.writeStringField(3, s.inventoryMode);
    e.writeStringField(4, s.baseline);
    e.writeStringField(5, s.fotaMasterVersion);
    e.writeListField(6, pkgItems);
    e.writeListField(7, stageItems);
    e.writeListField(8, faultItems);
    auto bytes = e.finalize();
    return ccu::fw::hash::sha256_hex(
        ccu::fw::hash::ByteView{bytes.data(), bytes.size()});
}

} // namespace mock
} // namespace ota
} // namespace ccu_fota
