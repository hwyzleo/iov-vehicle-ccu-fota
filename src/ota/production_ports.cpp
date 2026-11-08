// =============================================================================
// src/ota/production_ports.cpp
// CCU-FOTA 契约 B 生产车内端口适配实现 (CCU-FOTA-DSN-CR-009/010/011)
// =============================================================================
// 复用契约 A SnapshotAssembler 生成 FULL 清单；其余边界以 fail-closed 保守实现
// 占位（独立治理边界，由后续配对 CR 替换为真实来源）。不包含测试桩/Mock。
// =============================================================================

#include "ccu/fota/ota/ports/production_ports.hpp"

#include "ccu/fota/fingerprint/canonical_encoder.hpp"
#include "ccu/fota/fingerprint/inventory_fingerprint.hpp"

#include "ccu/fw/hash/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <tuple>
#include <vector>

#if defined(__linux__)
#include <sys/timex.h>
#endif

namespace ccu_fota {
namespace ota {

namespace {
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// wire digest 的规范化域（CCU-FOTA-DSN-CR-012 §规范化）。
// 变更此字符串或字段布局 = 摘要域版本升级，须同步 golden 向量。
constexpr const char* kInventoryWireDomain = "inventory-ecu-list-v1";

// CanonicalEncoder::finalize() 的结果转 ByteView（sha256_hex 入参）。
ccu::fw::hash::ByteView binaryToView(const std::vector<std::uint8_t>& bytes) {
    return ccu::fw::hash::ByteView{bytes.data(), bytes.size()};
}
} // namespace

// ---------------------------------------------------------------------------
// InventoryProviderFromAssembler
// ---------------------------------------------------------------------------
InventoryProviderFromAssembler::InventoryProviderFromAssembler(
    std::shared_ptr<SnapshotAssembler> assembler, std::string fotaMasterVersion)
    : assembler_(std::move(assembler))
    , fotaMasterVersion_(std::move(fotaMasterVersion)) {}

CollectedInventory InventoryProviderFromAssembler::collectInventory() {
    CollectedInventory info;
    info.fotaMasterVersion = fotaMasterVersion_;
    info.collectedAtMs = nowMs();

    if (!assembler_) {
        // 没有可用采集源 -> collected=false（CR-012 D7：不上报、不分配）。
        return info;
    }
    VehicleSoftwareSnapshot snap;
    // CR-012 D2：任务检测路径不得在采集阶段分配序号。
    if (!assembler_->assembleSnapshot(snap, SeqPolicy::Defer)) {
        return info;  // 采集失败/节流/校验不过 -> 不上报
    }
    if (snap.ecu_list.empty()) {
        return info;  // 空清单不得作为 FULL 上报（D7）
    }

    info.baselineCode = snap.baseline_id.value_or("");

    // ---------------------------------------------------------------
    // 稳定排序 (CR-012 §规范化)：ecuId -> hardware -> software part -> swVersion。
    // 排序后同时用于 wire ecuList、wire digest 与本地版本指纹，避免 DIAG 返回顺序
    // 抖动导致 digest 漂移。
    // ---------------------------------------------------------------
    std::vector<const EcuVersionEntry*> ordered;
    ordered.reserve(snap.ecu_list.size());
    for (const auto& e : snap.ecu_list) ordered.push_back(&e);
    std::sort(ordered.begin(), ordered.end(),
              [](const EcuVersionEntry* a, const EcuVersionEntry* b) {
                  return std::make_tuple(a->ecu_id,
                                         a->hw_version.value_or(""),
                                         a->part_number.value_or(""),
                                         a->sw_version.value_or("")) <
                         std::make_tuple(b->ecu_id,
                                         b->hw_version.value_or(""),
                                         b->part_number.value_or(""),
                                         b->sw_version.value_or(""));
              });

    std::vector<std::vector<std::uint8_t>> canonItems;
    canonItems.reserve(ordered.size());
    info.ecuList.reserve(ordered.size());
    for (const auto* e : ordered) {
        if (e->ecu_id.empty()) continue;
        ::vehicle::fota::v1::EcuVersion ev;
        ev.set_ecu_id(e->ecu_id);
        if (e->hw_version) ev.set_hardware_part_number(*e->hw_version);
        if (e->part_number) ev.set_software_part_number(*e->part_number);
        if (e->sw_version) ev.set_sw_version(*e->sw_version);
        info.ecuList.push_back(std::move(ev));

        fingerprint::CanonicalEncoder sub("");
        sub.writeStringField(0x0001, e->ecu_id);
        sub.writeOptionalStringField(0x0002, e->hw_version);
        sub.writeOptionalStringField(0x0003, e->part_number);
        sub.writeOptionalStringField(0x0004, e->sw_version);
        canonItems.push_back(sub.finalize());
    }

    try {
        // wire digest：独立版本化的 wire 规范化域 inventory-ecu-list-v1。
        fingerprint::CanonicalEncoder wire(kInventoryWireDomain);
        wire.writeListField(0x0001, canonItems);
        info.ecuListDigest.set_algorithm("sha-256");
        info.ecuListDigest.set_value_hex(
            ccu::fw::hash::sha256_hex(binaryToView(wire.finalize())));

        // 本地内容变化判据：versionFingerprint（cgw-fota-version-v1，CR-006）。
        auto fp = fingerprint::buildVersionFingerprint(snap);
        info.fingerprintCanonicalization = fp.canonicalization;
        info.fingerprintHex = fp.hex();
    } catch (const std::exception&) {
        // 规范化/摘要失败 -> fail-closed（不产生空摘要、不上报）。
        info = CollectedInventory{};
        info.fotaMasterVersion = fotaMasterVersion_;
        info.collectedAtMs = nowMs();
        return info;
    }

    info.collected = true;
    return info;
}

// ---------------------------------------------------------------------------
// ConservativeConsentProvider - fail-closed：永不自动同意。
// ---------------------------------------------------------------------------
ConsentChoice ConservativeConsentProvider::requestConsent(
    const std::string& /*vehicleTaskId*/, const ConsentTermsId& terms) {
    ConsentChoice c;
    c.termsAvailable = false;
    c.terms = terms;
    c.userChoice = ::vehicle::fota::v1::CONSENT_STATUS_UNSPECIFIED;
    return c;
}

// ---------------------------------------------------------------------------
// ConservativePackageDownloader - fail-closed：永不伪造下载成功。
// ---------------------------------------------------------------------------
DownloadOutcome ConservativePackageDownloader::downloadAndVerify(
    const ::vehicle::fota::v1::PackageSummary& /*pkg*/,
    const ::vehicle::fota::v1::DownloadGrantResponse& /*grant*/,
    std::uint64_t /*fromOffsetBytes*/) {
    DownloadOutcome out;
    out.allStagesSucceeded = false;
    out.errorCode = "FOTA-PACKAGE-DOWNLOAD";
    out.errorDetail = "object store downloader not provisioned (conservative)";
    return out;
}

// ---------------------------------------------------------------------------
// ConservativeVehicleConditionProvider - fail-closed：门禁永不通过。
// ---------------------------------------------------------------------------
ConditionEvaluation ConservativeVehicleConditionProvider::evaluateConditions() {
    ConditionEvaluation ev;
    ev.conditionSetVersion = "cond-v1";
    ev.localReadinessDigest = std::string(64, '0');
    ev.allGuardsPassed = false;
    ev.failedConditions.push_back("guards_not_evaluated");
    ev.snapshot.set_collected_at_ms(nowMs());
    return ev;
}

// ---------------------------------------------------------------------------
// ConservativeInstallExecutor - fail-closed：prepare 永不就绪，绝不触发真实刷写。
// ---------------------------------------------------------------------------
PrepareResult ConservativeInstallExecutor::prepare(
    const ::vehicle::fota::v1::VehicleTaskSnapshot&) {
    PrepareResult r;
    r.ready = false;
    r.reason = "install executor not provisioned (conservative)";
    return r;
}

StartResult ConservativeInstallExecutor::start(
    const ::vehicle::fota::v1::InstallPermitResponse&, EventSink&) {
    StartResult r;
    r.started = false;
    r.reason = "install executor not provisioned (conservative)";
    return r;
}

ControlApplyOutcome ConservativeInstallExecutor::apply(
    const ::vehicle::fota::v1::ControlCommand&) {
    ControlApplyOutcome o;
    o.status = ::vehicle::fota::v1::CONTROL_ACK_STATUS_REJECTED;
    o.reason = "control executor not provisioned (conservative)";
    return o;
}

ResumeResult ConservativeInstallExecutor::resume(
    const ::vehicle::fota::v1::InstallCheckpoint&, EventSink&) {
    ResumeResult r;
    r.resumed = false;
    r.reason = "install executor not provisioned (conservative)";
    return r;
}

FinalResultData ConservativeInstallExecutor::readFinalResult() { return {}; }

::vehicle::fota::v1::InstallCheckpoint ConservativeInstallExecutor::checkpoint() {
    ::vehicle::fota::v1::InstallCheckpoint c;
    c.set_checkpoint_version(0);
    return c;
}

// ---------------------------------------------------------------------------
// ConservativeLogCollector - fail-closed：永不伪造日志包。
// ---------------------------------------------------------------------------
LogPackage ConservativeLogCollector::collect(const LogCollectScope&) {
    LogPackage p;
    p.generated = false;
    p.errorCode = "FOTA-LOG-COLLECT";
    return p;
}

// ---------------------------------------------------------------------------
// SystemClockSyncProvider - 只读判定系统时钟是否可用于协议绝对时间字段。
// ---------------------------------------------------------------------------
SystemClockSyncProvider::SystemClockSyncProvider(SystemClockSyncConfig cfg)
    : cfg_(cfg) {}

ClockSyncStatus SystemClockSyncProvider::status() {
    ClockSyncStatus st;
    st.systemTimeMs = nowMs();

    if (ready_.load(std::memory_order_acquire)) {
        st.synchronized = true;
        st.detail = "latched";
        return st;
    }

    // 1. 合理性下限：RTC 未设置/掉电回落 -> 绝对时间字段不可用。
    if (st.systemTimeMs < cfg_.minValidEpochMs) {
        st.detail = "system time below min_valid_epoch (RTC unset or reset)";
        return st;
    }

    // 2. 可信同步证据。
    const char* evidence = nullptr;
    if (cfg_.syncMarkerPath != nullptr && cfg_.syncMarkerPath[0] != '\0') {
        std::error_code ec;
        if (std::filesystem::exists(cfg_.syncMarkerPath, ec) && !ec) {
            evidence = "time-sync marker present";
        }
    }
#if defined(__linux__)
    if (evidence == nullptr) {
        // modes=0 -> 只读查询内核时间状态，不调整时钟。
        struct timex tx {};
        int state = ntp_adjtime(&tx);
        if (state >= 0 && state != TIME_ERROR && (tx.status & STA_UNSYNC) == 0) {
            evidence = "kernel clock synchronized (STA_UNSYNC cleared)";
        }
    }
    if (evidence == nullptr && cfg_.requireSyncEvidence) {
        st.detail = "no trusted time-sync evidence (STA_UNSYNC set, no marker)";
        return st;
    }
#else
    // 非 Linux 仅用于开发/测试主机：无内核同步标志来源，以合理性下限为准。
    if (evidence == nullptr) {
        evidence = "host clock above min_valid_epoch (non-linux build)";
    }
#endif

    st.synchronized = true;
    st.detail = evidence != nullptr ? evidence : "min_valid_epoch only";
    ready_.store(true, std::memory_order_release);
    return st;
}

} // namespace ota
} // namespace ccu_fota
