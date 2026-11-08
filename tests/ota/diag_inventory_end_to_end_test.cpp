// =============================================================================
// tests/ota/diag_inventory_end_to_end_test.cpp
// DIAG 清单读取链路端到端测试 (CCU-FOTA-DSN-CR-013 验收标准 1/2/3/5
//                                 / CCU-FOTA-DSN-CR-014 US-041/044)
// -----------------------------------------------------------------------------
// 链路：Fake DIAG（真实 SomeIpRuntime + Provider，0x0002 payload v2）
//   -> DiagInventoryClient（真实 SOME/IP 调用 + DiagInventoryCodec 解码）
//   -> SnapshotAssembler -> InventoryProviderFromAssembler（稳定排序/digest/指纹）
//   -> FotaOrchestrator -> Fake 云侧（TaskCheck 上报）
// 断言：四类件号/版本逐字段进入云端且互不交叉；首轮 FULL 产生 1 个版本；
// 内容变化后恰好新增 1 个 revision + FULL；连续十轮不增长；
// 旧 DIAG payload v1（无软件件号）不产生任何上报或 revision。
// =============================================================================

#include "../someip/fota_someip_test_util.h"

#include "ccu/fota/ota/fota_cloud_proxy_via_transport.hpp"
#include "ccu/fota/ota/fota_orchestrator.hpp"
#include "ccu/fota/ota/mock/fake_vehicle_message_transport.hpp"
#include "ccu/fota/ota/mock/mock_ports.hpp"
#include "ccu/fota/ota/ports/production_ports.hpp"
#include "ccu/fota/someip/diag_inventory_client.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"
#include "ccu/fota/store/fota_state_store.hpp"
#include "constants.h"
#include "snapshot_assembler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace ccu_fota;
using namespace ccu_fota::ota;
using namespace ccu_fota::ota::mock;
using namespace ccu_fota::store;
using namespace ccu_fota::store::fota;
using namespace ccu_fota_test;
namespace someip_fw = ccu::fw::someip;

namespace {

fs::path makeUniqueRoot() {
    static int counter = 0;
    auto root = fs::temp_directory_path() /
                ("fota-diag-e2e-" + std::to_string(getpid()) + "-" +
                 std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

// DIAG 侧清单响应（可变更内容，模拟登记后版本变化 / seed->hardware）。
// CR-014：payloadVersion 可切回 v1 以验证混合版本 fail-closed 配对行为。
struct DiagInventoryResponse {
    int payloadVersion = 2;  // 2 = 目标契约（独立软件件号）；1 = 旧 DIAG
    std::string source = "seed";
    std::string baseline = "BASE-DIAG-1";
    std::string bmsVersion = "1.0.0";
    std::string vcuVersion = "1.0.0";
    // 软件件号（与总成件号分开变化，验证内容身份覆盖软件件号）。
    std::string bmsSoftwarePn = "SW-PN-BMS";
    std::string vcuSoftwarePn = "SW-PN-VCU";
    // 空清单 / 缺字段等 DIAG 侧异常（验证 FOTA fail-closed，不上报空清单）。
    bool emptyList = false;

    std::string json() const {
        if (emptyList) {
            return std::string(R"({
              "schemaVersion": )") + std::to_string(payloadVersion) + R"(,
              "inventorySource": ")" + source + R"(",
              "vin": "VINTEST0000000001",
              "registryVersion": "2026.07.1",
              "overallResult": "ALL_OK",
              "ecuList": []
            })";
        }
        // v2：逐 ECU 独立表达总成件号、软件件号、序列号与硬件版本；
        // v1：字段集冻结，不含 softwarePartNumber（旧 DIAG）。
        const std::string swPnBms =
            payloadVersion == 2 ? "\"softwarePartNumber\":\"" + bmsSoftwarePn + "\"," : "";
        const std::string swPnVcu =
            payloadVersion == 2 ? "\"softwarePartNumber\":\"" + vcuSoftwarePn + "\"," : "";
        const std::string snBms =
            payloadVersion == 2 ? R"("serialNumber":"SN-BMS",)" : "";
        const std::string snVcu =
            payloadVersion == 2 ? R"("serialNumber":"SN-VCU",)" : "";
        return std::string(R"({
          "schemaVersion": )") + std::to_string(payloadVersion) + R"(,
          "inventorySource": ")" + source + R"(",
          "vin": "VINTEST0000000001",
          "vinSource": "PROVISIONED",
          "baselineId": ")" + baseline + R"(",
          "baselineSource": "FACTORY",
          "registryVersion": "2026.07.1",
          "collectedAt": "2026-11-08T10:00:00Z",
          "overallResult": "ALL_OK",
          "ecuList": [
            {"ecuId":"BMS","partNumber":"PN-BMS",)" + swPnBms + snBms +
            R"("swVersion":")" + bmsVersion +
            R"(","hwVersion":"HW-BMS","source":"UDS_0x22","status":"OK"},
            {"ecuId":"VCU","partNumber":"PN-VCU",)" + swPnVcu + snVcu +
            R"("swVersion":")" + vcuVersion +
            R"(","hwVersion":"HW-VCU","source":"SOMEIP_GET_VERSION","status":"OK"}
          ]
        })";
    }
};

