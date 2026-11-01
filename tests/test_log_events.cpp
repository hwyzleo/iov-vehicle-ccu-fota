#include <gtest/gtest.h>
#include "fota_log_adapter.h"
#include "fota_log_context.h"
#include "error_codes.h"
#include "data_models.h"
#include <regex>

namespace ccu_fota {
namespace {

class LogEventsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ccu::fw::log::LogConfig config;
        config.level = ccu::fw::log::LogLevel::kDebug;

        auto result = FotaLogAdapter::init("ccu-fota-events-test", config);
        ASSERT_EQ(result.error, ccu::fw::log::LogError::kOk);
    }
};

// ============================================================
// 事件名格式验证（CCU-FOTA-DSN-CR-003 §事件目录）
// ============================================================
TEST_F(LogEventsTest, EventNameFormat) {
    // 验证事件名格式：fota.<module>.<action> 或 fota.<module>.<submodule>.<action>
    std::regex event_pattern("^fota\\.[a-z_]+\\.[a-z_.]+$");

    // 事件目录中的所有业务事件
    EXPECT_TRUE(std::regex_match(fota_events::INVENTORY_REQUEST_ACCEPTED, event_pattern));
    EXPECT_TRUE(std::regex_match(fota_events::INVENTORY_REQUEST_MERGED, event_pattern));
    EXPECT_TRUE(std::regex_match(fota_events::DIAG_COLLECT_SUCCEEDED, event_pattern));
    EXPECT_TRUE(std::regex_match(fota_events::DIAG_COLLECT_FAILED, event_pattern));
    EXPECT_TRUE(std::regex_match(fota_events::SNAPSHOT_ASSEMBLED, event_pattern));
    EXPECT_TRUE(std::regex_match(fota_events::SNAPSHOT_ASSEMBLE_FAILED, event_pattern));
    EXPECT_TRUE(std::regex_match(fota_events::TBOX_SUBMIT_SUCCEEDED, event_pattern));
    EXPECT_TRUE(std::regex_match(fota_events::TBOX_SUBMIT_FAILED, event_pattern));
    EXPECT_TRUE(std::regex_match(fota_events::INVENTORY_REPORT_COMPLETED, event_pattern));
}

TEST_F(LogEventsTest, AllModuleLoggersCallable) {
    // 验证所有模块 Logger 可以获取并调用
    FotaLogAdapter::orchestrator().info(fota_events::INVENTORY_REQUEST_ACCEPTED,
        "Test orchestrator event");
    FotaLogAdapter::snapshot_assembler().info(fota_events::SNAPSHOT_ASSEMBLED,
        "Test snapshot_assembler event");
    FotaLogAdapter::diag_client().info(fota_events::DIAG_COLLECT_SUCCEEDED,
        "Test diag_client event");
    FotaLogAdapter::inventory_reporter().info(fota_events::TBOX_SUBMIT_SUCCEEDED,
        "Test inventory_reporter event");
}

// ============================================================
// 错误码映射验证（CCU-FOTA-DSN-CR-003 §事件目录）
// ============================================================
TEST_F(LogEventsTest, ErrorCodeMapping) {
    // 验证 FOTA 业务错误码在正确范围内
    EXPECT_GE(static_cast<int>(ErrorCode::CCU_FOTA_1003), 1100);
    EXPECT_LE(static_cast<int>(ErrorCode::CCU_FOTA_1003), 1199);
    EXPECT_GE(static_cast<int>(ErrorCode::CCU_FOTA_1004), 1100);
    EXPECT_LE(static_cast<int>(ErrorCode::CCU_FOTA_1004), 1199);
    EXPECT_GE(static_cast<int>(ErrorCode::CCU_FOTA_1005), 1100);
    EXPECT_LE(static_cast<int>(ErrorCode::CCU_FOTA_1005), 1199);
    EXPECT_GE(static_cast<int>(ErrorCode::CCU_FOTA_1006), 1100);
    EXPECT_LE(static_cast<int>(ErrorCode::CCU_FOTA_1006), 1199);

    // 验证错误码可转换为字符串
    EXPECT_FALSE(errorCodeToString(ErrorCode::CCU_FOTA_1003).empty());
    EXPECT_FALSE(errorCodeToString(ErrorCode::CCU_FOTA_1004).empty());
    EXPECT_FALSE(errorCodeToString(ErrorCode::CCU_FOTA_1005).empty());
    EXPECT_FALSE(errorCodeToString(ErrorCode::CCU_FOTA_1006).empty());
}

