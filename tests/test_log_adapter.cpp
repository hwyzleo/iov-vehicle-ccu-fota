#include <gtest/gtest.h>
#include "fota_log_adapter.h"
#include "fota_log_context.h"
#include <regex>

namespace ccu_fota {
namespace {

class FotaLogAdapterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ccu::fw::log::LogConfig config;
        config.level = ccu::fw::log::LogLevel::kDebug;

        auto result = FotaLogAdapter::init("ccu-fota-test", config);
        ASSERT_EQ(result.error, ccu::fw::log::LogError::kOk);
    }
};

TEST_F(FotaLogAdapterTest, InitSucceeds) {
    // SetUp 已验证初始化成功
    SUCCEED();
}

TEST_F(FotaLogAdapterTest, IsInitializedFlag) {
    EXPECT_TRUE(FotaLogAdapter::isInitialized());
}

TEST_F(FotaLogAdapterTest, GetOrchestratorLogger) {
    auto logger = FotaLogAdapter::orchestrator();
    logger.info("test.event", "test message");
}

TEST_F(FotaLogAdapterTest, GetSnapshotAssemblerLogger) {
    auto logger = FotaLogAdapter::snapshot_assembler();
    logger.info("test.event", "test message");
}

TEST_F(FotaLogAdapterTest, GetDiagClientLogger) {
    auto logger = FotaLogAdapter::diag_client();
    logger.info("test.event", "test message");
}

TEST_F(FotaLogAdapterTest, GetInventoryReporterLogger) {
    auto logger = FotaLogAdapter::inventory_reporter();
    logger.info("test.event", "test message");
}

TEST_F(FotaLogAdapterTest, FieldTypesWork) {
    // 验证各种字段类型可以正确传递
    FotaLogAdapter::orchestrator().info("test.event",
        "Test field types",
        {flog::f_str("string_field", "value"),
         flog::f_int("int_field", 42),
         flog::f_bool("bool_field", true),
         flog::f_double("double_field", 3.14)}
    );
}

TEST_F(FotaLogAdapterTest, SensitivityLevelsWork) {
    // 验证不同敏感度级别的字段（CCU-FOTA-DSN-CR-003 §字段与脱敏）
    FotaLogAdapter::orchestrator().info("test.event",
        "Test sensitivity levels",
        {flog::f_str("normal_field", "normal"),
         flog::f_str("identifier_field", "VIN123", ccu::fw::log::Sensitivity::Identifier),
         flog::f_str("payload_field", "raw_data", ccu::fw::log::Sensitivity::Payload),
         flog::f_str("secret_field", "secret", ccu::fw::log::Sensitivity::Secret)}
    );
}

TEST_F(FotaLogAdapterTest, LogLevelMethods) {
    // 验证所有日志级别方法可调用
    auto logger = FotaLogAdapter::orchestrator();
    logger.trace("test.trace", "trace message");
    logger.debug("test.debug", "debug message");
    logger.info("test.info", "info message");
    logger.warn("test.warn", "warn message");
    logger.error("test.error", "error message");
}

TEST_F(FotaLogAdapterTest, GenerateTraceId) {
    std::string id1 = generate_trace_id();
    std::string id2 = generate_trace_id();

    // 两次生成的 ID 应不同
    EXPECT_NE(id1, id2);

    // ID 应以 "fota-trace-" 开头
    EXPECT_EQ(id1.substr(0, 11), "fota-trace-");
}

TEST_F(FotaLogAdapterTest, GenerateRequestId) {
    std::string id1 = generate_request_id();
    std::string id2 = generate_request_id();

    EXPECT_NE(id1, id2);
    EXPECT_EQ(id1.substr(0, 9), "fota-req-");
}

TEST_F(FotaLogAdapterTest, HexIdFormat) {
    // 验证十六进制 ID 格式（CCU-FW-SPEC设计 §5.4: 规范十六进制字符串）
    EXPECT_EQ(hex_id(0x1110), "0x1110");
    EXPECT_EQ(hex_id(0x0001), "0x0001");
    EXPECT_EQ(hex_id(0x6101), "0x6101");
    EXPECT_EQ(hex_id(0x1120), "0x1120");
}

} // namespace
} // namespace ccu_fota
