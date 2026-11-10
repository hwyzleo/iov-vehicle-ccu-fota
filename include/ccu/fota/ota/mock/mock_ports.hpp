#pragma once

// =============================================================================
// include/ccu/fota/ota/mock/mock_ports.hpp
// CCU-FOTA 车内 Mock 端口 (CCU-FOTA-DSN-CR-009 / CR-015 §D3–D4)
// =============================================================================
// 仅在 FOTA_ENABLE_TEST_DOUBLES 定义时可用；量产构建不得包含。Mock 与真实实现
// 共享同一端口、状态机和 payload，不分叉上层逻辑。
//
// CCU-FOTA-DSN-CR-015 关键约束：
//   * 业务事实（ECU/包/版本/计划节点/大小/摘要）来自冻结任务与授权响应，不使用
//     固定测试常量；场景只描述阶段、进度、结果与故障行为。
//   * 摘要（stage result / readiness / log）使用 framework-hash 与版本化规范化
//     输入，不使用固定占位值。
//   * Mock 不访问真实 CDN/对象存储，不读真实包文件，不发起真实刷写/重启。
// =============================================================================

#ifndef FOTA_ENABLE_TEST_DOUBLES
#error "mock_ports.hpp is only available with FOTA_ENABLE_TEST_DOUBLES (NON_PRODUCTION)"
#endif

#include "ccu/fota/ota/call_context.hpp"
#include "ccu/fota/ota/event_sink.hpp"
#include "ccu/fota/ota/fota_digest.hpp"
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

#include "fota_log_adapter.h"

#include <ccu/fw/hash/sha256.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {
namespace mock {

namespace detail {

inline std::int64_t realNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// 包身份 + 结果内容的稳定标识前缀（不同结果不复用同一 ID，重试同一结果保持相同 ID）。
inline std::string stableId(const std::string& domain, const std::string& content,
                            std::size_t hexChars = 32) {
    const std::string hex =
        ccu::fw::hash::sha256_hex(domain + "|" + content);
    return hex.substr(0, std::min(hexChars, hex.size()));
}

} // namespace detail

// ---------------------------------------------------------------------------
// MockInventoryProvider - 产生稳定内容清单（不决定 mode/revision，CR-012）
// 注意：CR-015 真实链路装配禁止使用本实现（仅隔离测试）。
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

        // CR-014 D2：总成件号、软件件号、软件版本与硬件版本取互不相同的值，
        // 防止测试继续固化旧交叉映射（partNumber<->software / hwVersion<->hardware）。
        ::vehicle::fota::v1::EcuVersion e;
        e.set_ecu_id("VCU-001");
        e.set_hardware_part_number(partNumber_);
        e.set_software_part_number(softwarePartNumber_);
        e.set_sw_version(swVersion_);
        e.set_hw_version(hwVersion_);
        info.ecuList.push_back(e);

        // 稳定规范化：与生产实现同构的确定性输入（Mock 仅用于测试）。
        const std::string canon = std::string("mock-ecu-list-v2|") + e.ecu_id() +
                                  "|" + partNumber_ + "|" +
                                  softwarePartNumber_ + "|" + swVersion_ + "|" +
                                  hwVersion_;
        info.ecuListDigest.set_algorithm("sha-256");
        info.ecuListDigest.set_value_hex(ccu::fw::hash::sha256_hex(canon));
        info.fingerprintCanonicalization = "cgw-fota-version-v2";
        info.fingerprintHex = ccu::fw::hash::sha256_hex(
            canon + "|fp|" + scenario_.baseline + "|" + scenario_.fotaMasterVersion);
        info.wireCanonicalization = "inventory-ecu-list-v2";
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
    // 四类字段取值互不相同（CR-014 D2）。
    std::string partNumber_ = "HT-VCU-001";          // 总成/备件件号
    std::string softwarePartNumber_ = "SW-VCU-001";  // 软件件号
    std::string hwVersion_ = "HWV-VCU-001";          // 硬件版本
    std::string swVersion_ = "1.0.0";                // 软件版本
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
// MockPackageDownloader - 任务驱动、无数据面 I/O 的确定性下载/校验 (CR-015 §D3)
//   * 只读取 PackageSummary（包身份/大小/摘要）与 DownloadGrantResponse
//     （packageRevision/ETag/Range），绝不访问 download_url。
//   * 成功：finalOffset=size，downloaded=size-fromOffset；fromOffset>size 失败。
//   * 失败：保留合法 STORED_OBJECT 偏移（不宣称完整下载），阶段结果 FAILED。
//   * stageResultId 由包身份 + 结果内容派生：重试同一结果保持相同 ID。
// ---------------------------------------------------------------------------
class MockPackageDownloader : public PackageDownloader {
public:
    explicit MockPackageDownloader(ScenarioScript s) : scenario_(std::move(s)) {}

