#pragma once

// =============================================================================
// include/ccu/fota/ota/mock/scenario_script.hpp
// CCU-FOTA Mock 场景脚本 (CCU-FOTA-DSN-CR-009 §Mock与测试桩, US-017)
// =============================================================================

#ifndef FOTA_ENABLE_TEST_DOUBLES
#error "scenario_script.hpp is only available with FOTA_ENABLE_TEST_DOUBLES (NON_PRODUCTION)"
#endif
// Mock 场景以 JSON 描述（设计允许 JSON/YAML 用于 Mock、调试和展示；本实现用 JSON
// 复用 nlohmann_json，避免引入 yaml-cpp 直接依赖）。脚本驱动 Inventory、packages、
// executor 阶段进度/结果与故障点。Mock 使用确定性时间/随机源与可复现脚本。
//
// 故障点：cloud_timeout、download_disconnect、etag_changed、hash_failed、
// signature_failed、guard_failed、event_drop、event_reorder、pause_at_safe_point、
// install_failed、rollback_required、restart_after_checkpoint、state_conflict、
// log_upload_failed。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {
namespace mock {

struct ScenarioPackage {
    std::string packageId;
    std::int64_t bytes = 0;
    int failuresBeforeSuccess = 0;   // 下载/校验失败次数后成功
};

struct ScenarioStage {
    std::string stage;               // INSTALL / REBOOT / POST_CHECK / ROLLBACK
    std::vector<std::uint32_t> progress; // 进度序列
    std::string result;              // SUCCEEDED / FAILED
};

struct ScenarioScript {
    std::string name;
    std::string clock = "virtual";   // virtual / realtime
    std::string inventoryMode = "FULL";
    std::string baseline = "BASE-001";
    std::string fotaMasterVersion = "1.0.0";
    std::vector<ScenarioPackage> packages;
    std::vector<ScenarioStage> stages;
    std::vector<std::string> faults; // 故障点列表

    // 是否包含某故障点。
    bool hasFault(const std::string& f) const;
};

// 从 JSON 文件加载场景。失败抛 std::runtime_error。
ScenarioScript loadScenario(const std::string& path);

// 从 JSON 字符串解析场景（测试用）。
ScenarioScript parseScenario(const std::string& jsonText);

} // namespace mock
} // namespace ota
} // namespace ccu_fota