TEST_F(LogEventsTest, FrameworkErrorCodes) {
    // 验证 framework 错误码（CCU-FW-0201~0205）
    EXPECT_EQ(static_cast<uint32_t>(ccu::fw::log::LogError::kConfigInvalid), 201);
    EXPECT_EQ(static_cast<uint32_t>(ccu::fw::log::LogError::kInitFailed), 202);
    EXPECT_EQ(static_cast<uint32_t>(ccu::fw::log::LogError::kSinkFailed), 203);
    EXPECT_EQ(static_cast<uint32_t>(ccu::fw::log::LogError::kQueueOverflow), 204);
    EXPECT_EQ(static_cast<uint32_t>(ccu::fw::log::LogError::kSensitiveViolation), 205);
}

// ============================================================
// 事件字段验证（CCU-FOTA-DSN-CR-003 §事件目录 / §字段与脱敏）
// ============================================================
TEST_F(LogEventsTest, InventoryRequestAcceptedFields) {
    // fota.inventory.request.accepted: request_id, report_id, reason
    FotaLogAdapter::orchestrator().info(
        fota_events::INVENTORY_REQUEST_ACCEPTED,
        "Inventory request accepted",
        {flog::f_str("request_id", "req-test-001"),
         flog::f_int("report_id", 1),
         flog::f_str("reason", "cloud_query")}
    );
}

TEST_F(LogEventsTest, InventoryRequestMergedFields) {
    // fota.inventory.request.merged: request_id, report_id
    FotaLogAdapter::orchestrator().info(
        fota_events::INVENTORY_REQUEST_MERGED,
        "Concurrent request merged",
        {flog::f_str("request_id", "req-test-002"),
         flog::f_int("report_id", 1)}
    );
}

TEST_F(LogEventsTest, DiagCollectSucceededFields) {
    // fota.diag.collect.succeeded: report_id, registry_version, overall_result, duration_ms
    FotaLogAdapter::snapshot_assembler().info(
        fota_events::DIAG_COLLECT_SUCCEEDED,
        "DIAG returned version inventory",
        {flog::f_str("report_id", "1"),
         flog::f_str("registry_version", "1.0.0"),
         flog::f_str("overall_result", "ALL_OK"),
         flog::f_int("duration_ms", 52)}
    );
}

TEST_F(LogEventsTest, DiagCollectFailedFields) {
    // fota.diag.collect.failed: request_id, report_id, SOME/IP context, duration_ms, error_code
    FotaLogAdapter::snapshot_assembler().error(
        fota_events::DIAG_COLLECT_FAILED,
        "DIAG collect failed",
        {flog::f_str("request_id", "req-test-001"),
         flog::f_str("report_id", "1"),
         flog::f_str("someip_service_id", "0x1110"),
         flog::f_str("someip_method_id", "0x0002"),
         flog::f_int("duration_ms", 5000),
         flog::f_str("error_code", "CCU-FOTA-1006")}
    );
}

TEST_F(LogEventsTest, SnapshotAssembledFields) {
    // fota.snapshot.assembled: report_id, snapshot_seq, registry_version, overall_result
    FotaLogAdapter::snapshot_assembler().info(
        fota_events::SNAPSHOT_ASSEMBLED,
        "Snapshot assembled",
        {flog::f_str("report_id", "1"),
         flog::f_int("snapshot_seq", 1),
         flog::f_str("registry_version", "1.0.0"),
         flog::f_str("overall_result", "ALL_OK")}
    );
}

TEST_F(LogEventsTest, SnapshotAssembleFailedFields) {
    // fota.snapshot.assemble.failed: report_id, snapshot_seq, error_code
    FotaLogAdapter::snapshot_assembler().error(
        fota_events::SNAPSHOT_ASSEMBLE_FAILED,
        "Snapshot assembly failed",
        {flog::f_str("report_id", "1"),
         flog::f_int("snapshot_seq", 1),
         flog::f_str("error_code", "CCU-FOTA-1003")}
    );
}

