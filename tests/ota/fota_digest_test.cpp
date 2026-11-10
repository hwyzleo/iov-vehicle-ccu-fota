// =============================================================================
// tests/ota/fota_digest_test.cpp
// OTA 摘要规范化测试 (CCU-FOTA-DSN-CR-015 §D4 / 测试矩阵 4、6)
// =============================================================================
// 覆盖 package manifest / stage result / execution event / final result /
// local readiness 五类摘要：golden 向量、输入顺序扰动、时间与本地序号排除、
// 重试/重放一致性。
// =============================================================================

#include "ccu/fota/ota/fota_digest.hpp"

#include "vehicle/fota/v1/execution.pb.h"
#include "vehicle/fota/v1/package.pb.h"
#include "vehicle/fota/v1/task.pb.h"
#include "vehicle/fota/v1/types.pb.h"

#include <gtest/gtest.h>

using namespace ccu_fota::ota::digest;
using ::vehicle::fota::v1::ConditionSnapshot;
using ::vehicle::fota::v1::ConditionValue;
using ::vehicle::fota::v1::EcuResult;
using ::vehicle::fota::v1::ExecutionEvent;
using ::vehicle::fota::v1::FinalResultReport;
using ::vehicle::fota::v1::PackageSummary;
using ::vehicle::fota::v1::StageResultReport;
using ::vehicle::fota::v1::VehicleTaskSnapshot;

