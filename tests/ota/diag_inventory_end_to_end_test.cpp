// =============================================================================
// tests/ota/diag_inventory_end_to_end_test.cpp
// DIAG 清单读取链路端到端测试 (CCU-FOTA-DSN-CR-013 验收标准 1/2/3/5)
// -----------------------------------------------------------------------------
// 链路：Fake DIAG（真实 SomeIpRuntime + Provider，0x0002 payload v1）
//   -> DiagInventoryClient（真实 SOME/IP 调用 + DiagInventoryCodec 解码）
//   -> SnapshotAssembler -> InventoryProviderFromAssembler（稳定排序/digest/指纹）
//   -> FotaOrchestrator -> Fake 云侧（TaskCheck 上报）
// 断言：清单内容完全来自 DIAG（无任何代码常量）；首轮 FULL 产生 1 个版本；
// 内容变化（模拟登记/seed→hardware）后恰好新增 1 个 revision + FULL；
// 连续周期内容不变仍为同 1 个版本。
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
struct DiagInventoryResponse {
    std::string source = "seed";
    std::string baseline = "BASE-DIAG-1";
    std::string bmsVersion = "1.0.0";
    std::string vcuVersion = "1.0.0";
    // 空清单 / 缺字段等 DIAG 侧异常（验证 FOTA fail-closed，不上报空清单）。
    bool emptyList = false;

    std::string json() const {
        if (emptyList) {
            return std::string(R"({
              "schemaVersion": 1,
              "inventorySource": ")") + source + R"(",
              "vin": "VINTEST0000000001",
              "registryVersion": "2026.07.1",
              "overallResult": "ALL_OK",
              "ecuList": []
            })";
        }
        return std::string(R"({
          "schemaVersion": 1,
          "inventorySource": ")") + source + R"(",
          "vin": "VINTEST0000000001",
          "vinSource": "PROVISIONED",
          "baselineId": ")" + baseline + R"(",
          "baselineSource": "FACTORY",
          "registryVersion": "2026.07.1",
          "collectedAt": "2026-11-08T10:00:00Z",
          "overallResult": "ALL_OK",
          "ecuList": [
            {"ecuId":"BMS","partNumber":"PN-BMS","swVersion":")" + bmsVersion +
            R"(","hwVersion":"HW-BMS","source":"UDS_0x22","status":"OK"},
            {"ecuId":"VCU","partNumber":"PN-VCU","swVersion":")" + vcuVersion +
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
    EXPECT_EQ(last.ecu_list(0).sw_version(), "1.0.0");
    EXPECT_EQ(last.ecu_list(0).software_part_number(), "PN-BMS");
    EXPECT_EQ(last.ecu_list(1).ecu_id(), "VCU");
    EXPECT_EQ(last.ecu_list(1).sw_version(), "1.0.0");
    EXPECT_FALSE(last.ecu_list_digest().value_hex().empty());

    // 清单只承载身份，不落 ECU 内容（fota.inventory 不含清单条目）。
    auto rec = cloudStore_->loadInventory();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->status, "ACCEPTED");
    EXPECT_EQ(rec->inventoryRevision, 1u);
    EXPECT_EQ(rec->baselineCode, "BASE-DIAG-1");
}

// ---------------------------------------------------------------------------
// 内容不变：连续周期保持 1 个版本（DIGEST）
// ---------------------------------------------------------------------------
TEST_F(DiagInventoryEndToEndTest, RepeatedCyclesDoNotGrowVersions) {
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(orch_->step().error.empty());
    }
    EXPECT_EQ(transport_->inventoryVersionCount(), 1u);
    EXPECT_EQ(transport_->inventoryFullCount(), 1u);
    EXPECT_EQ(transport_->inventoryDigestCount(), 4u);
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
