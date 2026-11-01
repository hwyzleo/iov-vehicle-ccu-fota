// =============================================================================
// tests/someip/tbox_generic_transport_contract_test.cpp
// CCU-FOTA 契约 B TBOX 通用消息服务寻址解析测试 (CR-010/011 fail-closed)
// =============================================================================
// 覆盖：未分配（blocked）、完整合法分配、部分声明、越界、与契约 A 专用 Method
// 冲突、接口版本不一致。任何冲突/版本不一致必须抛 FotaConfigException（fail-
// closed），绝不回退 Fake 或旧私有协议。
// =============================================================================

#include "ccu/fota/someip/tbox_generic_transport_contract.hpp"
#include "ccu/fota/config/fota_config.hpp"
#include "constants.h"

#include <gtest/gtest.h>

using namespace ccu_fota;
using namespace ccu_fota::someip;

namespace {

FotaConfig makeConfig(std::uint16_t method, std::uint16_t event,
                      std::uint16_t group, std::uint32_t ifaceMajor = 1) {
    FotaConfig c;
    c.genericTransport.methodId = method;
    c.genericTransport.eventId = event;
    c.genericTransport.eventgroupId = group;
    c.genericTransport.interfaceMajor = ifaceMajor;
    return c;
}

} // namespace

// 未分配（全 0）-> resolve 返回 false（契约 B blocked），不抛异常。
TEST(TboxGenericTransportContract, NotAllocatedReturnsFalse) {
    FotaConfig c;
    TboxGenericTransportAddress addr;
    EXPECT_FALSE(resolveTboxGenericTransport(c, addr));
    EXPECT_FALSE(addr.fullyAllocated());
}

// 完整合法分配 -> resolve 返回 true，寻址正确。
TEST(TboxGenericTransportContract, ValidAllocationResolves) {
    auto c = makeConfig(0x0002, 0x8001, 0x0001);
    TboxGenericTransportAddress addr;
    EXPECT_TRUE(resolveTboxGenericTransport(c, addr));
    EXPECT_TRUE(addr.fullyAllocated());
    EXPECT_EQ(addr.service, DEFAULT_TBOX_SERVICE_ID);
    EXPECT_EQ(addr.instance, DEFAULT_TBOX_INSTANCE_ID);
    EXPECT_EQ(addr.method, 0x0002);
    EXPECT_EQ(addr.event, 0x8001);
    EXPECT_EQ(addr.eventgroup, 0x0001);
    EXPECT_EQ(addr.interfaceVersion.major, 1);
}

// 部分声明 -> fail-closed。
TEST(TboxGenericTransportContract, PartialAllocationFailsClosed) {
    auto c = makeConfig(0x0002, 0, 0);
    TboxGenericTransportAddress addr;
    EXPECT_THROW(resolveTboxGenericTransport(c, addr), FotaConfigException);
}

// method 越界 -> fail-closed。
TEST(TboxGenericTransportContract, MethodOutOfRangeFailsClosed) {
    auto c = makeConfig(0x8000, 0x8001, 0x0001);  // method 超 0x7FFF
    TboxGenericTransportAddress addr;
    EXPECT_THROW(resolveTboxGenericTransport(c, addr), FotaConfigException);
}

// event 越界（< 0x8000）-> fail-closed。
TEST(TboxGenericTransportContract, EventOutOfRangeFailsClosed) {
    auto c = makeConfig(0x0002, 0x0003, 0x0001);
    TboxGenericTransportAddress addr;
    EXPECT_THROW(resolveTboxGenericTransport(c, addr), FotaConfigException);
}

// eventgroup 保留值 0xFFFF -> fail-closed。
TEST(TboxGenericTransportContract, EventgroupReservedFailsClosed) {
    auto c = makeConfig(0x0002, 0x8001, 0xFFFF);
    TboxGenericTransportAddress addr;
    EXPECT_THROW(resolveTboxGenericTransport(c, addr), FotaConfigException);
}

// 与契约 A 专用 Method（TBOX_METHOD_REPORT_SOFTWARE_INVENTORY）冲突 -> fail-closed。
TEST(TboxGenericTransportContract, ConflictWithContractARejected) {
    auto c = makeConfig(TBOX_METHOD_REPORT_SOFTWARE_INVENTORY, 0x8001, 0x0001);
    TboxGenericTransportAddress addr;
    EXPECT_THROW(resolveTboxGenericTransport(c, addr), FotaConfigException);
}

// 接口版本不一致 -> fail-closed。
TEST(TboxGenericTransportContract, InterfaceVersionMismatchRejected) {
    auto c = makeConfig(0x0002, 0x8001, 0x0001, /*ifaceMajor=*/2);
    TboxGenericTransportAddress addr;
    EXPECT_THROW(resolveTboxGenericTransport(c, addr), FotaConfigException);
}