namespace {

bool isLowerHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

PackageSummary makePackage(const std::string& ecu, const std::string& id,
                           const std::string& target) {
    PackageSummary p;
    p.set_package_id(id);
    p.set_package_revision("rev-1");
    p.set_ecu_id(ecu);
    p.set_plan_node_id("NODE-" + ecu);
    p.set_source_version("1.0.0");
    p.set_target_version(target);
    p.set_size_bytes(4096);
    p.mutable_package_digest()->set_algorithm("sha-256");
    p.mutable_package_digest()->set_value_hex(std::string(64, 'a'));
    p.set_differential_type("full");
    p.set_encrypted(false);
    return p;
}

VehicleTaskSnapshot makeTask() {
    VehicleTaskSnapshot t;
    t.set_vehicle_task_id("VT-1");
    t.set_task_revision(1);
    t.set_target_baseline_code("BASE-T");
    *t.add_package_list() = makePackage("VCU", "PKG-VCU", "2.0.0");
    *t.add_package_list() = makePackage("BMS", "PKG-BMS", "3.1.0");
    return t;
}

StageResultReport makeStage() {
    StageResultReport s;
    s.set_package_id("PKG-VCU");
    s.set_stage_result_id("SR-1");
    s.set_verified_package_revision("rev-1");
    s.set_result(::vehicle::fota::v1::RESULT_SUCCEEDED);
    s.set_downloaded_bytes(4096);
    s.mutable_actual_package_digest()->set_algorithm("sha-256");
    s.mutable_actual_package_digest()->set_value_hex(std::string(64, 'a'));
    s.set_hash_verified(true);
    s.set_signature_verified(true);
    s.set_decryption_succeeded(true);
    s.set_verified_at_ms(1000);
    return s;
}

ExecutionEvent makeEvent() {
    ExecutionEvent e;
    e.set_event_id("EVT-1");
    e.set_attempt_no(1);
    e.set_plan_node_id("NODE-VCU");
    e.set_package_id("PKG-VCU");
    e.set_ecu_id("VCU");
    e.set_stage("INSTALL");
    e.set_event_status("SUCCEEDED");
    e.set_progress(50);
    e.set_progress_scope("overall");
    e.set_current_version("1.0.0");
    e.set_target_version("2.0.0");
    e.set_sequence_no(7);
    e.set_occurred_at_ms(2000);
    return e;
}

EcuResult makeEcuResult(const std::string& ecu, const std::string& pkg,
                        bool success) {
    EcuResult r;
    r.set_ecu_id(ecu);
    r.set_source_version("1.0.0");
    r.set_target_version(success ? "2.0.0" : "1.0.0");
    r.set_plan_node_id("NODE-" + ecu);
    r.set_package_id(pkg);
    r.set_actual_version(success ? "2.0.0" : "1.0.0");
    r.set_version_read_status("ok");
    r.set_result(success ? ::vehicle::fota::v1::RESULT_SUCCEEDED
                         : ::vehicle::fota::v1::RESULT_FAILED);
    if (!success) r.set_failure_stage("INSTALL");
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. package manifest：稳定排序 + 内容敏感 + golden
// ---------------------------------------------------------------------------
TEST(FotaDigest, PackageManifestStableAcrossInputOrder) {
    auto t1 = makeTask();
    auto t2 = makeTask();
    // 交换 package_list 输入顺序，摘要必须一致。
    t2.mutable_package_list()->SwapElements(0, 1);
    const std::string d1 = packageManifestDigestHex(t1);
    const std::string d2 = packageManifestDigestHex(t2);
    EXPECT_TRUE(isLowerHex64(d1));
    EXPECT_EQ(d1, d2);
}

TEST(FotaDigest, PackageManifestContentSensitive) {
    auto base = makeTask();
    auto changed = makeTask();
    changed.mutable_package_list(1)->set_target_version("3.2.0");
    EXPECT_NE(packageManifestDigestHex(base), packageManifestDigestHex(changed));

    auto encrypted = makeTask();
    encrypted.mutable_package_list(0)->set_encrypted(true);
    EXPECT_NE(packageManifestDigestHex(base), packageManifestDigestHex(encrypted));
}

TEST(FotaDigest, PackageManifestGolden) {
    // golden：字段集合/顺序/枚举编码变更必须升级 domain 并更新本向量。
    auto t = makeTask();
    t.clear_package_list();
    *t.add_package_list() = makePackage("VCU", "PKG-VCU", "2.0.0");
    EXPECT_EQ(packageManifestDigestHex(t),
              "bb3914d66485c3f59e7e959ed118d59aac87cfe1930c58f32d065ebc6747a947");
}

// ---------------------------------------------------------------------------
// 2. stage result：排除 stageResultId / verified_at_ms；结果敏感
// ---------------------------------------------------------------------------
TEST(FotaDigest, StageResultExcludesIdAndTime) {
    auto s1 = makeStage();
    auto s2 = makeStage();
    s2.set_stage_result_id("SR-OTHER");
    s2.set_verified_at_ms(999999);
    s2.set_duration_ms(12345);
    EXPECT_EQ(stageResultDigestHex(s1), stageResultDigestHex(s2));

    auto failed = makeStage();
    failed.set_result(::vehicle::fota::v1::RESULT_FAILED);
    EXPECT_NE(stageResultDigestHex(s1), stageResultDigestHex(failed));

    auto noDecryption = makeStage();
    noDecryption.clear_decryption_succeeded();
    EXPECT_NE(stageResultDigestHex(s1), stageResultDigestHex(noDecryption));
}

// ---------------------------------------------------------------------------
// 3. execution event：排除 sequenceNo / occurred_at；重放稳定
// ---------------------------------------------------------------------------
TEST(FotaDigest, EventExcludesSequenceAndTime) {
    auto e1 = makeEvent();
    auto e2 = makeEvent();
    e2.set_sequence_no(99);
    e2.set_occurred_at_ms(888888);
    EXPECT_EQ(executionEventDigestHex(e1), executionEventDigestHex(e2));

    auto failed = makeEvent();
    failed.set_event_status("FAILED");
    EXPECT_NE(executionEventDigestHex(e1), executionEventDigestHex(failed));

    auto progress = makeEvent();
    progress.set_progress(60);
    EXPECT_NE(executionEventDigestHex(e1), executionEventDigestHex(progress));
}

TEST(FotaDigest, EventGolden) {
    auto e = makeEvent();
    EXPECT_EQ(executionEventDigestHex(e), "4ca049de5e5015d0600342b08d9eb143ab7fd86df6f57e91d1ba21521d7f0f63");
}

// ---------------------------------------------------------------------------
// 4. final result：ECU 顺序无关、时间/序号无关、失败/回滚敏感
// ---------------------------------------------------------------------------
namespace {
FinalResultReport makeFinalResult() {
    FinalResultReport r;
    r.set_final_sequence_no(5);
    r.set_result(::vehicle::fota::v1::RESULT_SUCCEEDED);
    r.set_actual_baseline_code("BASE-T");
    r.set_baseline_status("verified");
    r.set_rollback_occurred(false);
    r.set_completed_at_ms(1234);
    *r.add_ecu_results() = makeEcuResult("VCU", "PKG-VCU", true);
    *r.add_ecu_results() = makeEcuResult("BMS", "PKG-BMS", true);
    return r;
}
} // namespace

TEST(FotaDigest, FinalResultStableAcrossEcuOrderAndTime) {
    auto r1 = makeFinalResult();
    auto r2 = makeFinalResult();
    r2.mutable_ecu_results()->SwapElements(0, 1);
    r2.set_completed_at_ms(9999);
    r2.set_final_sequence_no(77);
    EXPECT_EQ(finalResultDigestHex(r1), finalResultDigestHex(r2));

    auto failed = makeFinalResult();
    failed.set_result(::vehicle::fota::v1::RESULT_FAILED);
    failed.mutable_ecu_results(1)->set_result(
        ::vehicle::fota::v1::RESULT_FAILED);
    failed.mutable_ecu_results(1)->set_actual_version("1.0.0");
    EXPECT_NE(finalResultDigestHex(r1), finalResultDigestHex(failed));

    auto rollback = makeFinalResult();
    rollback.set_rollback_occurred(true);
    rollback.set_result(::vehicle::fota::v1::RESULT_ROLLED_BACK);
    EXPECT_NE(finalResultDigestHex(r1), finalResultDigestHex(rollback));
}

// ---------------------------------------------------------------------------
// 5. local readiness：排除 collected_at_ms；条件顺序无关
// ---------------------------------------------------------------------------
TEST(FotaDigest, ReadinessIgnoresCollectedAtAndConditionOrder) {
    ConditionSnapshot s1;
    s1.set_collected_at_ms(1000);
    s1.set_traction_battery_soc(80);
    s1.set_low_voltage_battery_voltage_v(12.6);
    s1.set_ignition_state("off");
    s1.set_gear("P");
    s1.set_speed_kph(0.0);
    s1.set_network_type("LTE");
    s1.set_available_storage_bytes(1ULL << 30);
    auto* c1 = s1.add_extended_conditions();
    c1->set_code("door");
    c1->set_value("closed");
    auto* c2 = s1.add_extended_conditions();
    c2->set_code("charge");
    c2->set_value("idle");

    ConditionSnapshot s2 = s1;
    s2.set_collected_at_ms(999999);
    s2.mutable_extended_conditions()->SwapElements(0, 1);

    const std::string d1 = localReadinessDigestHex("cond-v1", s1);
    const std::string d2 = localReadinessDigestHex("cond-v1", s2);
    EXPECT_TRUE(isLowerHex64(d1));
    EXPECT_EQ(d1, d2);

    s2.set_traction_battery_soc(30);
    EXPECT_NE(d1, localReadinessDigestHex("cond-v1", s2));
    EXPECT_NE(d1, localReadinessDigestHex("cond-v2", s1));
}

// ---------------------------------------------------------------------------
// 6. stage / final result / local readiness golden 向量
//    （字段集合/顺序/枚举编码变更 = 摘要域升级，必须同步更新下列向量）
// ---------------------------------------------------------------------------
TEST(FotaDigest, StageResultGolden) {
    EXPECT_EQ(stageResultDigestHex(makeStage()), "62424721adc28a9f47329ebfa0d22ea19cfa4d0ed8f3527f71236800a357e875");
}

TEST(FotaDigest, FinalResultGolden) {
    EXPECT_EQ(finalResultDigestHex(makeFinalResult()), "f0e2405fda7b26681c65b39f958291c9e5c3c6986c3eff1984cf1dd0dd5ac72f");
}

TEST(FotaDigest, LocalReadinessGolden) {
    ConditionSnapshot s;
    s.set_traction_battery_soc(80);
    s.set_low_voltage_battery_voltage_v(12.6);
    s.set_ignition_state("off");
    s.set_gear("P");
    s.set_speed_kph(0.0);
    s.set_network_type("LTE");
    s.set_available_storage_bytes(1ULL << 30);
    EXPECT_EQ(localReadinessDigestHex("cond-v1", s),
              "7acfdffae82e6fa3b950c07781d6a3bca1673c22b05266dd1dae0f0978b5ef9a");
}
