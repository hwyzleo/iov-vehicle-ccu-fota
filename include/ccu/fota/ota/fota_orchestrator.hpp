#pragma once

// =============================================================================
// include/ccu/fota/ota/fota_orchestrator.hpp
// CCU-FOTA 车云 FOTA 编排器 (CCU-FOTA-DSN-CR-009 §13.1/§13.4/§13.5 / CR-011)
// =============================================================================
// FotaOrchestrator 是 Task/VehicleTask/Execution 编排唯一入口；协议适配、状态推进
// 和车内执行分离。状态推进按 durable 边界分步执行：step() 依据当前 VehicleTask
// 状态执行下一阶段动作并 durable 保存。同时实现 EventSink，executor 事件先
// durable 写入 outbox（fota.event_outbox:<seq> + 水位 meta）再发送严格递增
// sequenceNo。
//
// 业务重试使用 durable 状态和同一幂等身份；framework 自动重试关闭。传输失败
// （FotaCloudException）由 step() 捕获并保留状态，不推进，等待下次 step 重试。
//
// CCU-FOTA-DSN-CR-011：本模块消费 vehicle.fota.v1 正式契约，业务身份只由
// VehicleMessageEnvelope 承载（payload 不内嵌信封）；task/control/inventory
// revision 为 uint64；Store 使用 fota.* key（含 per-item downloads/event_outbox/
// controls/log_jobs）。
// =============================================================================

#include "ccu/fota/ota/call_context.hpp"
#include "ccu/fota/ota/event_sink.hpp"
#include "ccu/fota/ota/fota_cloud_proxy.hpp"
#include "ccu/fota/ota/ports/clock_sync_provider.hpp"
#include "ccu/fota/ota/ports/consent_provider.hpp"
#include "ccu/fota/ota/ports/install_executor.hpp"
#include "ccu/fota/ota/ports/inventory_provider.hpp"
#include "ccu/fota/ota/ports/log_collector.hpp"
#include "ccu/fota/ota/ports/package_downloader.hpp"
#include "ccu/fota/ota/ports/vehicle_condition_provider.hpp"
#include "ccu/fota/ota/state/execution_state.hpp"
#include "ccu/fota/ota/state/vehicle_task_state.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"

#include "vehicle/fota/v1/types.pb.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ccu_fota {
namespace ota {

struct FotaOrchestratorConfig {
    std::string protocolVersion = "fota-v1";
    std::string deviceId = "dev-mock";
    std::string vin = "MOCKVIN0000000001";
    std::chrono::milliseconds cloudCallTimeout{10000};
    std::chrono::milliseconds taskCheckInterval{60000};
    bool reconcileOnStart = true;
    std::uint32_t eventOutboxMax = 4096;
    // 时钟门禁：系统时钟与可信时间源同步前不推进任何上行动作（含启动期版本/清单
    // 上报）。Envelope 的 timestamp_ms/expire_at_ms 由 CLOCK_REALTIME 推导，时钟
    // 未同步时 expire_at_ms 落在过去，云侧网关按契约判定 MESSAGE_EXPIRED 丢入 DLQ。
    bool requireClockSync = true;
    // 等待时钟同步期间的轮询间隔。远小于 taskCheckInterval，使同步完成后立即上报，
    // 不必等满一个任务检测周期。
    std::chrono::milliseconds clockSyncPollInterval{1000};
};

struct StepOutcome {
    VehicleTaskState vehicleTaskState = VehicleTaskState::None;
    ExecutionState executionState = ExecutionState::PermitPersisted;
    bool terminal = false;
    std::string action;     // 本次动作描述（checkTask/download/...）
    std::string error;      // 非空表示本次失败（含 transport cause）
    bool transportFailed = false;
    // true 表示本次未推进的唯一原因是系统时钟未同步（未发出任何上行）。
    // 调用方据此以 clockSyncPollInterval 而非 taskCheckInterval 重试。
    bool awaitingClockSync = false;
};

// 控制应用结果（来自 InstallExecutor.apply）。
struct ControlOutcome {
    ::vehicle::fota::v1::ControlAckStatus status =
        ::vehicle::fota::v1::CONTROL_ACK_STATUS_REJECTED;
    std::string reason;
};

class FotaOrchestrator : public EventSink {
public:
    // clockSync 为时钟就绪判定来源。传 nullptr 表示不启用时钟门禁（纯状态机单测
    // 用法）；生产装配必须注入，由 FotaDaemon::create 在 requireClockSync=true
    // 时强制校验。
    FotaOrchestrator(FotaCloudProxy& cloud, InventoryProvider& inv,
                     ConsentProvider& consent, PackageDownloader& dl,
                     VehicleConditionProvider& cond, InstallExecutor& exec,
                     LogCollector& log, store::fota::FotaCloudStateStore& store,
                     FotaOrchestratorConfig cfg,
                     ClockSyncProvider* clockSync = nullptr);

