// =============================================================================
// tests/someip/diag_installed_version_registrar_test.cpp
// CCU-DIAG 安装结果登记适配器单元测试 (CCU-FOTA-DSN-CR-013 §测试矩阵 单元 3)
// -----------------------------------------------------------------------------
// 覆盖：method_id=0 -> 不调用并返回 UNAVAILABLE（Registry 未分配）；
// 请求 payload 构造（items 映射自冻结包清单）；四态处置映射；传输失败映射；
// 缺 registrationId -> 可重试失败；功能禁用 -> 能力不可用。
// 使用真实 SomeIpRuntime（FakeSomeIpBackend）+ 真 Provider 往返，不依赖后端私有 API。
// =============================================================================

#include "fota_someip_test_util.h"

#include "ccu/fota/someip/diag_installed_version_registrar.hpp"
#include "ccu/fota/ota/inventory_error_codes.hpp"
#include "ccu/fw/someip/runtime.hpp"
#include "ccu/fw/someip/types.hpp"
#include "constants.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace ccu_fota;
using namespace ccu_fota::ota;
using namespace ccu_fota_test;
namespace someip_fw = ccu::fw::someip;

namespace {

constexpr someip_fw::MethodId kRegistrationMethod = 0x0005;

InstalledVersionsReport makeReport() {
    InstalledVersionsReport r;
    r.vehicleTaskId = "VT-001";
    r.taskRevision = 3;
    r.executionId = "EXE-1";
    r.attemptNo = 1;
    r.idempotencyKey = "EXE-1:1";
    r.appliedAtMs = 1794000000000LL;
    r.items = {{"BMS", "PKG-BMS", "r1", "2.0.0"}, {"VCU", "PKG-VCU", "r1", "2.0.0"}};
    return r;
}

someip_fw::Payload toPayload(const std::string& s) {
    return someip_fw::Payload(s.begin(), s.end());
}

// 可控 DIAG 侧：记录收到的 payload，按脚本返回响应或错误。
struct FakeDiagEndpoint {
    std::string lastRequest;
    int callCount = 0;
    bool failTransport = false;
    std::string responseJson;
};

class RegistrarTest : public ::testing::Test {
protected:
    std::shared_ptr<InlineExecutor> exec_;
    someip_fw::SomeIpRuntime rt_;
    someip_fw::Provider provider_;
    std::shared_ptr<FakeDiagEndpoint> endpoint_;
    std::shared_ptr<someip::DiagInstalledVersionRegistrar> registrar_;

    void SetUp() override {
        exec_ = std::make_shared<InlineExecutor>();
        rt_ = someip_fw::SomeIpRuntime::create(makeTestConfig(), exec_);
        rt_.start();
        endpoint_ = std::make_shared<FakeDiagEndpoint>();
        endpoint_->responseJson =
            R"({"schemaVersion":1,"disposition":"APPLIED","registrationId":"REG-1",
                "appliedCount":2,"rejectedItems":[],"effectiveUpdatedAt":"2026-11-08T10:05:00Z"})";

        someip_fw::ServiceKey key{DEFAULT_DIAG_SERVICE_ID, DEFAULT_DIAG_INSTANCE_ID};
        provider_ = rt_.createProvider(key, {1, 0});
        auto ep = endpoint_;
        provider_.registerMethod(kRegistrationMethod,
            [ep](const someip_fw::RequestContext&, someip_fw::PayloadView payload) {
                ++ep->callCount;
                ep->lastRequest.assign(
                    reinterpret_cast<const char*>(payload.data), payload.size);
                if (ep->failTransport) {
                    return someip_fw::MethodResult::error(0x05, "transport failure");
                }
                return someip_fw::MethodResult::ok(toPayload(ep->responseJson));
            });
        provider_.offer();

