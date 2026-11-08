#pragma once

// =============================================================================
// include/ccu/fota/ota/mock/mock_ports.hpp
// CCU-FOTA Mock 端口 (CCU-FOTA-DSN-CR-009 §Mock / CR-011 类型校准)
// =============================================================================
// 仅在 FOTA_ENABLE_TEST_DOUBLES 定义时可用；量产构建不得包含。Mock 与真实实现
// 共享同一端口、状态机和 payload，不分叉上层逻辑。使用确定性时间/随机源与可复现
// 脚本，不含生产秘密。fake TBOX/cloud server 在 fake_vehicle_message_transport.hpp
// （实现通用 VehicleMessageTransport 端口，按 fully-qualified PayloadType 分发并
// 注入故障点）。本文件保留车内端口 Mock。
// =============================================================================

#ifndef FOTA_ENABLE_TEST_DOUBLES
#error "mock_ports.hpp is only available with FOTA_ENABLE_TEST_DOUBLES (NON_PRODUCTION)"
#endif

#include "ccu/fota/ota/call_context.hpp"
#include "ccu/fota/ota/event_sink.hpp"
#include "ccu/fota/ota/mock/scenario_script.hpp"
#include "ccu/fota/ota/ports/consent_provider.hpp"
#include "ccu/fota/ota/ports/install_executor.hpp"
#include "ccu/fota/ota/ports/installed_version_registrar.hpp"
#include "ccu/fota/ota/ports/inventory_provider.hpp"
#include "ccu/fota/ota/ports/log_collector.hpp"
#include "ccu/fota/ota/ports/package_downloader.hpp"
#include "ccu/fota/ota/ports/vehicle_condition_provider.hpp"

#include "vehicle/fota/v1/execution.pb.h"
#include "vehicle/fota/v1/task.pb.h"
#include "vehicle/fota/v1/types.pb.h"

#include <ccu/fw/hash/sha256.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <cstdint>
#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {
namespace mock {

// ---------------------------------------------------------------------------
// MockInventoryProvider - 产生稳定内容清单（不决定 mode/revision，CR-012）
// ---------------------------------------------------------------------------
class MockInventoryProvider : public InventoryProvider {
public:
    explicit MockInventoryProvider(ScenarioScript s) : scenario_(std::move(s)) {}

    CollectedInventory collectInventory() override {
        CollectedInventory info;
        info.fotaMasterVersion = scenario_.fotaMasterVersion;
        info.collectedAtMs = nowMs_;
        info.baselineCode = scenario_.baseline;
        if (!available_) return info;  // 采集失败：collected=false

        ::vehicle::fota::v1::EcuVersion e;
        e.set_ecu_id("VCU-001");
        e.set_hardware_part_number(hwVersion_);
        e.set_software_part_number("S-VCU-001");
        e.set_sw_version(swVersion_);
        info.ecuList.push_back(e);

        // 稳定规范化：与生产实现同构的确定性输入（Mock 仅用于测试）。
        const std::string canon = std::string("mock-ecu-list-v1|") + e.ecu_id() +
                                  "|" + hwVersion_ + "|" +
                                  e.software_part_number() + "|" + swVersion_;
        info.ecuListDigest.set_algorithm("sha-256");
        info.ecuListDigest.set_value_hex(ccu::fw::hash::sha256_hex(canon));
        info.fingerprintCanonicalization = "cgw-fota-version-v1";
        info.fingerprintHex = ccu::fw::hash::sha256_hex(
            canon + "|fp|" + scenario_.baseline + "|" + scenario_.fotaMasterVersion);
        info.collected = true;
        return info;
    }