someip_fw::Payload payloadOf(const std::string& s) {
    return someip_fw::Payload(s.begin(), s.end());
}

} // namespace

class DiagInventoryEndToEndTest : public ::testing::Test {
protected:
    // DIAG SOME/IP 侧
    std::shared_ptr<InlineExecutor> exec_;
    someip_fw::SomeIpRuntime rt_;
    someip_fw::Provider provider_;
    std::shared_ptr<DiagInventoryResponse> diagResp_;
    std::shared_ptr<someip::DiagInventoryClient> diagClient_;

    // FOTA 业务侧
    fs::path root_;
    std::shared_ptr<FotaStateStore> fotaStore_;
    std::unique_ptr<FotaCloudStateStore> cloudStore_;
    std::unique_ptr<FakeVehicleMessageTransport> transport_;
    std::unique_ptr<FotaCloudProxyViaTransport> cloud_;
    std::shared_ptr<SnapshotAssembler> assembler_;
    std::unique_ptr<InventoryProviderFromAssembler> inv_;
    std::unique_ptr<MockConsentProvider> consent_;
    std::unique_ptr<MockPackageDownloader> dl_;
    std::unique_ptr<MockVehicleConditionProvider> cond_;
    std::unique_ptr<MockInstallExecutor> execPort_;
    std::unique_ptr<MockLogCollector> log_;
    std::unique_ptr<FakeInstalledVersionRegistrar> registrar_;
    std::unique_ptr<FotaOrchestrator> orch_;