    DownloadOutcome
    downloadAndVerify(const ::vehicle::fota::v1::PackageSummary& pkg,
                      const ::vehicle::fota::v1::DownloadGrantResponse& grant,
                      std::uint64_t fromOffsetBytes) override {
        DownloadOutcome out;
        const std::uint64_t size = pkg.size_bytes();
        const std::uint32_t attempt = ++attempts_[pkg.package_id()];
        const std::string revision = !grant.package_revision().empty()
                                         ? grant.package_revision()
                                         : pkg.package_revision();

        // 起始偏移超出包大小：非法 Range -> 失败，不产生成功阶段结果。
        if (fromOffsetBytes > size) {
            out.finalOffsetBytes = fromOffsetBytes;
            out.bytesDownloaded = 0;
            out.allStagesSucceeded = false;
            out.errorCode = "FOTA-PACKAGE-OFFSET";
            out.errorDetail = "offset_exceeds_size";
            makeStage(out, pkg, revision, attempt, fromOffsetBytes, size,
                      ::vehicle::fota::v1::RESULT_FAILED, false, false, true);
            return out;
        }

        const ScenarioPackage* sp = findScenarioPackage(pkg.package_id());
        std::string fault;
        if (sp != nullptr && attempt <=
                                static_cast<std::uint32_t>(sp->failuresBeforeSuccess)) {
            fault = "download_failed";
        } else if (scenario_.hasFault("download_disconnect") && !downDisconnectOnce_) {
            downDisconnectOnce_ = true;
            fault = "download_disconnect";
        } else if (scenario_.hasFault("hash_failed") && !hashFailedOnce_) {
            hashFailedOnce_ = true;
            fault = "hash_failed";
        } else if (scenario_.hasFault("signature_failed") && !signatureFailedOnce_) {
            signatureFailedOnce_ = true;
            fault = "signature_failed";
        } else if (scenario_.hasFault("decryption_failed") && !decryptionFailedOnce_) {
            decryptionFailedOnce_ = true;
            fault = "decryption_failed";
        }

        if (!fault.empty()) {
            // 失败尝试保留合法偏移：不推进到完整大小。
            out.finalOffsetBytes = fromOffsetBytes;
            out.bytesDownloaded = 0;
            out.allStagesSucceeded = false;
            out.errorCode = fault == "hash_failed"
                                ? "FOTA-PACKAGE-VERIFY"
                                : (fault == "signature_failed"
                                       ? "FOTA-PACKAGE-SIGNATURE"
                                       : (fault == "decryption_failed"
                                              ? "FOTA-PACKAGE-DECRYPT"
                                              : "FOTA-PACKAGE-DOWNLOAD"));
            out.errorDetail = fault;
            makeStage(out, pkg, revision, attempt, fromOffsetBytes, size,
                      ::vehicle::fota::v1::RESULT_FAILED,
                      fault != "hash_failed", fault != "signature_failed",
                      fault != "decryption_failed");
            return out;
        }

        out.finalOffsetBytes = size;
        out.bytesDownloaded = size - fromOffsetBytes;
        out.allStagesSucceeded = true;
        makeStage(out, pkg, revision, attempt, fromOffsetBytes, size,
                  ::vehicle::fota::v1::RESULT_SUCCEEDED, true, true, true);
        return out;
    }

private:
    ScenarioScript scenario_;
    std::map<std::string, std::uint32_t> attempts_;
    bool downDisconnectOnce_ = false;
    bool hashFailedOnce_ = false;
    bool signatureFailedOnce_ = false;
    bool decryptionFailedOnce_ = false;

