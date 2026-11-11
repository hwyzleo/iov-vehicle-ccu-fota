#pragma once

#include "log.h"
#include "log_types.h"
#include <string>
#include <cstdint>

namespace ccu_fota {

// ============================================================
// FOTA 事件名常量（CCU-FOTA-DSN-CR-003 事件目录）
// ============================================================
namespace fota_events {
// Inventory request
constexpr const char* INVENTORY_REQUEST_ACCEPTED = "fota.inventory.request.accepted";
constexpr const char* INVENTORY_REQUEST_MERGED   = "fota.inventory.request.merged";
// DIAG collect
constexpr const char* DIAG_COLLECT_SUCCEEDED     = "fota.diag.collect.succeeded";
constexpr const char* DIAG_COLLECT_FAILED        = "fota.diag.collect.failed";
// DIAG 清单解码 / 安装结果登记 (CCU-FOTA-DSN-CR-013)
// 清单只记 count/digest/来源；登记只记 executionId/attemptNo/appliedCount。
constexpr const char* DIAG_INVENTORY_DECODED     = "fota.diag.inventory.decoded";
constexpr const char* DIAG_INVENTORY_DECODE_FAILED = "fota.diag.inventory.decode_failed";
constexpr const char* DIAG_REGISTRATION_REQUESTED  = "fota.diag.registration.requested";
constexpr const char* DIAG_REGISTRATION_ACCEPTED   = "fota.diag.registration.accepted";
constexpr const char* DIAG_REGISTRATION_REJECTED   = "fota.diag.registration.rejected";
constexpr const char* DIAG_REGISTRATION_DUPLICATE  = "fota.diag.registration.duplicate";
constexpr const char* DIAG_REGISTRATION_UNAVAILABLE = "fota.diag.registration.unavailable";
// Snapshot assemble
constexpr const char* SNAPSHOT_ASSEMBLED         = "fota.snapshot.assembled";
constexpr const char* SNAPSHOT_ASSEMBLE_FAILED   = "fota.snapshot.assemble.failed";
// TBOX submit
constexpr const char* TBOX_SUBMIT_SUCCEEDED      = "fota.tbox.submit.succeeded";
constexpr const char* TBOX_SUBMIT_FAILED         = "fota.tbox.submit.failed";
// Report completed
constexpr const char* INVENTORY_REPORT_COMPLETED = "fota.inventory.report.completed";
// Inventory identity (CCU-FOTA-DSN-CR-012)
// 清单版本是内容版本：日志只记 revision/mode/algorithm/digest/reason，
// 禁止输出完整 ECU 清单与 VIN 原值。
constexpr const char* INVENTORY_COLLECT_EMPTY       = "fota.inventory.collect.empty";
constexpr const char* INVENTORY_FINGERPRINT_COMPARED = "fota.inventory.fingerprint.compared";
constexpr const char* INVENTORY_REVISION_ALLOCATED  = "fota.inventory.revision.allocated";
constexpr const char* INVENTORY_REVISION_REUSED     = "fota.inventory.revision.reused";
constexpr const char* INVENTORY_DISPOSITION         = "fota.inventory.disposition";
constexpr const char* INVENTORY_IDENTITY_FAILED     = "fota.inventory.identity.failed";
// Service lifecycle
constexpr const char* SERVICE_STARTING           = "fota.service.starting";
constexpr const char* SERVICE_CONFIG_LOADED      = "fota.service.config_loaded";
constexpr const char* SERVICE_LOG_INITIALIZED    = "fota.service.log_initialized";
constexpr const char* SERVICE_PROFILE            = "fota.service.profile";
constexpr const char* SERVICE_READY              = "fota.service.ready";
constexpr const char* SERVICE_SHUTTING_DOWN      = "fota.service.shutting_down";
// NON_PRODUCTION Mock 联调 (CCU-FOTA-DSN-CR-015 §D1/D7)
// 日志只记录 profile / 场景名 / 场景摘要 / stage / 包身份，不输出场景敏感原文。
constexpr const char* MOCK_ACTIVATED             = "fota.mock.activated";
constexpr const char* MOCK_SCENARIO_FAILED       = "fota.mock.scenario.failed";
constexpr const char* MOCK_DOWNLOAD_RESULT       = "fota.mock.download.result";
constexpr const char* MOCK_EXECUTION_STAGE       = "fota.mock.execution.stage";
constexpr const char* MOCK_RESULT_ASSEMBLED      = "fota.mock.result.assembled";
// OTA 编排观测（CR-015 §D5/D7）
constexpr const char* OTA_FINAL_RESULT           = "fota.ota.final_result";
constexpr const char* OTA_EXECUTION_RESTORED     = "fota.ota.execution.restored";
// Store / persistence (CCU-FOTA-DSN-CR-005)
// 日志只记 key/phase/format_version/attempt/error_code；禁止 payload/VIN/device_sn。
constexpr const char* STORE_OPEN_FAILED          = "fota.store.open.failed";
constexpr const char* STORE_SEQUENCE_FAILED      = "fota.store.sequence.failed";
constexpr const char* STORE_LOCK_FAILED          = "fota.store.lock.failed";
constexpr const char* STORE_MIGRATION_FAILED     = "fota.store.migration.failed";
constexpr const char* STORE_RECOVERY_STARTED     = "fota.store.recovery.started";
constexpr const char* STORE_RECOVERY_COMPLETED   = "fota.store.recovery.completed";
constexpr const char* STORE_RECOVERY_BLOCKED     = "fota.store.recovery.blocked";
constexpr const char* STORE_SEQ_ALLOCATED        = "fota.store.seq.allocated";
constexpr const char* STORE_SEQ_BLOCKED          = "fota.store.seq.blocked";
// Cloud uplink gating（时钟门禁：上行绝对时间字段依赖可信系统时钟）
constexpr const char* CLOUD_CLOCK_SYNC_PENDING   = "fota.cloud.clock_sync.pending";
constexpr const char* CLOUD_CLOCK_SYNC_READY     = "fota.cloud.clock_sync.ready";
// 传输/回执解码失败可定位原因：只记 kind / framework cause code / detail，不含 payload。
constexpr const char* CLOUD_TRANSPORT_FAILED     = "fota.cloud.transport.failed";
// TBOX 通用 Method 响应帧解析结果：只记 outcome/error_code/reason/payload_type。
constexpr const char* CLOUD_TRANSPORT_FRAME      = "fota.cloud.transport.frame";
// 车辆 VIN 未解析：契约 B Envelope 身份不完整，上行会被云侧判为契约错误，禁止启动。
constexpr const char* CLOUD_VIN_UNRESOLVED       = "fota.cloud.vin.unresolved";
} // namespace fota_events

// ============================================================
// 便捷字段构造器 - 减少业务代码中的冗长写法
// ============================================================
namespace flog {
inline ccu::fw::log::Field f_str(const std::string& key, const std::string& val,
                                  ccu::fw::log::Sensitivity s = ccu::fw::log::Sensitivity::Normal) {
    return ccu::fw::log::Field(key, ccu::fw::log::FieldValue::makeString(val), s);
}
inline ccu::fw::log::Field f_int(const std::string& key, int64_t val) {
    return ccu::fw::log::Field(key, ccu::fw::log::FieldValue::makeInt(val));
}
inline ccu::fw::log::Field f_bool(const std::string& key, bool val) {
    return ccu::fw::log::Field(key, ccu::fw::log::FieldValue::makeBool(val));
}
inline ccu::fw::log::Field f_double(const std::string& key, double val) {
    return ccu::fw::log::Field(key, ccu::fw::log::FieldValue::makeDouble(val));
}
} // namespace flog

// ============================================================
// FotaLogAdapter - 封装 ccu-framework-log 的初始化和模块 Logger
//
// 模块划分（CCU-FOTA-DSN-CR-003）：
// - orchestrator:        自动/主动触发、并发合并、节流与去重
// - snapshot_assembler:  快照组装、序号和整体结果
// - diag_client:         调用 CCU-DIAG、超时和错误映射
// - inventory_reporter:  调用 TBOX-SOMEIP、重试和最终提交结果
// ============================================================
class FotaLogAdapter {
public:
    /// 初始化日志系统（在 main.cpp 中调用一次）
    static ccu::fw::log::InitResult init(
        const std::string& service,
        const ccu::fw::log::LogConfig& config
    );

    /// 获取各模块的 Logger 实例
    static ccu::fw::log::Logger orchestrator();
    static ccu::fw::log::Logger snapshot_assembler();
    static ccu::fw::log::Logger diag_client();
    static ccu::fw::log::Logger inventory_reporter();
    static ccu::fw::log::Logger store();

    /// 是否已初始化
    static bool isInitialized();

private:
    static bool s_initialized;
};

} // namespace ccu_fota