    void SetUp() override {
        // ---- DIAG Provider：真实 SOME/IP 往返 ----
        exec_ = std::make_shared<InlineExecutor>();
        rt_ = someip_fw::SomeIpRuntime::create(makeTestConfig(), exec_);
        rt_.start();
        diagResp_ = std::make_shared<DiagInventoryResponse>();
        someip_fw::ServiceKey key{DEFAULT_DIAG_SERVICE_ID, DEFAULT_DIAG_INSTANCE_ID};
        provider_ = rt_.createProvider(key, {1, 0});
        auto resp = diagResp_;
        provider_.registerMethod(METHOD_READ_VIN,
            [](const someip_fw::RequestContext&, someip_fw::PayloadView) {
                return someip_fw::MethodResult::ok(payloadOf("VINTEST0000000001"));
            });
        provider_.registerMethod(METHOD_COLLECT_VEHICLE_INVENTORY,
            [resp](const someip_fw::RequestContext&, someip_fw::PayloadView) {
                return someip_fw::MethodResult::ok(payloadOf(resp->json()));
            });
        provider_.offer();

        auto fwClient = std::make_shared<someip_fw::Client>(rt_.createClient(key, {1, 0}));
        diagClient_ = std::make_shared<someip::DiagInventoryClient>(
            fwClient, std::chrono::milliseconds(2000));
        diagClient_->requestService();
        for (int i = 0; i < 1000 && diagClient_->availability() != someip_fw::Availability::Available; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(diagClient_->availability(), someip_fw::Availability::Available);

        // ---- FOTA 业务装配（生产清单端口 + Mock 车内端口 + Fake 云）----
        root_ = makeUniqueRoot();
        auto s = FotaStateStore::open(root_, 100, 3600000);
        fotaStore_ = std::make_shared<FotaStateStore>(std::move(s));
        cloudStore_ = std::make_unique<FotaCloudStateStore>(fotaStore_->underlyingStore());

        assembler_ = std::make_shared<SnapshotAssembler>(diagClient_);
        assembler_->setThrottleInterval(0);
        assembler_->setMaxEcuCount(DEFAULT_MAX_ECU_COUNT);
        inv_ = std::make_unique<InventoryProviderFromAssembler>(assembler_, "1.0.0");

        // 云端无任务场景：每轮只做 TaskCheck（清单上报），不进入安装。
        auto scenario = parseScenario(R"({
          "scenario": "diag-e2e",
          "clock": "virtual",
          "inventory": {"mode": "FULL", "baseline": "BASE-DIAG-1", "fota_master_version": "1.0.0"},
          "packages": [],
          "executor": {"stages": []},
          "faults": ["inventory_no_task"]
        })");
        transport_ = std::make_unique<FakeVehicleMessageTransport>(scenario);
        cloud_ = std::make_unique<FotaCloudProxyViaTransport>(*transport_);
        consent_ = std::make_unique<MockConsentProvider>();
        dl_ = std::make_unique<MockPackageDownloader>(scenario);
        cond_ = std::make_unique<MockVehicleConditionProvider>();
        execPort_ = std::make_unique<MockInstallExecutor>(scenario);
        log_ = std::make_unique<MockLogCollector>();
        registrar_ = std::make_unique<FakeInstalledVersionRegistrar>();

        FotaOrchestratorConfig cfg;
        cfg.inventoryIdentityRetryBackoff = std::chrono::milliseconds(0);
        orch_ = std::make_unique<FotaOrchestrator>(
            *cloud_, *inv_, *consent_, *dl_, *cond_, *execPort_, *log_, *cloudStore_,
            cfg, /*clockSync=*/nullptr,
            [this]() { return fotaStore_->allocateSnapshotSeq(); },
            registrar_.get());
    }

    void TearDown() override {
        diagClient_->releaseService();
        provider_.stopOffer();
        rt_.stop();
    }
};