    const ScenarioPackage* findScenarioPackage(const std::string& id) const {
        for (const auto& p : scenario_.packages) {
            if (p.packageId == id) return &p;
        }
        return nullptr;
    }

    void makeStage(DownloadOutcome& out,
                   const ::vehicle::fota::v1::PackageSummary& pkg,
                   const std::string& revision, std::uint32_t attempt,
                   std::uint64_t fromOffset, std::uint64_t size,
                   ::vehicle::fota::v1::Result result, bool hashVerified,
                   bool signatureVerified, bool decryptionSucceeded) {
        out.hasStageResult = true;
        auto& s = out.stageResult;
        s.set_package_id(pkg.package_id());
        // 包身份 + 结果内容 -> 稳定 stageResultId（重试同一结果同 ID）。
        const std::string identity =
            pkg.package_id() + "|" + revision + "|" + std::to_string(fromOffset) +
            "|" + std::to_string(size) + "|" + std::to_string(result) + "|" +
            out.errorDetail;
        s.set_stage_result_id("SR-" + detail::stableId("stage-result", identity));
        s.set_verified_package_revision(revision);
        s.set_result(result);
        s.set_downloaded_bytes(result == ::vehicle::fota::v1::RESULT_SUCCEEDED
                                   ? size - fromOffset
                                   : 0);
        if (pkg.has_package_digest() && !pkg.package_digest().value_hex().empty()) {
            *s.mutable_actual_package_digest() = pkg.package_digest();
        }
        s.set_hash_verified(hashVerified);
        s.set_signature_verified(signatureVerified);
        s.set_decryption_succeeded(decryptionSucceeded);
        if (result == ::vehicle::fota::v1::RESULT_FAILED) s.set_error_code(out.errorCode);
        // D4：确定性阶段结果摘要（不含时间/ID）。
        s.mutable_stage_result_digest()->set_algorithm("sha-256");
        s.mutable_stage_result_digest()->set_value_hex(
            digest::stageResultDigestHex(s));
        FotaLogAdapter::orchestrator().info(
            fota_events::MOCK_DOWNLOAD_RESULT, "Mock download/verify result",
            {flog::f_str("package_id", pkg.package_id()),
             flog::f_str("package_revision", revision),
             flog::f_str("result",
                         result == ::vehicle::fota::v1::RESULT_SUCCEEDED
                             ? "SUCCEEDED"
                             : "FAILED"),
             flog::f_int("attempt", static_cast<std::int64_t>(attempt)),
             flog::f_int("downloaded_bytes",
                         static_cast<std::int64_t>(s.downloaded_bytes())),
             flog::f_str("error_code", out.errorCode),
             flog::f_str("profile", "NON_PRODUCTION")});
        (void)attempt;
    }
};

// ---------------------------------------------------------------------------
// MockVehicleConditionProvider - 默认门禁通过；guard_failed 可配置
// localReadinessDigest 由条件集合 + 快照确定性计算（CR-015 §D4）。
// ---------------------------------------------------------------------------
class MockVehicleConditionProvider : public VehicleConditionProvider {
public:
    ConditionEvaluation evaluateConditions() override {
        ConditionEvaluation ev;
        ev.conditionSetVersion = "cond-v1";
        ev.snapshot.set_collected_at_ms(2000);
        ev.snapshot.set_traction_battery_soc(80);
        ev.snapshot.set_low_voltage_battery_voltage_v(12.6);
        ev.snapshot.set_ignition_state("off");
        ev.snapshot.set_gear("P");
        ev.snapshot.set_speed_kph(0.0);
        ev.snapshot.set_network_type("LTE");
        ev.snapshot.set_available_storage_bytes(1ULL << 30);
        ev.allGuardsPassed = !guardFailed_;
        if (guardFailed_) ev.failedConditions.push_back("battery");
        ev.localReadinessDigest =
            digest::localReadinessDigestHex(ev.conditionSetVersion, ev.snapshot);
        return ev;
    }
    bool allGuardsPassed() override { return !guardFailed_; }
    void setGuardFailed(bool f) { guardFailed_ = f; }

private:
    bool guardFailed_ = false;
};

// ---------------------------------------------------------------------------
// MockLogCollector - 生成确定性无敏感日志包；可配置失败
// 摘要由确定性内容计算（不使用固定占位值）。
// ---------------------------------------------------------------------------
class MockLogCollector : public LogCollector {
public:
    LogPackage collect(const LogCollectScope& scope) override {
        LogPackage p;
        if (failNext_) {
            p.generated = false;
            p.errorCode = "FOTA-LOG-COLLECT";
            return p;
        }
        const std::uint64_t seq = ++seq_;
        p.objectKey = "mock-log-" + std::to_string(seq);
        p.algorithm = "sha-256";
        p.sizeBytes = 256;
        std::string content = std::string("mock-log-v1|") + p.objectKey + "|" +
                              std::to_string(p.sizeBytes) + "|" + scope.logType;
        for (const auto& c : scope.collectionScope) content += "|" + c;
        p.digestHex = ccu::fw::hash::sha256_hex(content);
        p.generated = true;
        return p;
    }
    void setFailNext(bool f) { failNext_ = f; }

private:
    std::uint64_t seq_ = 0;
    bool failNext_ = false;
};

// ---------------------------------------------------------------------------
// MockInstallExecutor - 任务驱动阶段事件与最终结果 (CR-015 §D3–D5)
//   * prepare() 保存冻结任务；空任务包 fail-closed（不就绪，不伪造成功）。
//   * 事件/ECU 结果逐项映射冻结任务的 ecuId/packageId/planNodeId/source/target。
//   * eventId 由 executionId + attemptNo + 包 + 阶段 + 事件索引派生，重放稳定。
//   * occurred_at_ms 使用真实时间且同一 Execution 内单调不减。
//   * install_failed / 阶段 FAILED -> 失败结果；rollback_required -> 回滚结果。
//   * resume() 不重生成已 durable 的阶段事件（事件由既有 Outbox 补发）。
// ---------------------------------------------------------------------------
class MockInstallExecutor : public InstallExecutor {
public:
    explicit MockInstallExecutor(ScenarioScript s) : scenario_(std::move(s)) {}

