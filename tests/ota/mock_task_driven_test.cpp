// =============================================================================
// tests/ota/mock_task_driven_test.cpp
// 任务驱动 Mock 端口测试 (CCU-FOTA-DSN-CR-015 §D3/D4 / 测试矩阵 3–5)
// =============================================================================
// 覆盖：冻结任务事实（两 ECU/两包/不同版本）逐项映射、断点偏移、stageResultId
// 幂等、失败/回滚结果、空任务包 fail-closed、场景校验。
// =============================================================================

#include "ccu/fota/ota/mock/mock_ports.hpp"
#include "ccu/fota/ota/mock/scenario_script.hpp"

#include "vehicle/fota/v1/execution.pb.h"
#include "vehicle/fota/v1/package.pb.h"
#include "vehicle/fota/v1/task.pb.h"
#include "vehicle/fota/v1/types.pb.h"

#include <gtest/gtest.h>

#include <set>

namespace fota_v1 = ::vehicle::fota::v1;
using fota_v1::PackageSummary;
using fota_v1::VehicleTaskSnapshot;
using namespace ccu_fota::ota;
using namespace ccu_fota::ota::mock;

namespace {

const char* kStagesScenario = R"({
  "scenario": "two-package",
  "clock": "realtime",
  "packages": [
    {"package_id": "PKG-VCU", "bytes": 4096, "ecu_id": "VCU",
     "plan_node_id": "NODE-VCU", "package_revision": "rev-1",
     "source_version": "1.0.0", "target_version": "2.0.0"},
    {"package_id": "PKG-BMS", "bytes": 8192, "ecu_id": "BMS",
     "plan_node_id": "NODE-BMS", "package_revision": "rev-9",
     "source_version": "3.0.0", "target_version": "3.1.0"}
  ],
  "executor": {"stages": [
    {"stage": "INSTALL", "progress": [50, 100], "result": "SUCCEEDED"},
    {"stage": "REBOOT", "result": "SUCCEEDED"},
    {"stage": "POST_CHECK", "result": "SUCCEEDED"}
  ]},
  "faults": []
})";

PackageSummary makePackage(const std::string& ecu, const std::string& id,
                           const std::string& node, const std::string& rev,
                           const std::string& source, const std::string& target,
                           std::uint64_t size) {
    PackageSummary p;
    p.set_package_id(id);
    p.set_package_revision(rev);
    p.set_ecu_id(ecu);
    p.set_plan_node_id(node);
    p.set_source_version(source);
    p.set_target_version(target);
    p.set_size_bytes(size);
    p.mutable_package_digest()->set_algorithm("sha-256");
    p.mutable_package_digest()->set_value_hex(std::string(64, 'c'));
    p.set_differential_type("full");
    return p;
}

VehicleTaskSnapshot makeTwoPackageTask() {
    VehicleTaskSnapshot t;
    t.set_vehicle_task_id("VT-CR015");
    t.set_task_revision(7);
    t.set_target_baseline_code("BASE-TARGET");
    *t.add_package_list() = makePackage("VCU", "PKG-VCU", "NODE-VCU", "rev-1",
                                        "1.0.0", "2.0.0", 4096);
    *t.add_package_list() = makePackage("BMS", "PKG-BMS", "NODE-BMS", "rev-9",
                                        "3.0.0", "3.1.0", 8192);
    return t;
}

fota_v1::DownloadGrantResponse makeGrant(const std::string& id,
                                         const std::string& rev) {
    fota_v1::DownloadGrantResponse g;
    g.set_package_revision(rev);
    g.set_etag("etag-" + id);
    g.set_size_bytes(1 << 20);
    return g;
}

fota_v1::InstallPermitResponse makePermit() {
    fota_v1::InstallPermitResponse p;
    p.set_execution_id("EX-CR015");
    p.set_attempt_no(2);
    p.set_permit_id("PMT-1");
    return p;
}

class CollectingSink : public EventSink {
public:
    bool emit(const fota_v1::ExecutionEvent& evt) override {
        events.push_back(evt);
        return true;
    }
    std::vector<fota_v1::ExecutionEvent> events;
};

