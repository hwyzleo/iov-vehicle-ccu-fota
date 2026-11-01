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

#include "ccu/fota/ota/ports/consent_provider.hpp"
#include "ccu/fota/ota/ports/install_executor.hpp"
#include "ccu/fota/ota/ports/inventory_provider.hpp"
#include "ccu/fota/ota/ports/log_collector.hpp"
#include "ccu/fota/ota/ports/package_downloader.hpp"
#include "ccu/fota/ota/ports/vehicle_condition_provider.hpp"

#include "data_models.h"
#include "snapshot_assembler.h"

#include "vehicle/fota/v1/reconcile.pb.h"

#include <memory>
#include <string>

namespace ccu_fota {
namespace ota {

// ---------------------------------------------------------------------------
// InventoryProviderFromAssembler - 生产清单提供者：复用契约 A SnapshotAssembler
// （DIAG 采集）映射为 vehicle.fota.v1 CollectedInventory（不平行创建第二套采集）。
// ---------------------------------------------------------------------------
class InventoryProviderFromAssembler : public InventoryProvider {
public:
    InventoryProviderFromAssembler(std::shared_ptr<SnapshotAssembler> assembler,
                                   std::string fotaMasterVersion);
    CollectedInventory
    collectInventory(::vehicle::fota::v1::InventoryMode mode) override;

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

} // namespace ota
} // namespace ccu_fota
