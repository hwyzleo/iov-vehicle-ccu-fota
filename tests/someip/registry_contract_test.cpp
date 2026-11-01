// =============================================================================
// tests/someip/registry_contract_test.cpp
// CCU-FOTA-DSN-CR-007 §测试设计 - Registry/IDL 契约一致性测试
// 覆盖 ServiceKey/InterfaceVersion 校验、保留 ID 冲突 fail-closed (CCU-FW-0302)、
// 资源上限 (CCU-FW-0309)、寻址 SSOT 不在源码重新分配。
// =============================================================================
#include "fota_someip_test_util.h"
#include "ccu/fw/someip/runtime.hpp"
#include "ccu/fw/someip/types.hpp"
#include "constants.h"
#include <gtest/gtest.h>

using namespace ccu_fota_test;
using namespace ccu_fota;
namespace someip_fw = ccu::fw::someip;

// 断言语句抛出 SomeIpException 且 code 匹配。
#define EXPECT_SOMEIP_CODE(stmt, expected_code)                               \
    do {                                                                       \
        bool caught = false;                                                   \
        try { stmt; }                                                          \
        catch (const someip_fw::SomeIpException& e) {                          \
            caught = (e.code == (expected_code));                              \
            if (!caught)                                                       \
                ADD_FAILURE() << "expected " << (expected_code) << " got "     \
                              << e.code << ": " << e.what();                   \
        } catch (...) { caught = false; }                                      \
        EXPECT_TRUE(caught);                                                   \
    } while (0)

TEST(RegistryContractTest, FotaProviderServiceKeyMatchesRegistry) {
    // 过渡 SSOT (constants.h) 与 Registry 分配一致：0x1120/0x0001
    someip_fw::ServiceKey fotaKey{FOTA_PROVIDER_SERVICE_ID, FOTA_PROVIDER_INSTANCE_ID};
    EXPECT_EQ(fotaKey.service, 0x1120);
    EXPECT_EQ(fotaKey.instance, 0x0001);
}

TEST(RegistryContractTest, DiagClientServiceKeyMatchesRegistry) {
    someip_fw::ServiceKey diagKey{DEFAULT_DIAG_SERVICE_ID, DEFAULT_DIAG_INSTANCE_ID};
    EXPECT_EQ(diagKey.service, 0x1110);
    EXPECT_EQ(diagKey.instance, 0x0001);
}

TEST(RegistryContractTest, TboxClientServiceKeyMatchesRegistry) {
    someip_fw::ServiceKey tboxKey{DEFAULT_TBOX_SERVICE_ID, DEFAULT_TBOX_INSTANCE_ID};
    EXPECT_EQ(tboxKey.service, 0x6101);
    EXPECT_EQ(tboxKey.instance, 0x0001);
}

TEST(RegistryContractTest, ReservedServiceIdRejected) {
    auto exec = std::make_shared<InlineExecutor>();
    someip_fw::SomeIpRuntime rt = someip_fw::SomeIpRuntime::create(makeTestConfig(), exec);
    rt.start();
    // 0x0000 保留 -> CCU-FW-0302 fail-closed
    EXPECT_SOMEIP_CODE(
        rt.createProvider({0x0000, 0x0001}, {1, 0}), "CCU-FW-0302");
    EXPECT_SOMEIP_CODE(
        rt.createProvider({0xFFFF, 0x0001}, {1, 0}), "CCU-FW-0302");
    rt.stop();
}

TEST(RegistryContractTest, ReservedInstanceIdRejected) {
    auto exec = std::make_shared<InlineExecutor>();
    someip_fw::SomeIpRuntime rt = someip_fw::SomeIpRuntime::create(makeTestConfig(), exec);
    rt.start();
    EXPECT_SOMEIP_CODE(
        rt.createClient({0x1110, 0xFFFF}, {1, 0}), "CCU-FW-0302");
    rt.stop();
}

TEST(RegistryContractTest, DuplicateApplicationIdentityRejected) {
    // 同一 application identity 不得并行启动第二 Runtime (CR-007 §总体架构)
    auto exec = std::make_shared<InlineExecutor>();
    someip_fw::SomeIpRuntime rt1 = someip_fw::SomeIpRuntime::create(
        makeTestConfig("ccu-fota-dup"), exec);
    rt1.start();
    EXPECT_SOMEIP_CODE(
        someip_fw::SomeIpRuntime::create(makeTestConfig("ccu-fota-dup"), exec),
        "CCU-FW-0302");
    rt1.stop();
}

TEST(RegistryContractTest, ResourceLimitEnforced) {
    // maxProviders=1 -> 第二个 Provider 触发 CCU-FW-0309
    auto cfg = makeTestConfig("ccu-fota-reslimit");
    cfg.maxProviders = 1;
    auto exec = std::make_shared<InlineExecutor>();
    someip_fw::SomeIpRuntime rt = someip_fw::SomeIpRuntime::create(cfg, exec);
    rt.start();
    auto p1 = rt.createProvider({0x1120, 0x0001}, {1, 0});  // 保留以防析构减计数
    EXPECT_SOMEIP_CODE(
        rt.createProvider({0x1121, 0x0001}, {1, 0}), "CCU-FW-0309");
    rt.stop();
}

TEST(RegistryContractTest, StartStopIdempotent) {
    auto exec = std::make_shared<InlineExecutor>();
    someip_fw::SomeIpRuntime rt = someip_fw::SomeIpRuntime::create(makeTestConfig(), exec);
    rt.start();
    rt.stop();
    rt.stop();  // 幂等
    EXPECT_NE(rt.state(), someip_fw::RuntimeState::Running);
}