    // 启动对账恢复：从 durable 状态恢复 VehicleTask/Execution/outbox 水位。
    void reconcileOnStart();

    // 单步推进：依据当前状态执行下一阶段动作并 durable 保存。
    StepOutcome step();

    // 注入控制指令（云端推送）。durable 保存后交给 executor。
    ControlOutcome applyControl(const ::vehicle::fota::v1::ControlCommand& cmd);

    // 策略同步。
    bool syncPolicy();

    // ---- EventSink ----
    // 先 durable 写 outbox，再分配严格递增 sequenceNo。Executor 不指定序号。
    bool emit(const ::vehicle::fota::v1::ExecutionEvent& evt) override;

    // ---- 状态查询 ----
    VehicleTaskState vehicleTaskState() const { return vtState_; }
    ExecutionState executionState() const { return exState_; }
    std::uint64_t acceptedSequenceNo() const { return acceptedSeq_; }
    std::uint64_t nextSequenceNo() const { return nextSeq_; }

private:
    FotaCloudProxy& cloud_;
    InventoryProvider& inv_;
    ConsentProvider& consent_;
    PackageDownloader& dl_;
    VehicleConditionProvider& cond_;
    InstallExecutor& exec_;
    LogCollector& log_;
    store::fota::FotaCloudStateStore& store_;
    FotaOrchestratorConfig cfg_;
    ClockSyncProvider* clockSync_ = nullptr;  // 可为空 = 不启用时钟门禁

    VehicleTaskState vtState_ = VehicleTaskState::None;
    ExecutionState exState_ = ExecutionState::PermitPersisted;
    std::uint64_t nextSeq_ = 1;
    std::uint64_t acceptedSeq_ = 0;
    std::uint64_t ackSeq_ = 1;

    // 内存缓存（恢复时从 store 加载）
    ::vehicle::fota::v1::VehicleTaskSnapshot frozenTask_;
    ::vehicle::fota::v1::InstallPermitResponse permit_;
    std::string executionId_;
    std::uint32_t attemptNo_ = 0;
    std::uint64_t lastReceivedControlRev_ = 0;
    std::uint64_t lastAppliedControlRev_ = 0;
    std::vector<std::string> pendingControlIds_;

    CallContext makeCtx(const std::string& idempotencyKey) const;
    std::int64_t nowMs() const;

    // 时钟门禁：未就绪时填充 outcome（action=awaitClockSync）并返回 false。
    bool clockReady(StepOutcome& outcome);

    // 阶段动作
    StepOutcome doCheckTask();
    StepOutcome doConsent();
    StepOutcome doDownload();
    StepOutcome doPermit();
    StepOutcome doExecute();
    StepOutcome doFinalize();
    StepOutcome doLogUpload();

    // 持久化辅助
    void persistVehicleTask(const char* action);
    void persistExecution();
    void loadFromStore();
};

} // namespace ota
} // namespace ccu_fota