        auto client = std::make_shared<someip_fw::Client>(rt_.createClient(key, {1, 0}));
        client->requestService();
        someip::DiagInstalledVersionRegistrarConfig cfg;
        cfg.methodId = kRegistrationMethod;
        cfg.timeout = std::chrono::milliseconds(2000);
        registrar_ = std::make_shared<someip::DiagInstalledVersionRegistrar>(
            client, cfg);
        for (int i = 0; i < 1000 &&
                        registrar_->available() == false; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void TearDown() override {
        provider_.stopOffer();
        rt_.stop();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 正常路径：请求 payload 字段/items 映射 + APPLIED 处置
// ---------------------------------------------------------------------------
TEST_F(RegistrarTest, RegistrationSendsFrozenPayloadAndMapsApplied) {
    ASSERT_TRUE(registrar_->available());
    CallContext ctx;
    auto result = registrar_->registerInstalledVersions(makeReport(), ctx);

    EXPECT_EQ(result.disposition, RegistrationDisposition::Applied);
    EXPECT_TRUE(result.completed());
    EXPECT_EQ(result.registrationId, "REG-1");
    EXPECT_EQ(result.appliedCount, 2u);
    EXPECT_TRUE(result.rejectedItems.empty());
    EXPECT_EQ(endpoint_->callCount, 1);

    // 冻结 payload：schemaVersion + 身份字段 + items（installedVersion=targetVersion）
    auto j = nlohmann::json::parse(endpoint_->lastRequest);
    EXPECT_EQ(j.at("schemaVersion").get<int>(), 1);
    EXPECT_EQ(j.at("vehicleTaskId").get<std::string>(), "VT-001");
    EXPECT_EQ(j.at("taskRevision").get<std::uint64_t>(), 3u);
    EXPECT_EQ(j.at("executionId").get<std::string>(), "EXE-1");
    EXPECT_EQ(j.at("attemptNo").get<std::uint32_t>(), 1u);
    EXPECT_EQ(j.at("idempotencyKey").get<std::string>(), "EXE-1:1");
    EXPECT_EQ(j.at("appliedAtMs").get<std::int64_t>(), 1794000000000LL);
    ASSERT_EQ(j.at("items").size(), 2u);
    EXPECT_EQ(j.at("items")[0].at("ecuId").get<std::string>(), "BMS");
    EXPECT_EQ(j.at("items")[0].at("packageId").get<std::string>(), "PKG-BMS");
    EXPECT_EQ(j.at("items")[0].at("packageRevision").get<std::string>(), "r1");
    EXPECT_EQ(j.at("items")[0].at("installedVersion").get<std::string>(), "2.0.0");
}

// ---------------------------------------------------------------------------
// method_id=0（Registry 未分配）：能力关闭，不调用 DIAG，不猜 ID
// ---------------------------------------------------------------------------
TEST(RegistrarNoMethodIdTest, UnallocatedMethodIdNeverCallsDiag) {
    auto exec = std::make_shared<InlineExecutor>();
    auto rt = someip_fw::SomeIpRuntime::create(makeTestConfig(), exec);
    rt.start();
    someip_fw::ServiceKey key{DEFAULT_DIAG_SERVICE_ID, DEFAULT_DIAG_INSTANCE_ID};
    auto provider = rt.createProvider(key, {1, 0});
    int calls = 0;
    provider.registerMethod(kRegistrationMethod,
        [&calls](const someip_fw::RequestContext&, someip_fw::PayloadView) {
            ++calls;
            return someip_fw::MethodResult::ok(toPayload(
                R"({"schemaVersion":1,"disposition":"APPLIED","registrationId":"R"})"));
        });
    provider.offer();

    auto client = std::make_shared<someip_fw::Client>(rt.createClient(key, {1, 0}));
    client->requestService();
    someip::DiagInstalledVersionRegistrarConfig cfg;  // methodId = 0
    someip::DiagInstalledVersionRegistrar registrar(client, cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_FALSE(registrar.available());
    CallContext ctx;
    auto result = registrar.registerInstalledVersions(makeReport(), ctx);
    EXPECT_EQ(result.disposition, RegistrationDisposition::Unavailable);
    EXPECT_EQ(result.errorCode, inventory_error::kRegistrationIdUnallocated);
    EXPECT_EQ(calls, 0);
    provider.stopOffer();
    rt.stop();
}

// ---------------------------------------------------------------------------
// 功能禁用：能力不可用（不是“跳过登记”的许可）
// ---------------------------------------------------------------------------
TEST(RegistrarDisabledTest, DisabledNeverCallsDiag) {
    // 空 Client：禁用状态下不应触碰。
    someip::DiagInstalledVersionRegistrarConfig cfg;
    cfg.methodId = kRegistrationMethod;
    cfg.enabled = false;
    someip::DiagInstalledVersionRegistrar registrar(nullptr, cfg);
    EXPECT_FALSE(registrar.available());
    CallContext ctx;
    auto result = registrar.registerInstalledVersions(makeReport(), ctx);
    EXPECT_EQ(result.disposition, RegistrationDisposition::Unavailable);
    EXPECT_EQ(result.errorCode, inventory_error::kRegistrationUnavailable);
}

// ---------------------------------------------------------------------------
// 处置矩阵（响应解码）
// ---------------------------------------------------------------------------
TEST(RegistrarResponseTest, DuplicateIsCompleted) {
    auto r = someip::DiagInstalledVersionRegistrar::decodeResponse(
        R"({"schemaVersion":1,"disposition":"DUPLICATE","registrationId":"REG-9"})");
    EXPECT_EQ(r.disposition, RegistrationDisposition::Duplicate);
    EXPECT_TRUE(r.completed());
    EXPECT_EQ(r.registrationId, "REG-9");
}

TEST(RegistrarResponseTest, RetryableFailedIsNotCompleted) {
    auto r = someip::DiagInstalledVersionRegistrar::decodeResponse(
        R"({"schemaVersion":1,"disposition":"RETRYABLE_FAILED"})");
    EXPECT_EQ(r.disposition, RegistrationDisposition::RetryableFailed);
    EXPECT_FALSE(r.completed());
    EXPECT_TRUE(r.retryable());
}

TEST(RegistrarResponseTest, RejectedInvalidCarriesReasonCodes) {
    auto r = someip::DiagInstalledVersionRegistrar::decodeResponse(
        R"({"schemaVersion":1,"disposition":"REJECTED_INVALID","rejectedItems":[
              {"ecuId":"VCU","reasonCode":"UNKNOWN_ECU"},
              {"ecuId":"BMS","reasonCode":"INVALID_VERSION"}]})");
    EXPECT_EQ(r.disposition, RegistrationDisposition::RejectedInvalid);
    EXPECT_TRUE(r.rejected());
    ASSERT_EQ(r.rejectedItems.size(), 2u);
    EXPECT_EQ(r.rejectedItems[0].ecuId, "VCU");
    EXPECT_EQ(r.rejectedItems[0].reasonCode, "UNKNOWN_ECU");
    EXPECT_EQ(r.rejectedItems[1].reasonCode, "INVALID_VERSION");
}

TEST(RegistrarResponseTest, MissingRegistrationIdIsRetryable) {
    auto r = someip::DiagInstalledVersionRegistrar::decodeResponse(
        R"({"schemaVersion":1,"disposition":"APPLIED","appliedCount":1})");
    EXPECT_EQ(r.disposition, RegistrationDisposition::RetryableFailed);
    EXPECT_EQ(r.errorCode, inventory_error::kRegistrationFailed);
}

TEST(RegistrarResponseTest, UnknownDispositionAndBadJsonAreRetryable) {
    auto unknown = someip::DiagInstalledVersionRegistrar::decodeResponse(
        R"({"schemaVersion":1,"disposition":"WEIRD_STATE"})");
    EXPECT_EQ(unknown.disposition, RegistrationDisposition::RetryableFailed);

    auto bad = someip::DiagInstalledVersionRegistrar::decodeResponse("not-json");
    EXPECT_EQ(bad.disposition, RegistrationDisposition::RetryableFailed);

    auto empty = someip::DiagInstalledVersionRegistrar::decodeResponse("");
    EXPECT_EQ(empty.disposition, RegistrationDisposition::RetryableFailed);

    auto newer = someip::DiagInstalledVersionRegistrar::decodeResponse(
        R"({"schemaVersion":2,"disposition":"APPLIED","registrationId":"R"})");
    EXPECT_EQ(newer.disposition, RegistrationDisposition::RetryableFailed);
}

// ---------------------------------------------------------------------------
// 传输失败（DIAG 返回错误）：可重试，不当作成功
// ---------------------------------------------------------------------------
TEST_F(RegistrarTest, TransportFailureIsRetryable) {
    endpoint_->failTransport = true;
    CallContext ctx;
    auto result = registrar_->registerInstalledVersions(makeReport(), ctx);
    EXPECT_EQ(result.disposition, RegistrationDisposition::RetryableFailed);
    EXPECT_EQ(result.errorCode, inventory_error::kRegistrationFailed);
    EXPECT_TRUE(result.retryable());
    EXPECT_EQ(endpoint_->callCount, 1);
}