    // 测试钩子：模拟 ECU 软件版本变化（内容指纹随之变化）。
    void setSoftwareVersion(std::string v) { swVersion_ = std::move(v); }
    // 测试钩子：模拟采集失败/空清单（上游 DIAG 不可达）。
    void setAvailable(bool available) { available_ = available; }
    void advanceClock(std::int64_t ms) { nowMs_ = ms; }

private:
    ScenarioScript scenario_;
    std::int64_t nowMs_ = 1000;
    std::string hwVersion_ = "P-VCU-001";
    std::string swVersion_ = "1.0.0";
    bool available_ = true;
};

// ---------------------------------------------------------------------------
// MockConsentProvider - 默认 ACCEPTED；可配置拒绝
// ---------------------------------------------------------------------------
class MockConsentProvider : public ConsentProvider {
public:
    MockConsentProvider() = default;
    ConsentChoice requestConsent(const std::string& /*vehicleTaskId*/,
                                 const ConsentTermsId& terms) override {
        ConsentChoice c;
        c.termsAvailable = true;
        c.terms = terms;
        c.userChoice = rejected_ ? ::vehicle::fota::v1::CONSENT_STATUS_REJECTED
                                 : ::vehicle::fota::v1::CONSENT_STATUS_ACCEPTED;
        return c;
    }
    void setRejected(bool r) { rejected_ = r; }

private:
    bool rejected_ = false;
};

// ---------------------------------------------------------------------------
// MockPackageDownloader - 按 scenario 模拟下载/校验，支持故障点
// ---------------------------------------------------------------------------
class MockPackageDownloader : public PackageDownloader {
public:
    explicit MockPackageDownloader(ScenarioScript s) : scenario_(std::move(s)) {}

    DownloadOutcome
    downloadAndVerify(const ::vehicle::fota::v1::PackageSummary& pkg,
                      const ::vehicle::fota::v1::DownloadGrantResponse& grant,
                      std::uint64_t fromOffsetBytes) override {
        DownloadOutcome out;
        out.finalOffsetBytes = pkg.size_bytes();
        out.bytesDownloaded = pkg.size_bytes() - fromOffsetBytes;

        if (scenario_.hasFault("download_disconnect") && !grantFailedOnce_) {
            grantFailedOnce_ = true;
            out.allStagesSucceeded = false;
            out.errorCode = "FOTA-PACKAGE-DOWNLOAD";
            out.errorDetail = "download_disconnect";
            makeStage(out, pkg, ::vehicle::fota::v1::RESULT_FAILED, "download_disconnect");
            return out;
        }
        if (scenario_.hasFault("hash_failed") && !hashFailedOnce_) {
            hashFailedOnce_ = true;
            out.allStagesSucceeded = false;
            out.errorCode = "FOTA-PACKAGE-VERIFY";
            out.errorDetail = "hash_failed";
            makeStage(out, pkg, ::vehicle::fota::v1::RESULT_FAILED, "hash_failed");
            return out;
        }

        out.allStagesSucceeded = true;
        makeStage(out, pkg, ::vehicle::fota::v1::RESULT_SUCCEEDED, "");
        return out;
    }

private:
    ScenarioScript scenario_;
    bool grantFailedOnce_ = false;
    bool hashFailedOnce_ = false;