    PrepareResult prepare(const ::vehicle::fota::v1::VehicleTaskSnapshot& task) override {
        task_ = task;
        prepared_ = true;
        failedPackages_.clear();
        failedStages_.clear();
        rollback_ = false;
        if (task.package_list_size() == 0) {
            // 空任务包 = 任务/配置异常：不得生成成功执行结果（fail-closed）。
            PrepareResult r;
            r.ready = false;
            r.reason = "empty task package list";
            return r;
        }
        PrepareResult r;
        r.ready = true;
        return r;
    }

    StartResult start(const ::vehicle::fota::v1::InstallPermitResponse& permit,
                      EventSink& sink) override {
        permit_ = permit;
        sink_ = &sink;
        eventIndex_ = 0;
        lastEventTs_ = 0;
        StartResult r;
        if (!prepared_ || task_.package_list_size() == 0) {
            r.started = false;
            r.reason = "executor not prepared or empty task package list";
            return r;
        }

        const bool injectFailure = scenario_.hasFault("install_failed") ||
                                   scenario_.hasFault("rollback_required");
        bool injected = false;
        std::size_t pkgIndex = 0;
        for (const auto& pkg : task_.package_list()) {
            bool packageFailed = false;
            std::size_t stageIndex = 0;
            for (const auto& st : scenario_.stages) {
                std::string status = st.result;
                if (!injected && injectFailure && pkgIndex == 0 && stageIndex == 0) {
                    injected = true;
                    status = "FAILED";
                }
                if (status == "FAILED") {
                    failedPackages_.insert(pkg.package_id());
                    failedStages_[pkg.package_id()] = st.stage;
                    packageFailed = true;
                    emitEvent(pkg, st, 0, "FAILED", "MOCK-INSTALL-FAILED", "");
                    break;
                }
                for (std::uint32_t progress : st.progress) {
                    emitEvent(pkg, st, progress, "SUCCEEDED", "", "");
                }
                emitEvent(pkg, st, 100, "SUCCEEDED", "", "");
                ++stageIndex;
            }
            if (!packageFailed && scenario_.hasFault("rollback_required") &&
                injected) {
                // 该包安装阶段成功但整体需回滚（场景显式要求）。
            }
            ++pkgIndex;
        }

        if (scenario_.hasFault("rollback_required")) {
            rollback_ = true;
            for (const auto& pkg : task_.package_list()) {
                ScenarioStage rollbackStage;
                rollbackStage.stage = "ROLLBACK";
                rollbackStage.result = "SUCCEEDED";
                emitEvent(pkg, rollbackStage, 100, "SUCCEEDED", "", "");
            }
        }

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
        // 已 durable 的事件由 Outbox 补发；恢复不得重复生成阶段事件。
        sink_ = &sink;
        ResumeResult r;
        r.resumed = true;
        return r;
    }