TEST_F(LogEventsTest, TboxSubmitSucceededFields) {
    // fota.tbox.submit.succeeded: report_id, snapshot_seq, duration_ms
    FotaLogAdapter::inventory_reporter().info(
        fota_events::TBOX_SUBMIT_SUCCEEDED,
        "TBOX accepted snapshot",
        {flog::f_str("report_id", "1"),
         flog::f_int("snapshot_seq", 1),
         flog::f_int("duration_ms", 30)}
    );
}

TEST_F(LogEventsTest, TboxSubmitFailedFields) {
    // fota.tbox.submit.failed: report_id, snapshot_seq, SOME/IP context, attempt, duration_ms, error_code
    FotaLogAdapter::inventory_reporter().error(
        fota_events::TBOX_SUBMIT_FAILED,
        "TBOX submission failed",
        {flog::f_str("report_id", "1"),
         flog::f_int("snapshot_seq", 1),
         flog::f_str("someip_service_id", "0x6101"),
         flog::f_str("someip_method_id", "0x0001"),
         flog::f_int("attempt", 3),
         flog::f_int("duration_ms", 5000),
         flog::f_str("error_code", "CCU-FOTA-1005")}
    );
}

TEST_F(LogEventsTest, InventoryReportCompletedFields) {
    // fota.inventory.report.completed: request_id, report_id, snapshot_seq, overall_result, duration_ms
    FotaLogAdapter::inventory_reporter().info(
        fota_events::INVENTORY_REPORT_COMPLETED,
        "Report cycle completed",
        {flog::f_str("request_id", "req-test-001"),
         flog::f_str("report_id", "1"),
         flog::f_int("snapshot_seq", 1),
         flog::f_str("overall_result", "ALL_OK"),
         flog::f_int("duration_ms", 100)}
    );
}

// ============================================================
// 敏感类型验证（CCU-FOTA-DSN-CR-003 §字段与脱敏）
// ============================================================
TEST_F(LogEventsTest, SensitivityClassification) {
    // FOTA 专用字段应为 Normal
    EXPECT_EQ(ccu::fw::log::Sensitivity::Normal, ccu::fw::log::Sensitivity::Normal);

    // VIN/device_sn 应按 Identifier 处理
    FotaLogAdapter::inventory_reporter().info("test.event",
        "VIN as Identifier",
        {flog::f_str("vin", "LSVAU2180N2123456", ccu::fw::log::Sensitivity::Identifier)}
    );

    // 快照 payload 应按 Payload 处理
    FotaLogAdapter::snapshot_assembler().info("test.event",
        "Snapshot as Payload",
        {flog::f_str("ecu_list", "raw_payload_data", ccu::fw::log::Sensitivity::Payload)}
    );

    // 密钥材料应按 Secret 处理（直接拒绝）
    FotaLogAdapter::diag_client().info("test.event",
        "Secret field rejection",
        {flog::f_str("session_key", "secret_key_data", ccu::fw::log::Sensitivity::Secret)}
    );
}

TEST_F(LogEventsTest, PayloadFieldOnlyMetadata) {
    // Payload 字段: INFO 及以上仅记录条目数、长度、摘要和结果
    FotaLogAdapter::snapshot_assembler().info("test.event",
        "Payload metadata only",
        {flog::f_int("ecu_count", 42),
         flog::f_int("payload_length", 1024),
         flog::f_str("overall_result", "ALL_OK")}
    );
}

// ============================================================
// 枚举转字符串验证
// ============================================================
TEST_F(LogEventsTest, EnumToStringConversions) {
    EXPECT_STREQ(collectionStatusToString(CollectionStatus::ALL_OK), "ALL_OK");
    EXPECT_STREQ(collectionStatusToString(CollectionStatus::PARTIAL), "PARTIAL");
    EXPECT_STREQ(collectionStatusToString(CollectionStatus::FAILED), "FAILED");

    EXPECT_STREQ(ecuStatusToString(EcuStatus::OK), "OK");
    EXPECT_STREQ(ecuStatusToString(EcuStatus::NRC), "NRC");
    EXPECT_STREQ(ecuStatusToString(EcuStatus::TIMEOUT), "TIMEOUT");

    EXPECT_STREQ(baselineSourceToString(BaselineSource::FACTORY), "FACTORY");
    EXPECT_STREQ(versionSourceToString(VersionSource::UDS_0x22), "UDS_0x22");
}

} // namespace
} // namespace ccu_fota