// ---------------------------------------------------------------------------
// 首轮：清单内容与 DIAG 返回一致（无代码常量），1 个 FULL 版本
// ---------------------------------------------------------------------------
TEST_F(DiagInventoryEndToEndTest, FirstCycleReportsDiagContent) {
    auto o = orch_->step();
    EXPECT_TRUE(o.error.empty()) << o.error;

    EXPECT_EQ(transport_->inventoryVersionCount(), 1u);
    EXPECT_EQ(transport_->inventoryFullCount(), 1u);
    EXPECT_EQ(transport_->cloudInventoryRevision(), 1u);

    auto last = transport_->lastTaskCheck();
    ASSERT_EQ(last.ecu_list_size(), 2);
    // 稳定排序（ecuId）：BMS < VCU。
    EXPECT_EQ(last.ecu_list(0).ecu_id(), "BMS");
    // CR-014 D2：四类字段逐项一致，不得交叉赋值。
    EXPECT_EQ(last.ecu_list(0).hardware_part_number(), "PN-BMS");
    EXPECT_EQ(last.ecu_list(0).software_part_number(), "SW-PN-BMS");
    EXPECT_EQ(last.ecu_list(0).sw_version(), "1.0.0");
    EXPECT_EQ(last.ecu_list(0).hw_version(), "HW-BMS");
    EXPECT_NE(last.ecu_list(0).software_part_number(),
              last.ecu_list(0).hardware_part_number());
    EXPECT_NE(last.ecu_list(0).hardware_part_number(),
              last.ecu_list(0).hw_version());
    EXPECT_EQ(last.ecu_list(1).ecu_id(), "VCU");
    EXPECT_EQ(last.ecu_list(1).hardware_part_number(), "PN-VCU");
    EXPECT_EQ(last.ecu_list(1).software_part_number(), "SW-PN-VCU");
    EXPECT_EQ(last.ecu_list(1).sw_version(), "1.0.0");
    EXPECT_EQ(last.ecu_list(1).hw_version(), "HW-VCU");
    // serialNumber 不进入任何件号字段。
    EXPECT_NE(last.ecu_list(0).hardware_part_number(), "SN-BMS");
    EXPECT_NE(last.ecu_list(0).software_part_number(), "SN-BMS");
    EXPECT_FALSE(last.ecu_list_digest().value_hex().empty());

    // 清单只承载身份，不落 ECU 内容（fota.inventory 不含清单条目）。
    auto rec = cloudStore_->loadInventory();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->status, "ACCEPTED");
    EXPECT_EQ(rec->inventoryRevision, 1u);
    EXPECT_EQ(rec->baselineCode, "BASE-DIAG-1");
    // CR-014 D5：持久化双域版本。
    EXPECT_EQ(rec->fingerprintCanonicalization, "cgw-fota-version-v2");
    EXPECT_EQ(rec->wireCanonicalization, "inventory-ecu-list-v2");
}

// ---------------------------------------------------------------------------
// 内容不变：连续周期保持 1 个版本（CR-014：十轮不增长）
// ---------------------------------------------------------------------------
TEST_F(DiagInventoryEndToEndTest, RepeatedCyclesDoNotGrowVersions) {
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(orch_->step().error.empty());
    }
    EXPECT_EQ(transport_->inventoryVersionCount(), 1u);
    EXPECT_EQ(transport_->inventoryFullCount(), 1u);
    EXPECT_EQ(transport_->inventoryDigestCount(), 9u);
}

// ---------------------------------------------------------------------------
// DIAG 清单变化（登记后版本事实 / seed->hardware 切换）：恰好 +1 revision + FULL
// ---------------------------------------------------------------------------
TEST_F(DiagInventoryEndToEndTest, DiagContentChangeAddsExactlyOneRevision) {
    ASSERT_TRUE(orch_->step().error.empty());
    ASSERT_EQ(transport_->inventoryVersionCount(), 1u);

    // 模拟 DIAG 侧登记成功后清单事实变化。
    diagResp_->bmsVersion = "2.0.0";
    diagResp_->source = "hardware";
    ASSERT_TRUE(orch_->step().error.empty());

    EXPECT_EQ(transport_->inventoryFullCount(), 2u);
    EXPECT_EQ(transport_->inventoryVersionCount(), 2u);
    EXPECT_EQ(transport_->cloudInventoryRevision(), 2u);
    auto last = transport_->lastTaskCheck();
    ASSERT_EQ(last.ecu_list_size(), 2);
    EXPECT_EQ(last.ecu_list(0).ecu_id(), "BMS");
    EXPECT_EQ(last.ecu_list(0).sw_version(), "2.0.0");

    // 之后内容未变：DIGEST，不再新增版本。
    ASSERT_TRUE(orch_->step().error.empty());
    EXPECT_EQ(transport_->inventoryVersionCount(), 2u);
}