    FinalResultData readFinalResult() override {
        FinalResultData fi;
        bool allSucceeded = true;
        for (const auto& pkg : task_.package_list()) {
            const bool failed =
                failedPackages_.count(pkg.package_id()) > 0;
            ::vehicle::fota::v1::EcuResult er;
            er.set_ecu_id(pkg.ecu_id());
            er.set_source_version(pkg.source_version());
            er.set_target_version(pkg.target_version());
            er.set_plan_node_id(pkg.plan_node_id());
            er.set_package_id(pkg.package_id());
            er.set_version_read_status("ok");
            if (failed) {
                allSucceeded = false;
                er.set_actual_version(pkg.source_version());
                er.set_result(::vehicle::fota::v1::RESULT_FAILED);
                auto it = failedStages_.find(pkg.package_id());
                er.set_failure_stage(it != failedStages_.end() ? it->second
                                                               : "INSTALL");
            } else {
                er.set_actual_version(pkg.target_version());
                er.set_result(::vehicle::fota::v1::RESULT_SUCCEEDED);
            }
            fi.ecuResults.push_back(std::move(er));
        }
        const bool rollback =
            rollback_ || rollbackRequested_ ||
            scenario_.hasFault("rollback_required");
        if (allSucceeded && !rollback) {
            fi.actualBaselineCode = task_.target_baseline_code();
            fi.baselineStatus = "verified";
        } else {
            fi.actualBaselineCode.clear();
            fi.baselineStatus = "not_applied";
        }
        if (rollback) {
            for (auto& er : fi.ecuResults) {
                er.set_rollback_result(::vehicle::fota::v1::RESULT_SUCCEEDED);
            }
        }
        fi.rollbackOccurred = rollback;
        FotaLogAdapter::orchestrator().info(
            fota_events::MOCK_RESULT_ASSEMBLED, "Mock final result assembled",
            {flog::f_str("execution_id", permit_.execution_id()),
             flog::f_int("attempt_no",
                         static_cast<std::int64_t>(permit_.attempt_no())),
             flog::f_int("package_count",
                         static_cast<std::int64_t>(task_.package_list_size())),
             flog::f_int("failed_count",
                         static_cast<std::int64_t>(failedPackages_.size())),
             flog::f_bool("rollback", rollback),
             flog::f_str("baseline_status", fi.baselineStatus),
             flog::f_str("vehicle_task_id", task_.vehicle_task_id()),
             flog::f_str("profile", "NON_PRODUCTION")});
        return fi;
    }

