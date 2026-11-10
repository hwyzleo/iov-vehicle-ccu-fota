#pragma once

// =============================================================================
// include/ccu/fota/ota/fota_digest.hpp
// OTA 业务摘要规范化 (CCU-FOTA-DSN-CR-015 §D4)
// =============================================================================
// 背景：CR-015 之前的 Mock 路径使用固定占位摘要（64 个 b/c/f/m），云端无法做
// 幂等校验，重试/恢复也可能产生不一致内容。本模块为以下五类业务对象提供
// 版本化、长度前缀、确定性规范化输入，并经 ccu-framework-hash 计算 SHA-256：
//   * packageManifestDigest  - 冻结任务 package_list 内容
//   * stageResultDigest      - 单包下载/校验阶段结果
//   * eventDigest            - 执行事件（不含本地 sequenceNo 与时间）
//   * resultDigest           - 最终结果、ECU 结果、失败摘要与回滚事实
//   * localReadinessDigest   - 车辆条件集合与快照（不含采集时间）
//
// 不变量（与 CCU-FOTA-DSN-CR-006 §10.2 一致）：
//   * 长度前缀二进制、大端、missing/null/present 分离、列表按稳定键全序排序；
//   * 排除时间戳、本地序号分配状态、控制 revision 等非业务内容；
//   * 变更字段集合/顺序/枚举编码 = 摘要域版本升级，必须更换 domain 字符串并
//     同步 golden 向量（不得原地修改既有 domain 语义）；
//   * 只产生小写 64 字符 Hex；不实现私有 SHA-256，不新增 vehicle.fota.v1 wire 字段。
// =============================================================================

#include "vehicle/fota/v1/execution.pb.h"
#include "vehicle/fota/v1/package.pb.h"
#include "vehicle/fota/v1/task.pb.h"
#include "vehicle/fota/v1/types.pb.h"

#include <string>
#include <string_view>

namespace ccu_fota {
namespace ota {
namespace digest {

// 摘要域（canonicalization ID）。变更输入语义须递增版本号。
constexpr const char* kPackageManifestDomain = "cgw-fota-package-manifest-v1";
constexpr const char* kStageResultDomain = "cgw-fota-stage-result-v1";
constexpr const char* kExecutionEventDomain = "cgw-fota-execution-event-v1";
constexpr const char* kFinalResultDomain = "cgw-fota-final-result-v1";
constexpr const char* kLocalReadinessDomain = "cgw-fota-local-readiness-v1";

// 冻结任务包清单摘要：按 (ecuId, packageId, packageRevision, planNodeId) 稳定排序，
// 覆盖包版本、源/目标版本、大小、包摘要、differential_type 与 encrypted。
std::string packageManifestDigestHex(
    const ::vehicle::fota::v1::VehicleTaskSnapshot& task);

// 下载/校验阶段结果摘要。排除 stage_result_id、verified_at_ms、duration_ms。
std::string stageResultDigestHex(
    const ::vehicle::fota::v1::StageResultReport& stageResult);

// 执行事件摘要。覆盖 eventId/attempt/包/ECU/计划节点/stage/status/progress/版本/
// 错误；排除 sequence_no 与 occurred_at_ms（重放必须保持原摘要）。
std::string executionEventDigestHex(
    const ::vehicle::fota::v1::ExecutionEvent& event);

// 最终结果摘要。覆盖 result、基线、回滚事实、ECU 结果与失败摘要；
// 排除 final_sequence_no、控制 revision、pending control ids 与时间戳。
std::string finalResultDigestHex(
    const ::vehicle::fota::v1::FinalResultReport& result);

// 本地就绪摘要。覆盖条件集合版本与条件快照；排除 collected_at_ms。
std::string localReadinessDigestHex(
    std::string_view conditionSetVersion,
    const ::vehicle::fota::v1::ConditionSnapshot& snapshot);

} // namespace digest
} // namespace ota
} // namespace ccu_fota