// ---------------------------------------------------------------------------
// 混合版本配对：旧 DIAG payload v1（无软件件号）+ 新 FOTA
// => 不产生任何上报、不分配 revision（不得静默上报错误字段）。
// ---------------------------------------------------------------------------
TEST_F(DiagInventoryEndToEndTest, LegacyDiagPayloadV1IsNeverReported) {
    diagResp_->payloadVersion = 1;
    auto o = orch_->step();
    EXPECT_FALSE(o.error.empty());
    EXPECT_EQ(transport_->inventoryVersionCount(), 0u);
    EXPECT_EQ(transport_->inventoryFullCount(), 0u);
    EXPECT_FALSE(cloudStore_->loadInventory().has_value());
    auto seq = fotaStore_->loadSequence();
    EXPECT_FALSE(seq.has_value() && seq->highestAllocated != 0u);
}

// ---------------------------------------------------------------------------
// CR-014 US-043：修正后首轮仅 1 个 FULL 版本；只改软件件号（总成件号不变）
// 恰好新增一个版本，之后收敛。
// ---------------------------------------------------------------------------
TEST_F(DiagInventoryEndToEndTest, SoftwarePartNumberChangeAddsExactlyOneRevision) {
    ASSERT_TRUE(orch_->step().error.empty());
    ASSERT_EQ(transport_->inventoryVersionCount(), 1u);

    // DIAG 侧只变软件件号：总成件号、软件版本与硬件版本均不变。
    diagResp_->bmsSoftwarePn = "SW-PN-BMS-2";
    ASSERT_TRUE(orch_->step().error.empty());

    EXPECT_EQ(transport_->inventoryFullCount(), 2u);
    EXPECT_EQ(transport_->inventoryVersionCount(), 2u);
    EXPECT_EQ(transport_->cloudInventoryRevision(), 2u);
    EXPECT_EQ(transport_->lastTaskCheck().ecu_list(0).hardware_part_number(),
              "PN-BMS");
    EXPECT_EQ(transport_->lastTaskCheck().ecu_list(0).software_part_number(),
              "SW-PN-BMS-2");

    // 之后内容不变：DIGEST，不再增长。
    ASSERT_TRUE(orch_->step().error.empty());
    EXPECT_EQ(transport_->inventoryVersionCount(), 2u);
    EXPECT_EQ(transport_->inventoryFullCount(), 2u);
}

// ---------------------------------------------------------------------------
// DIAG 返回空清单 / 非法 payload：不上报、不分配 revision（fail-closed）
// ---------------------------------------------------------------------------
TEST_F(DiagInventoryEndToEndTest, EmptyDiagListIsNeverReported) {
    auto o = orch_->step();
    EXPECT_TRUE(o.error.empty()) << o.error;
    ASSERT_EQ(transport_->inventoryVersionCount(), 1u);

    // DIAG 返回“成功 + 空清单”：必须视为采集失败（不得上报空清单/新版本）。
    diagResp_->emptyList = true;
    auto failed = orch_->step();
    EXPECT_FALSE(failed.error.empty());
    EXPECT_EQ(transport_->inventoryVersionCount(), 1u);
    EXPECT_EQ(transport_->inventoryFullCount(), 1u);
    auto rec = cloudStore_->loadInventory();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->inventoryRevision, 1u);
}

// ---------------------------------------------------------------------------
// DIAG payload 非法（未知枚举）：同样 fail-closed，不上报、不分配 revision
// ---------------------------------------------------------------------------
TEST_F(DiagInventoryEndToEndTest, InvalidDiagPayloadIsNeverReported) {
    auto o = orch_->step();
    EXPECT_TRUE(o.error.empty()) << o.error;
    ASSERT_EQ(transport_->inventoryVersionCount(), 1u);

    diagResp_->source = "unexpected-source";
    auto failed = orch_->step();
    EXPECT_FALSE(failed.error.empty());
    EXPECT_EQ(transport_->inventoryVersionCount(), 1u);
    auto rec = cloudStore_->loadInventory();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->inventoryRevision, 1u);
}
