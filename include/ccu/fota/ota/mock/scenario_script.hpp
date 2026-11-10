#pragma once

// =============================================================================
// include/ccu/fota/ota/mock/scenario_script.hpp
// CCU-FOTA Mock 场景脚本 (CCU-FOTA-DSN-CR-009 §Mock与测试桩 / CR-015 §D3)
// =============================================================================
// NON_PRODUCTION 联调场景只描述确定性“副作用行为”：安装阶段、进度、结果、
// 故障点与可选的按包失败次数。业务事实（ECU、包、版本、计划节点、大小、摘要）
// 不来自场景，而来自云端冻结任务（CR-015 §D3）；场景中的 packages[] 仅用于
// 测试用 Fake 云端构造任务快照（真实链路联调由非生产云端下发任务）。
//
// 故障点（未知故障名 fail-closed）：
//   cloud_timeout、download_disconnect、etag_changed、hash_failed、
//   signature_failed、decryption_failed、guard_failed、event_drop、
//   event_reorder、pause_at_safe_point、install_failed、rollback_required、
//   restart_after_checkpoint、state_conflict、log_upload_failed、
//   final_retry_pending、inventory_no_task。
// =============================================================================

#ifndef FOTA_ENABLE_TEST_DOUBLES
#error "scenario_script.hpp is only available with FOTA_ENABLE_TEST_DOUBLES (NON_PRODUCTION)"
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {
namespace mock {

struct ScenarioPackage {
    // ---- 测试用 Fake 云端的任务事实（真实链路联调由云端下发，不使用本字段）----
    std::string packageId;
    std::int64_t bytes = 0;
    std::string ecuId = "VCU-001";
    std::string planNodeId = "NODE-1";
    std::string componentId;
    std::string packageRevision = "prev-1";
    std::string sourceVersion = "1.0.0";
    std::string targetVersion = "2.0.0";
    std::string packageDigestHex = std::string(64, 'e');
    std::string differentialType = "full";
    bool encrypted = false;

    // 该包下载/校验失败次数（达到次数后成功）；用于断点重试场景。
    int failuresBeforeSuccess = 0;
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

// 已知故障点集合校验（未知故障抛出 std::runtime_error，fail-closed）。
bool isKnownFault(const std::string& f);

// 从 JSON 文件加载场景。失败抛 std::runtime_error。
ScenarioScript loadScenario(const std::string& path);

// 从 JSON 字符串解析场景（测试用）。
ScenarioScript parseScenario(const std::string& jsonText);

// 场景内容的确定性摘要（用于启动日志标识；不含敏感原文）。
// domain = cgw-fota-mock-scenario-v1。
std::string scenarioDigestHex(const ScenarioScript& scenario);

} // namespace mock
} // namespace ota
} // namespace ccu_fota