ScenarioScript scenarioWith(const std::string& extraFaults) {
    std::string json = kStagesScenario;
    if (!extraFaults.empty()) {
        const auto pos = json.find("\"faults\": []");
        json.replace(pos, std::string("\"faults\": []").size(),
                     "\"faults\": [\"" + extraFaults + "\"]");
    }
    return parseScenario(json);
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 下载结果逐项来自冻结任务与授权（不依赖固定测试常量）
// ---------------------------------------------------------------------------
TEST(MockTaskDriven, DownloadUsesTaskPackageFacts) {
    MockPackageDownloader dl(scenarioWith(""));
    auto pkg = makePackage("BMS", "PKG-BMS", "NODE-BMS", "rev-9", "3.0.0",
                           "3.1.0", 8192);
    auto grant = makeGrant("PKG-BMS", "rev-9");
    auto out = dl.downloadAndVerify(pkg, grant, 1024);
    ASSERT_TRUE(out.allStagesSucceeded);
    ASSERT_TRUE(out.hasStageResult);
    EXPECT_EQ(out.finalOffsetBytes, 8192u);
    EXPECT_EQ(out.bytesDownloaded, 8192u - 1024u);
    const auto& s = out.stageResult;
    EXPECT_EQ(s.package_id(), "PKG-BMS");
    EXPECT_EQ(s.verified_package_revision(), "rev-9");
    EXPECT_EQ(s.downloaded_bytes(), 8192u - 1024u);
    EXPECT_EQ(s.result(), fota_v1::RESULT_SUCCEEDED);
    EXPECT_EQ(s.actual_package_digest().value_hex(), pkg.package_digest().value_hex());
    EXPECT_TRUE(s.hash_verified());
    EXPECT_TRUE(s.signature_verified());
    EXPECT_EQ(s.stage_result_digest().value_hex().size(), 64u);
}

TEST(MockTaskDriven, StageResultIdStableForSameResultAndDistinctForFailure) {
    MockPackageDownloader dl(scenarioWith(""));
    auto pkg = makePackage("VCU", "PKG-VCU", "NODE-VCU", "rev-1", "1.0.0",
                           "2.0.0", 4096);
    auto grant = makeGrant("PKG-VCU", "rev-1");
    auto a = dl.downloadAndVerify(pkg, grant, 0);
    auto b = dl.downloadAndVerify(pkg, grant, 0);
    ASSERT_TRUE(a.hasStageResult && b.hasStageResult);
    EXPECT_EQ(a.stageResult.stage_result_id(), b.stageResult.stage_result_id());
    EXPECT_EQ(a.stageResult.stage_result_digest().value_hex(),
              b.stageResult.stage_result_digest().value_hex());

    MockPackageDownloader failing(scenarioWith("hash_failed"));
    auto f = failing.downloadAndVerify(pkg, grant, 0);
    ASSERT_TRUE(f.hasStageResult);
    EXPECT_FALSE(f.allStagesSucceeded);
    EXPECT_NE(f.stageResult.stage_result_id(),
              a.stageResult.stage_result_id());
    EXPECT_EQ(f.finalOffsetBytes, 0u);  // 失败不宣称完整下载
}

TEST(MockTaskDriven, DownloadRejectsOffsetBeyondSize) {
    MockPackageDownloader dl(scenarioWith(""));
    auto pkg = makePackage("VCU", "PKG-VCU", "NODE-VCU", "rev-1", "1.0.0",
                           "2.0.0", 4096);
    auto grant = makeGrant("PKG-VCU", "rev-1");
    auto out = dl.downloadAndVerify(pkg, grant, 4097);
    EXPECT_FALSE(out.allStagesSucceeded);
    EXPECT_EQ(out.finalOffsetBytes, 4097u);
    ASSERT_TRUE(out.hasStageResult);
    EXPECT_EQ(out.stageResult.result(), fota_v1::RESULT_FAILED);
    EXPECT_EQ(out.errorDetail, "offset_exceeds_size");
}

TEST(MockTaskDriven, DownloadFailuresBeforeSuccessUsesOffset) {
    const std::string json = R"({
      "scenario": "retry-offset",
      "packages": [{"package_id": "PKG-A", "bytes": 1000,
                    "failures_before_success": 1}],
      "executor": {"stages": [{"stage": "INSTALL", "result": "SUCCEEDED"}]},
      "faults": []
    })";
    MockPackageDownloader dl(parseScenario(json));
    auto pkg = makePackage("VCU", "PKG-A", "NODE-A", "r1", "1", "2", 1000);
    auto grant = makeGrant("PKG-A", "r1");

    auto first = dl.downloadAndVerify(pkg, grant, 200);
    EXPECT_FALSE(first.allStagesSucceeded);
    EXPECT_EQ(first.finalOffsetBytes, 200u);  // 保留合法偏移

    auto second = dl.downloadAndVerify(pkg, grant, 200);
    EXPECT_TRUE(second.allStagesSucceeded);
    EXPECT_EQ(second.finalOffsetBytes, 1000u);
    EXPECT_EQ(second.bytesDownloaded, 800u);
}