    void makeStage(DownloadOutcome& out, const ::vehicle::fota::v1::PackageSummary& pkg,
                   ::vehicle::fota::v1::Result r, const std::string& detail) {
        out.hasStageResult = true;
        auto& s = out.stageResult;
        s.set_package_id(pkg.package_id());
        s.set_stage_result_id("SR-" + pkg.package_id() + "-1");
        s.mutable_stage_result_digest()->set_algorithm("sha-256");
        s.mutable_stage_result_digest()->set_value_hex(std::string(64, 'b'));
        s.set_verified_package_revision(pkg.package_revision());
        s.set_result(r);
        s.set_downloaded_bytes(out.bytesDownloaded);
        s.set_hash_verified(r == ::vehicle::fota::v1::RESULT_SUCCEEDED);
        s.set_signature_verified(r == ::vehicle::fota::v1::RESULT_SUCCEEDED);
        s.set_decryption_succeeded(r == ::vehicle::fota::v1::RESULT_SUCCEEDED);
        if (r == ::vehicle::fota::v1::RESULT_FAILED) s.set_error_code(out.errorCode);
        if (!detail.empty()) s.mutable_actual_package_digest()->set_algorithm("sha-256");
    }
};

// ---------------------------------------------------------------------------
// MockVehicleConditionProvider - 默认门禁通过；guard_failed 可配置
// ---------------------------------------------------------------------------
class MockVehicleConditionProvider : public VehicleConditionProvider {
public:
    ConditionEvaluation evaluateConditions() override {
        ConditionEvaluation ev;
        ev.conditionSetVersion = "cond-v1";
        ev.localReadinessDigest = std::string(64, 'c');
        ev.allGuardsPassed = !guardFailed_;
        ev.snapshot.set_collected_at_ms(2000);
        ev.snapshot.set_traction_battery_soc(80);
        ev.snapshot.set_low_voltage_battery_voltage_v(12.6);
        ev.snapshot.set_ignition_state("off");
        ev.snapshot.set_gear("P");
        ev.snapshot.set_speed_kph(0.0);
        ev.snapshot.set_network_type("LTE");
        ev.snapshot.set_available_storage_bytes(1ULL << 30);
        if (guardFailed_) {
            ev.failedConditions.push_back("battery");
        }
        return ev;
    }
    bool allGuardsPassed() override { return !guardFailed_; }
    void setGuardFailed(bool f) { guardFailed_ = f; }

private:
    bool guardFailed_ = false;
};

// ---------------------------------------------------------------------------
// MockLogCollector - 生成确定性无敏感日志包；可配置失败
// ---------------------------------------------------------------------------
class MockLogCollector : public LogCollector {
public:
    LogPackage collect(const LogCollectScope& /*scope*/) override {
        LogPackage p;
        if (failNext_) {
            p.generated = false;
            p.errorCode = "FOTA-LOG-COLLECT";
            return p;
        }
        p.objectKey = "mock-log-" + std::to_string(++seq_);
        p.digestHex = std::string(64, 'd');
        p.algorithm = "sha-256";
        p.sizeBytes = 256;
        p.generated = true;
        return p;
    }
    void setFailNext(bool f) { failNext_ = f; }

private:
    std::uint64_t seq_ = 0;
    bool failNext_ = false;
};

// ---------------------------------------------------------------------------
// MockInstallExecutor - 按 scenario 驱动阶段，经 EventSink 投递事件
// 支持 install_failed、rollback_required、pause_at_safe_point、restart_after_checkpoint
// ---------------------------------------------------------------------------
class MockInstallExecutor : public InstallExecutor {
public:
    explicit MockInstallExecutor(ScenarioScript s) : scenario_(std::move(s)) {}

    PrepareResult prepare(const ::vehicle::fota::v1::VehicleTaskSnapshot& task) override {
        task_ = task;
        PrepareResult r;
        r.ready = true;
        return r;
    }

    StartResult start(const ::vehicle::fota::v1::InstallPermitResponse& permit,
                      EventSink& sink) override {
        permit_ = permit;
        sink_ = &sink;
        stageIdx_ = 0;
        progressIdx_ = 0;
        for (stageIdx_ = 0; stageIdx_ < scenario_.stages.size(); ++stageIdx_) {
            const auto& st = scenario_.stages[stageIdx_];
            for (progressIdx_ = 0; progressIdx_ < st.progress.size(); ++progressIdx_) {
                emitEvent(st, st.progress[progressIdx_], "SUCCEEDED");
            }
            emitEvent(st, 100, st.result.empty() ? "SUCCEEDED" : st.result);
            progressIdx_ = 0;
        }
        StartResult r;
        r.started = true;
        return r;
    }

