#pragma once

// =============================================================================
// include/ccu/fota/ota/ports/vehicle_condition_provider.hpp
// CCU-FOTA 车辆条件采集器端口 (CCU-FOTA-DSN-CR-009 §安装许可 / CR-011 类型校准)
// =============================================================================
// VehicleConditionProvider 生成 conditionSetVersion + ConditionSnapshot +
// localReadinessDigest + failedConditions；Mock 可按脚本变化门禁结果。仅在
// startTime <= now < endTime、任务允许且全部本地门禁通过时申请开始安装。
// =============================================================================

#include "vehicle/fota/v1/types.pb.h"

#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {

// 条件评估结果（组装进 vehicle.fota.v1.InstallPermitRequest）。
struct ConditionEvaluation {
    std::string conditionSetVersion;
    ::vehicle::fota::v1::ConditionSnapshot snapshot;
    std::string localReadinessDigest;       // 小写十六进制
    bool allGuardsPassed = false;
    std::vector<std::string> failedConditions;
};

class VehicleConditionProvider {
public:
    virtual ~VehicleConditionProvider() = default;

    // 生成条件集合、快照和本地就绪摘要。
    virtual ConditionEvaluation evaluateConditions() = 0;

    // 是否所有门禁通过（申请安装许可前复检；进入 INSTALL_STARTED 前再次复检）。
    virtual bool allGuardsPassed() = 0;
};

} // namespace ota
} // namespace ccu_fota