    ::vehicle::fota::v1::InstallCheckpoint checkpoint() override {
        ::vehicle::fota::v1::InstallCheckpoint c;
        c.set_checkpoint_version(1);
        c.mutable_checkpoint_digest()->set_algorithm("sha-256");
        c.mutable_checkpoint_digest()->set_value_hex(
            detail::stableId("mock-checkpoint", executionKey()));
        if (!scenario_.stages.empty()) {
            c.set_stage(scenario_.stages.front().stage);
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
    std::int64_t lastEventTs_ = 0;
    std::uint64_t eventIndex_ = 0;
    bool prepared_ = false;
    bool rollbackRequested_ = false;
    bool aborted_ = false;
    bool rollback_ = false;
    std::set<std::string> failedPackages_;
    std::map<std::string, std::string> failedStages_;

    std::string executionKey() const {
        return permit_.execution_id() + ":" +
               std::to_string(permit_.attempt_no());
    }

    void emitEvent(const ::vehicle::fota::v1::PackageSummary& pkg,
                   const ScenarioStage& st, std::uint32_t progress,
                   const std::string& status, const std::string& errorCode,
                   const std::string& errorDetail) {
        if (sink_ == nullptr) return;
        ::vehicle::fota::v1::ExecutionEvent evt;
        // 事件身份由 execution + 包 + 阶段 + 事件索引确定（重放稳定）。
        const std::string identity = executionKey() + "|" + pkg.package_id() + "|" +
                                     st.stage + "|" + std::to_string(eventIndex_);
        evt.set_event_id("EVT-" + detail::stableId("execution-event", identity));
        ++eventIndex_;
        evt.set_attempt_no(permit_.attempt_no());
        evt.set_plan_node_id(pkg.plan_node_id());
        if (pkg.has_component_id() && !pkg.component_id().empty()) {
            evt.set_component_id(pkg.component_id());
        }
        evt.set_package_id(pkg.package_id());
        evt.set_ecu_id(pkg.ecu_id());
        const std::int64_t now = detail::realNowMs();
        lastEventTs_ = std::max(lastEventTs_, now);
        evt.set_occurred_at_ms(lastEventTs_);
        evt.set_stage(st.stage);
        evt.set_event_status(status);
        evt.set_progress(progress);
        evt.set_progress_scope("overall");
        evt.set_current_version(pkg.source_version());
        evt.set_target_version(pkg.target_version());
        if (!errorCode.empty()) evt.set_error_code(errorCode);
        if (!errorDetail.empty()) evt.set_error_detail(errorDetail);
        FotaLogAdapter::orchestrator().info(
            fota_events::MOCK_EXECUTION_STAGE, "Mock execution stage event",
            {flog::f_str("execution_id", permit_.execution_id()),
             flog::f_int("attempt_no",
                         static_cast<std::int64_t>(permit_.attempt_no())),
             flog::f_str("package_id", pkg.package_id()),
             flog::f_str("ecu_id", pkg.ecu_id()),
             flog::f_str("stage", st.stage),
             flog::f_str("vehicle_task_id", task_.vehicle_task_id()),
             flog::f_int("progress", static_cast<std::int64_t>(progress)),
             flog::f_str("event_id", evt.event_id()),
             flog::f_str("profile", "NON_PRODUCTION")});
        // 摘要由 FotaOrchestrator::emit() 统一计算并落盘（CR-015 §D4）。
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