// ---------------------------------------------------------------------------
// 2. 安装事件/最终结果逐项映射冻结任务
// ---------------------------------------------------------------------------
TEST(MockTaskDriven, InstallEventsMapEachPackageFromTask) {
    MockInstallExecutor exec(scenarioWith(""));
    auto task = makeTwoPackageTask();
    auto prep = exec.prepare(task);
    ASSERT_TRUE(prep.ready);

    CollectingSink sink;
    auto st = exec.start(makePermit(), sink);
    ASSERT_TRUE(st.started);

    // 两包 × 3 阶段，每包有 50/100 进度事件（INSTALL 2 条 + 每阶段 1 条）。
    std::set<std::string> eventIds;
    bool sawVcu = false;
    bool sawBms = false;
    std::int64_t lastTs = 0;
    for (const auto& e : sink.events) {
        EXPECT_FALSE(e.event_id().empty());
        eventIds.insert(e.event_id());
        EXPECT_GE(e.occurred_at_ms(), lastTs);  // 单调不减
        lastTs = e.occurred_at_ms();
        if (e.package_id() == "PKG-VCU") {
            sawVcu = true;
            EXPECT_EQ(e.ecu_id(), "VCU");
            EXPECT_EQ(e.plan_node_id(), "NODE-VCU");
            EXPECT_EQ(e.current_version(), "1.0.0");
            EXPECT_EQ(e.target_version(), "2.0.0");
        }
        if (e.package_id() == "PKG-BMS") {
            sawBms = true;
            EXPECT_EQ(e.ecu_id(), "BMS");
            EXPECT_EQ(e.plan_node_id(), "NODE-BMS");
            EXPECT_EQ(e.current_version(), "3.0.0");
            EXPECT_EQ(e.target_version(), "3.1.0");
        }
    }
    EXPECT_TRUE(sawVcu);
    EXPECT_TRUE(sawBms);
    EXPECT_EQ(eventIds.size(), sink.events.size());  // eventId 唯一

    auto finalData = exec.readFinalResult();
    ASSERT_EQ(finalData.ecuResults.size(), 2u);
    EXPECT_EQ(finalData.actualBaselineCode, "BASE-TARGET");
    EXPECT_EQ(finalData.baselineStatus, "verified");
    EXPECT_FALSE(finalData.rollbackOccurred);

    bool finalVcu = false;
    bool finalBms = false;
    for (const auto& er : finalData.ecuResults) {
        EXPECT_EQ(er.result(), fota_v1::RESULT_SUCCEEDED);
        if (er.ecu_id() == "VCU") {
            finalVcu = true;
            EXPECT_EQ(er.package_id(), "PKG-VCU");
            EXPECT_EQ(er.source_version(), "1.0.0");
            EXPECT_EQ(er.target_version(), "2.0.0");
            EXPECT_EQ(er.actual_version(), "2.0.0");
            EXPECT_EQ(er.plan_node_id(), "NODE-VCU");
        }
        if (er.ecu_id() == "BMS") {
            finalBms = true;
            EXPECT_EQ(er.package_id(), "PKG-BMS");
            EXPECT_EQ(er.actual_version(), "3.1.0");
        }
    }
    EXPECT_TRUE(finalVcu);
    EXPECT_TRUE(finalBms);
}