    ControlApplyOutcome apply(const ::vehicle::fota::v1::ControlCommand& cmd) override {
        ControlApplyOutcome o;
        if (cmd.action() == ::vehicle::fota::v1::CONTROL_ACTION_PAUSE) {
            o.status = ::vehicle::fota::v1::CONTROL_ACK_STATUS_APPLIED;
        } else if (cmd.action() == ::vehicle::fota::v1::CONTROL_ACTION_ROLLBACK) {
            o.status = ::vehicle::fota::v1::CONTROL_ACK_STATUS_APPLIED;
            rollbackRequested_ = true;
        } else if (cmd.action() == ::vehicle::fota::v1::CONTROL_ACTION_ABORT) {
            o.status = ::vehicle::fota::v1::CONTROL_ACK_STATUS_APPLIED;
            aborted_ = true;
        } else {
            o.status = ::vehicle::fota::v1::CONTROL_ACK_STATUS_DEFERRED;
        }
        o.reason = "mock apply";
        return o;
    }

    ResumeResult resume(const ::vehicle::fota::v1::InstallCheckpoint& /*ckpt*/,
                        EventSink& sink) override {
        sink_ = &sink;
        ResumeResult r;
        r.resumed = true;
        return r;
    }

    FinalResultData readFinalResult() override {
        FinalResultData fi;
        fi.actualBaselineCode = scenario_.baseline;
        fi.baselineStatus = "verified";
        fi.rollbackOccurred = rollbackRequested_;
        ::vehicle::fota::v1::EcuResult er;
        er.set_ecu_id("VCU-001");
        er.set_source_version("1.0.0");
        er.set_target_version("2.0.0");
        er.set_plan_node_id("NODE-1");
        er.set_package_id("PKG-VCU-001");
        er.set_actual_version("2.0.0");
        er.set_version_read_status("ok");
        er.set_result(::vehicle::fota::v1::RESULT_SUCCEEDED);
        fi.ecuResults.push_back(std::move(er));
        return fi;
    }

    ::vehicle::fota::v1::InstallCheckpoint checkpoint() override {
        ::vehicle::fota::v1::InstallCheckpoint c;
        c.set_checkpoint_version(1);
        c.mutable_checkpoint_digest()->set_algorithm("sha-256");
        c.mutable_checkpoint_digest()->set_value_hex(std::string(64, 'c'));
        if (stageIdx_ < scenario_.stages.size()) {
            c.set_stage(scenario_.stages[stageIdx_].stage);
        }
        return c;
    }

    bool rollbackRequested() const { return rollbackRequested_; }
    bool aborted() const { return aborted_; }

private:
    ScenarioScript scenario_;
    ::vehicle::fota::v1::VehicleTaskSnapshot task_;
    ::vehicle::fota::v1::InstallPermitResponse permit_;
    EventSink* sink_ = nullptr;
    std::size_t stageIdx_ = 0;
    std::size_t progressIdx_ = 0;
    std::int64_t eventTs_ = 0;
    bool rollbackRequested_ = false;
    bool aborted_ = false;

    void emitEvent(const ScenarioStage& st, std::uint32_t progress,
                   const std::string& eventStatus) {
        if (!sink_) return;
        ::vehicle::fota::v1::ExecutionEvent evt;
        evt.set_event_id("EVT-" + std::to_string(eventTs_ + 1));
        evt.mutable_event_digest()->set_algorithm("sha-256");
        evt.mutable_event_digest()->set_value_hex(std::string(64, 'f'));
        evt.set_attempt_no(permit_.attempt_no());
        evt.set_plan_node_id("NODE-1");
        evt.set_package_id("PKG-VCU-001");
        evt.set_ecu_id("VCU-001");
        evt.set_occurred_at_ms(++eventTs_);
        evt.set_stage(st.stage);
        evt.set_event_status(eventStatus);
        evt.set_progress(progress);
        evt.set_progress_scope("overall");
        evt.set_current_version("1.0.0");
        evt.set_target_version("2.0.0");
        sink_->emit(evt);
    }
};

// ---------------------------------------------------------------------------
// FakeInstalledVersionRegistrar - 与生产实现共享同一端口与同一幂等语义的
// CCU-FOTA-DSN-CR-013 测试端（Fake DIAG 侧）：
//   * 记录每次调用（次数/身份/items），供“登记恰一次”断言；
//   * 以 executionId+attemptNo 去重：重复键返回 DUPLICATE（不重复应用）；
//   * 可注入失败处置序列（RETRYABLE_FAILED/UNAVAILABLE/REJECTED_INVALID）；
//   * 可切换 available()，模拟 Registry 未分配/接口未就绪/服务不可用。
// ---------------------------------------------------------------------------
class FakeInstalledVersionRegistrar : public InstalledVersionRegistrar {
public:
    bool available() override { return availableFlag && enabled; }

