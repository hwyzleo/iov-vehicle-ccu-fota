#pragma once

// =============================================================================
// include/ccu/fota/ota/ports/production_ports.hpp
// CCU-FOTA 契约 B 生产车内端口适配 (CCU-FOTA-DSN-CR-009 §车内执行 / CR-010/011)
// =============================================================================
// 生产 daemon 装配 FotaOrchestrator 所需的车内端口生产实现。原则：
//   * 尽量复用既有真实抽象（清单采集复用契约 A 的 SnapshotAssembler，不建立
//     第二套采集/传输）。
//   * 尚未具备真实来源的边界（用户授权 HMI/TBOX、对象存储下载、真实 ECU
//     Installer、整车条件、日志对象上传）以 fail-closed 保守实现占位：绝不伪造
//     成功、绝不自动安装、绝不自动同意。这些是独立治理边界（CR-009 §影响与边界），
//     由后续配对 CR 以真实实现替换；本类不包含任何测试桩/Mock 凭据。
//   * 本文件为量产代码，不依赖 FOTA_ENABLE_TEST_DOUBLES。
// =============================================================================

#include "ccu/fota/ota/ports/clock_sync_provider.hpp"
#include "ccu/fota/ota/ports/consent_provider.hpp"
#include "ccu/fota/ota/ports/install_executor.hpp"
#include "ccu/fota/ota/ports/inventory_provider.hpp"
#include "ccu/fota/ota/ports/log_collector.hpp"
#include "ccu/fota/ota/ports/package_downloader.hpp"
#include "ccu/fota/ota/ports/vehicle_condition_provider.hpp"

#include "data_models.h"
#include "snapshot_assembler.h"

#include "vehicle/fota/v1/reconcile.pb.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace ccu_fota {
namespace ota {

// ---------------------------------------------------------------------------
// InventoryProviderFromAssembler - 生产清单提供者：复用契约 A SnapshotAssembler
// （DIAG 采集）映射为 vehicle.fota.v1 CollectedInventory（不平行创建第二套采集）。
// CCU-FOTA-DSN-CR-012：只负责采集 + 稳定排序 + digest + 版本指纹，不决定
// mode/revision，也不分配序号（Defer）。
// ---------------------------------------------------------------------------
class InventoryProviderFromAssembler : public InventoryProvider {
public:
    InventoryProviderFromAssembler(std::shared_ptr<SnapshotAssembler> assembler,
                                   std::string fotaMasterVersion);
    CollectedInventory collectInventory() override;

private:
    std::shared_ptr<SnapshotAssembler> assembler_;
    std::string fotaMasterVersion_;
};

// ---------------------------------------------------------------------------
// ConservativeConsentProvider - 尚无真实用户授权来源时的 fail-closed 实现。
// 永不返回 ACCEPTED（绝不自动同意）；返回 UNSPECIFIED + termsAvailable=false，
// 由真实 HMI/TBOX 授权来源 CR 替换。
// ---------------------------------------------------------------------------
class ConservativeConsentProvider : public ConsentProvider {
public:
    ConsentChoice
    requestConsent(const std::string& vehicleTaskId, const ConsentTermsId& terms) override;
};

// ---------------------------------------------------------------------------
// ConservativePackageDownloader - 尚无对象存储下载来源时的 fail-closed 实现。
// 永不伪造下载成功；返回下载失败错误码。
// ---------------------------------------------------------------------------
class ConservativePackageDownloader : public PackageDownloader {
public:
    DownloadOutcome
    downloadAndVerify(const ::vehicle::fota::v1::PackageSummary& pkg,
                      const ::vehicle::fota::v1::DownloadGrantResponse& grant,
                      std::uint64_t fromOffsetBytes) override;
};

// ---------------------------------------------------------------------------
// ConservativeVehicleConditionProvider - 尚无整车条件来源时的 fail-closed 实现。
// 门禁永不通过（绝不自动进入安装）。
// ---------------------------------------------------------------------------
class ConservativeVehicleConditionProvider : public VehicleConditionProvider {
public:
    ConditionEvaluation evaluateConditions() override;
    bool allGuardsPassed() override { return false; }
};

// ---------------------------------------------------------------------------
// ConservativeInstallExecutor - 尚无真实 ECU Installer 时的 fail-closed 实现。
// prepare 永不就绪（绝不触发真实刷写副作用）。
// ---------------------------------------------------------------------------
class ConservativeInstallExecutor : public InstallExecutor {
public:
    PrepareResult prepare(const ::vehicle::fota::v1::VehicleTaskSnapshot& task) override;
    StartResult start(const ::vehicle::fota::v1::InstallPermitResponse& permit,
                      EventSink& sink) override;
    ControlApplyOutcome apply(const ::vehicle::fota::v1::ControlCommand& cmd) override;
    ResumeResult resume(const ::vehicle::fota::v1::InstallCheckpoint& ckpt,
                        EventSink& sink) override;
    FinalResultData readFinalResult() override;
    ::vehicle::fota::v1::InstallCheckpoint checkpoint() override;
};

// ---------------------------------------------------------------------------
// ConservativeLogCollector - 尚无日志对象上传来源时的 fail-closed 实现。
// 永不伪造日志包；返回采集失败错误码。
// ---------------------------------------------------------------------------
class ConservativeLogCollector : public LogCollector {
public:
    LogPackage collect(const LogCollectScope& scope) override;
};

// ---------------------------------------------------------------------------
// SystemClockSyncConfig - 系统时钟就绪判定参数。
// ---------------------------------------------------------------------------
struct SystemClockSyncConfig {
    // 合理性下限：CLOCK_REALTIME 早于该时刻一律视为未同步（RTC 未设置或掉电回落）。
    // 缺省 2025-01-01T00:00:00Z，必须早于任何可能的真实车辆运行时间。
    std::int64_t minValidEpochMs = 1735689600000LL;
    // 授时服务同步标记路径（systemd-timesyncd 约定）。为空则跳过该证据来源。
    const char* syncMarkerPath = "/run/systemd/timesync/synchronized";
    // true：Linux 上必须拿到可信同步证据（内核 STA_UNSYNC 已清除或标记文件存在）；
    // false：仅以 minValidEpochMs 合理性下限判定（实验台/无授时服务环境）。
    bool requireSyncEvidence = true;
};

// ---------------------------------------------------------------------------
// SystemClockSyncProvider - 生产时钟就绪判定（只读，绝不修改系统时间）。
//
// 判定顺序（全部满足才算就绪）：
//   1. CLOCK_REALTIME >= minValidEpochMs（排除 1970/出厂时间）；
//   2. 可信同步证据之一：授时服务同步标记文件存在，或内核时间状态已清除
//      STA_UNSYNC（Linux ntp_adjtime 只读查询）。requireSyncEvidence=false
//      时跳过本步。
// 非 Linux 主机（仅开发/测试构建）没有内核同步标志来源，退化为只做第 1 步。
//
// 一旦观察到就绪即锁存，避免标记文件被清理或瞬时查询失败导致门禁抖动、业务反复停摆。
// ---------------------------------------------------------------------------
class SystemClockSyncProvider : public ClockSyncProvider {
public:
    explicit SystemClockSyncProvider(SystemClockSyncConfig cfg = {});
    ClockSyncStatus status() override;

private:
    SystemClockSyncConfig cfg_;
    std::atomic<bool> ready_{false};  // 锁存位：就绪后不再回退
};

} // namespace ota
} // namespace ccu_fota
