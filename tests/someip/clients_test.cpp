// =============================================================================
// tests/someip/clients_test.cpp
// CCU-FOTA-DSN-CR-007 §测试设计 - DIAG/TBOX Client 适配器测试
// 覆盖：availability 状态机、VersionMismatch 不调用、sync 调用成功/失败、
// timeout/cancel、late/duplicate response 不完成其他请求（framework 保证）。
// 使用真实 SomeIpRuntime（FakeSomeIpBackend）+ InlineExecutor。
// =============================================================================
#include "fota_someip_test_util.h"
#include "ccu/fota/someip/diag_inventory_client.hpp"
#include "ccu/fw/someip/runtime.hpp"
#include "ccu/fw/someip/types.hpp"
#include "constants.h"
#include "data_models.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <string>

using namespace ccu_fota;
using namespace ccu_fota_test;
namespace someip_fw = ccu::fw::someip;

namespace {

someip_fw::Payload vinPayload() {
    std::string vin = "12345678901234567";  // 17 chars
    return someip_fw::Payload(vin.begin(), vin.end());
}

// CCU-FOTA-DSN-CR-013：DIAG 清单接口 payload v1（含 ecuList 逐 ECU 明细）。
someip_fw::Payload inventoryPayloadV1() {
    const std::string json = R"({
      "schemaVersion": 1,
      "inventorySource": "seed",
      "vin": "12345678901234567",
      "vinSource": "PROVISIONED",
      "baselineId": "BASE-DIAG-1",
      "baselineSource": "FACTORY",
      "registryVersion": "2026.07.1",
      "collectedAt": "2026-11-08T10:00:00Z",
      "overallResult": "ALL_OK",
      "ecuList": [
        {"ecuId":"BMS","partNumber":"PN-BMS","swVersion":"1.0.0","hwVersion":"HW-BMS",
         "source":"UDS_0x22","status":"OK","errorCode":""},
        {"ecuId":"VCU","partNumber":"PN-VCU","swVersion":"2.0.0","hwVersion":"HW-VCU",
         "source":"SOMEIP_GET_VERSION","status":"OK"}
      ]
    })";
    return someip_fw::Payload(json.begin(), json.end());
}

} // namespace

// ============================================================
// DIAG Client
// ============================================================
class DiagClientTest : public ::testing::Test {
protected:
    std::shared_ptr<InlineExecutor> exec_;
    someip_fw::SomeIpRuntime rt_;
    someip_fw::Provider provider_;
    std::shared_ptr<someip::DiagInventoryClient> diag_;

    void SetUp() override {
        exec_ = std::make_shared<InlineExecutor>();
        rt_ = someip_fw::SomeIpRuntime::create(makeTestConfig(), exec_);
        rt_.start();
        someip_fw::ServiceKey key{DEFAULT_DIAG_SERVICE_ID, DEFAULT_DIAG_INSTANCE_ID};
        provider_ = rt_.createProvider(key, {1, 0});
        provider_.registerMethod(METHOD_READ_VIN,
            [](const someip_fw::RequestContext&, someip_fw::PayloadView) {
                return someip_fw::MethodResult::ok(vinPayload());
            });
        provider_.registerMethod(METHOD_COLLECT_VEHICLE_INVENTORY,
            [](const someip_fw::RequestContext&, someip_fw::PayloadView) {
                return someip_fw::MethodResult::ok(inventoryPayloadV1());
            });
        provider_.offer();

        auto client = std::make_shared<someip_fw::Client>(
            rt_.createClient(key, {1, 0}));
        diag_ = std::make_shared<someip::DiagInventoryClient>(
            std::move(client), std::chrono::milliseconds(2000));
        diag_->requestService();
        // 等待 availability 达到 Available（adapter 透传 framework Client）
        for (int i = 0; i < 1000 && diag_->availability() != someip_fw::Availability::Available; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(diag_->availability(), someip_fw::Availability::Available);
    }
    void TearDown() override {
        diag_->releaseService();
        provider_.stopOffer();
        rt_.stop();
    }
};

TEST_F(DiagClientTest, GetVinReturnsSeventeenChars) {
    std::string vin;
    ASSERT_TRUE(diag_->getVin(vin));
    EXPECT_EQ(vin.size(), 17u);
    EXPECT_EQ(vin, "12345678901234567");
}

TEST_F(DiagClientTest, CollectVehicleInventorySucceeds) {
    VehicleSoftwareSnapshot snap;
    ASSERT_TRUE(diag_->collectVehicleInventory(snap));
    // 清单内容完全来自 DIAG payload，FOTA 侧不得补全/硬编码任何常量。
    EXPECT_EQ(snap.schema_version, "1");
    EXPECT_EQ(snap.vin, "12345678901234567");
    EXPECT_EQ(snap.registry_version, "2026.07.1");
    EXPECT_EQ(snap.baseline_id.value_or(""), "BASE-DIAG-1");
    EXPECT_EQ(snap.inventory_source, InventorySource::SEED);
    EXPECT_EQ(snap.overall_result, CollectionStatus::ALL_OK);
    ASSERT_EQ(snap.ecu_list.size(), 2u);
    EXPECT_EQ(snap.ecu_list[0].ecu_id, "BMS");
    EXPECT_EQ(snap.ecu_list[0].source, VersionSource::UDS_0x22);
    EXPECT_EQ(snap.ecu_list[1].ecu_id, "VCU");
    EXPECT_EQ(snap.ecu_list[1].source, VersionSource::SOMEIP_GET_VERSION);
    // 采集阶段不分配序号（CR-012 D2）。
    EXPECT_EQ(snap.snapshot_seq, 0u);
}

// ============================================================
// VersionMismatch - Client major 版本与 Provider 不匹配时不调用
// ============================================================
TEST(ClientsVersionTest, VersionMismatchPreventsCall) {
    auto exec = std::make_shared<InlineExecutor>();
    someip_fw::SomeIpRuntime rt = someip_fw::SomeIpRuntime::create(makeTestConfig(), exec);
    rt.start();
    someip_fw::ServiceKey key{DEFAULT_DIAG_SERVICE_ID, DEFAULT_DIAG_INSTANCE_ID};
    someip_fw::Provider provider = rt.createProvider(key, {1, 0});
    provider.registerMethod(METHOD_READ_VIN,
        [](const someip_fw::RequestContext&, someip_fw::PayloadView) {
            return someip_fw::MethodResult::ok(vinPayload());
        });
    provider.offer();

    // Client 请求 major=2 -> VersionMismatch
    auto client = std::make_shared<someip_fw::Client>(rt.createClient(key, {2, 0}));
    auto diag = someip::DiagInventoryClient(std::move(client),
                                            std::chrono::milliseconds(500));
    diag.requestService();
    // 等待 availability 稳定（应进入 VersionMismatch，而非 Available）
    for (int i = 0; i < 500; ++i) {
        if (diag.availability() != someip_fw::Availability::Unknown) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // VersionMismatch 时不调用 -> getVin 失败
    std::string vin;
    EXPECT_FALSE(diag.getVin(vin));

    diag.releaseService();
    provider.stopOffer();
    rt.stop();
}