    RegistrationResult registerInstalledVersions(
        const InstalledVersionsReport& report, CallContext&) override {
        ++callCount;
        calls.push_back(report);

        RegistrationResult r;
        // 1. 注入的失败处置（不改变幂等状态，模拟 DIAG 未受理）。
        //    每次调用消费一个脚本项；脚本耗尽后回到正常受理路径。
        if (!scriptedDispositions.empty()) {
            r.disposition = scriptedDispositions.front();
            scriptedDispositions.pop_front();
            if (r.disposition == RegistrationDisposition::Duplicate) {
                // 模拟 DIAG 幂等回放：返回既有登记标识，不再变更清单。
                r.registrationId = "REG-EXISTING";
            } else if (r.disposition == RegistrationDisposition::RejectedInvalid) {
                RejectedRegistrationItem item;
                item.ecuId = report.items.empty() ? "" : report.items.front().ecuId;
                item.reasonCode = "UNKNOWN_ECU";
                r.rejectedItems.push_back(item);
                r.errorCode = "FOTA-INVENTORY-REGISTRATION-REJECTED";
            } else if (r.disposition == RegistrationDisposition::RetryableFailed) {
                r.errorCode = "FOTA-INVENTORY-REGISTRATION-FAILED";
            } else if (r.disposition == RegistrationDisposition::Unavailable) {
                r.errorCode = "FOTA-INVENTORY-REGISTRATION-UNAVAILABLE";
            }
            return r;
        }

        // 2. 幂等去重：同一 executionId+attemptNo 返回既有 registrationId。
        auto it = applied.find(report.idempotencyKey);
        if (it != applied.end()) {
            r.disposition = RegistrationDisposition::Duplicate;
            r.registrationId = it->second;
            r.appliedCount = 0;
            ++duplicateCount;
            return r;
        }

        // 3. 正常受理：单次原子“应用”，记录登记标识。
        const std::string registrationId =
            "REG-" + std::to_string(applied.size() + 1);
        applied.emplace(report.idempotencyKey, registrationId);
        r.disposition = RegistrationDisposition::Applied;
        r.registrationId = registrationId;
        r.appliedCount = static_cast<std::uint32_t>(report.items.size());
        r.effectiveRevision = ++effectiveRevision;
        appliedItems[idempotencyKey(report)] = report.items;
        ++appliedCountTotal;
        return r;
    }

    static std::string idempotencyKey(const InstalledVersionsReport& r) {
        return r.executionId + ":" + std::to_string(r.attemptNo);
    }

    // ---- 测试控制 ----
    bool availableFlag = true;
    bool enabled = true;
    // 依次消费的失败处置；脚本耗尽后按正常受理路径（幂等）处理。
    std::deque<RegistrationDisposition> scriptedDispositions;

    int callCount = 0;
    int appliedCountTotal = 0;
    int duplicateCount = 0;
    std::uint64_t effectiveRevision = 0;
    std::vector<InstalledVersionsReport> calls;
    std::map<std::string, std::string> applied;  // key -> registrationId
    // 每个幂等键实际“应用”的 items（用于断言重试不改变内容且只应用一次）。
    std::map<std::string, std::vector<InstalledVersionItem>> appliedItems;
};

} // namespace mock
} // namespace ota
} // namespace ccu_fota