TEST(MockTaskDriven, EventIdsStableAcrossReplay) {
    auto task = makeTwoPackageTask();
    CollectingSink sinkA;
    CollectingSink sinkB;
    MockInstallExecutor a(scenarioWith(""));
    MockInstallExecutor b(scenarioWith(""));
    ASSERT_TRUE(a.prepare(task).ready);
    ASSERT_TRUE(b.prepare(task).ready);
    ASSERT_TRUE(a.start(makePermit(), sinkA).started);
    ASSERT_TRUE(b.start(makePermit(), sinkB).started);
    ASSERT_EQ(sinkA.events.size(), sinkB.events.size());
    for (std::size_t i = 0; i < sinkA.events.size(); ++i) {
        EXPECT_EQ(sinkA.events[i].event_id(), sinkB.events[i].event_id());
        EXPECT_EQ(sinkA.events[i].package_id(), sinkB.events[i].package_id());
        EXPECT_EQ(sinkA.events[i].stage(), sinkB.events[i].stage());
    }
}

TEST(MockTaskDriven, InstallFailedProducesFailedResults) {
    MockInstallExecutor exec(scenarioWith("install_failed"));
    auto task = makeTwoPackageTask();
    ASSERT_TRUE(exec.prepare(task).ready);
    CollectingSink sink;
    ASSERT_TRUE(exec.start(makePermit(), sink).started);

    auto finalData = exec.readFinalResult();
    EXPECT_FALSE(finalData.rollbackOccurred);
    EXPECT_TRUE(finalData.actualBaselineCode.empty());
    EXPECT_EQ(finalData.baselineStatus, "not_applied");
    bool anyFailed = false;
    for (const auto& er : finalData.ecuResults) {
        if (er.result() != fota_v1::RESULT_SUCCEEDED) {
            anyFailed = true;
            EXPECT_NE(er.actual_version(), er.target_version());
            EXPECT_TRUE(er.has_failure_stage());
        }
    }
    EXPECT_TRUE(anyFailed);
}

TEST(MockTaskDriven, RollbackRequiredProducesRollbackFacts) {
    MockInstallExecutor exec(scenarioWith("rollback_required"));
    auto task = makeTwoPackageTask();
    ASSERT_TRUE(exec.prepare(task).ready);
    CollectingSink sink;
    ASSERT_TRUE(exec.start(makePermit(), sink).started);

    bool sawRollbackEvent = false;
    for (const auto& e : sink.events) {
        if (e.stage() == "ROLLBACK") sawRollbackEvent = true;
    }
    EXPECT_TRUE(sawRollbackEvent);

    auto finalData = exec.readFinalResult();
    EXPECT_TRUE(finalData.rollbackOccurred);
    EXPECT_TRUE(finalData.actualBaselineCode.empty());
    for (const auto& er : finalData.ecuResults) {
        EXPECT_TRUE(er.has_rollback_result());
    }
}

TEST(MockTaskDriven, EmptyTaskPackageListIsNotReady) {
    MockInstallExecutor exec(scenarioWith(""));
    fota_v1::VehicleTaskSnapshot empty;
    empty.set_vehicle_task_id("VT-EMPTY");
    auto prep = exec.prepare(empty);
    EXPECT_FALSE(prep.ready);
    EXPECT_EQ(prep.reason, "empty task package list");
}

// ---------------------------------------------------------------------------
// 3. 条件摘要与场景校验
// ---------------------------------------------------------------------------
TEST(MockTaskDriven, ConditionReadinessDigestIsDeterministic) {
    MockVehicleConditionProvider cond;
    auto a = cond.evaluateConditions();
    auto b = cond.evaluateConditions();
    EXPECT_EQ(a.localReadinessDigest, b.localReadinessDigest);
    EXPECT_EQ(a.localReadinessDigest.size(), 64u);
    EXPECT_TRUE(a.allGuardsPassed);

    cond.setGuardFailed(true);
    auto c = cond.evaluateConditions();
    EXPECT_FALSE(c.allGuardsPassed);
    EXPECT_FALSE(c.failedConditions.empty());
}

TEST(MockTaskDriven, UnknownFaultAndInvalidStageFailClosed) {
    EXPECT_THROW(parseScenario(R"({"scenario":"x","faults":["bogus"]})"),
                 std::runtime_error);
    EXPECT_THROW(parseScenario(
                     R"({"scenario":"x","executor":{"stages":[{"stage":"INSTALL","result":"MAYBE"}]}})"),
                 std::runtime_error);
    EXPECT_THROW(parseScenario(R"({"scenario":"x","packages":[{}]})"),
                 std::runtime_error);
}
