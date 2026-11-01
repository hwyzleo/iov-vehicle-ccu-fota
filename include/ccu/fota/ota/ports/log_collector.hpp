#pragma once

// =============================================================================
// include/ccu/fota/ota/ports/log_collector.hpp
// CCU-FOTA 日志采集器端口 (CCU-FOTA-DSN-CR-009 §日志 / CR-011 类型校准)
// =============================================================================
// MockLogCollector 生成无敏感数据的确定性诊断包，沿真实授权/对象上传/结果上报
// 状态机执行；对象上传数据面可由本地 Stub 模拟。文件数据面走对象存储，不进 Proto。
// =============================================================================

#include "vehicle/fota/v1/log.pb.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {

// 采集范围（来自 LogGrantRequest 的控制面元数据，不含文件内容）。
struct LogCollectScope {
    std::vector<std::string> collectionScope;
    ::vehicle::fota::v1::LogTimeRange timeRange;
    std::string redactionProfileVersion;
    std::string logType;
};

// 生成的日志包元数据（不含文件内容）。
struct LogPackage {
    std::string objectKey;
    std::string digestHex;
    std::string algorithm;        // 摘要算法，如 "sha-256"
    std::uint64_t sizeBytes = 0;
    bool generated = false;
    std::string errorCode;
};

class LogCollector {
public:
    virtual ~LogCollector() = default;

    // 按采集范围、时间范围和脱敏规则采集日志包。Mock 生成确定性无敏感数据包。
    virtual LogPackage
    collect(const LogCollectScope& scope) = 0;
};

} // namespace ota
} // namespace ccu_fota
