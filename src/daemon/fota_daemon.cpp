// =============================================================================
// src/daemon/fota_daemon.cpp
// CCU-FOTA 契约 B daemon 装配实现 (CCU-FOTA-DSN-CR-010/011)
// =============================================================================

#include "ccu/fota/daemon/fota_daemon.hpp"

#include "ccu/fota/ota/payload_types.hpp"

#include "vehicle/fota/v1/types.pb.h"

namespace ccu_fota {
namespace daemon {

using namespace ccu_fota::ota;

FotaDaemon::FotaDaemon(ccu::fw::someip::SomeIpRuntime& runtime,
                       const ccu_fota::someip::TboxGenericTransportAddress& address,
                       SomeIpTransportConfig transportCfg,
                       FotaOrchestratorConfig orchCfg,
                       bool businessTrigger,
                       std::shared_ptr<InventoryProvider> inv,
                       std::shared_ptr<ConsentProvider> consent,
                       std::shared_ptr<PackageDownloader> dl,
                       std::shared_ptr<VehicleConditionProvider> cond,
                       std::shared_ptr<InstallExecutor> exec,
                       std::shared_ptr<LogCollector> log,
                       std::shared_ptr<store::fota::FotaCloudStateStore> cloudStore)
    : runtime_(runtime)
    , transportCfg_(std::move(transportCfg))
    , orchCfg_(std::move(orchCfg))
    , businessTrigger_(businessTrigger)
    , inv_(std::move(inv))
    , consent_(std::move(consent))
    , dl_(std::move(dl))
    , cond_(std::move(cond))
    , exec_(std::move(exec))
    , log_(std::move(log))
    , cloudStore_(std::move(cloudStore)) {
    if (!inv_ || !consent_ || !dl_ || !cond_ || !exec_ || !log_ || !cloudStore_) {
        throw std::invalid_argument("FotaDaemon: ports/store must be non-null");
    }
    // 复用进程唯一 Runtime：只在其上 createClient（不创建第二个 Runtime/application）。
    ccu::fw::someip::ServiceKey key{address.service, address.instance};
    ccu::fw::someip::InterfaceVersion ifaceV{address.interfaceVersion.major,
                                             address.interfaceVersion.minor};
    auto client = runtime_.createClient(key, ifaceV);

    transport_ = std::make_unique<SomeIpVehicleMessageTransport>(
        std::move(client), address, transportCfg_);
    cloud_ = std::make_unique<FotaCloudProxyViaTransport>(*transport_);
    orch_ = std::make_unique<FotaOrchestrator>(
        *cloud_, *inv_, *consent_, *dl_, *cond_, *exec_, *log_, *cloudStore_, orchCfg_);
}

FotaDaemon::~FotaDaemon() { stop(); }

std::unique_ptr<FotaDaemon> FotaDaemon::create(
    ccu::fw::someip::SomeIpRuntime& runtime,
    const ccu_fota::someip::TboxGenericTransportAddress& address,
    SomeIpTransportConfig transportCfg,
    FotaOrchestratorConfig orchCfg,
    bool businessTrigger,
    std::shared_ptr<InventoryProvider> inv,
    std::shared_ptr<ConsentProvider> consent,
    std::shared_ptr<PackageDownloader> dl,
    std::shared_ptr<VehicleConditionProvider> cond,
    std::shared_ptr<InstallExecutor> exec,
    std::shared_ptr<LogCollector> log,
    std::shared_ptr<store::fota::FotaCloudStateStore> cloudStore) {
    return std::unique_ptr<FotaDaemon>(new FotaDaemon(
        runtime, address, std::move(transportCfg), std::move(orchCfg),
        businessTrigger, std::move(inv), std::move(consent), std::move(dl),
        std::move(cond), std::move(exec), std::move(log), std::move(cloudStore)));
}

void FotaDaemon::start() {
    if (running_.exchange(true)) return;
    stopping_.store(false);

    // 1. 开放 SOME/IP 通用传输（requestService + 下行订阅 + 独立 executor）。
    transport_->start();

    // 2. 下行订阅：service=vehicle.fota -> 解码 ControlCommand -> applyControl。
    //    回调在 transport 独立 executor 线程执行，不在 SOME/IP I/O 线程推进状态机。
    downlinkSub_ = cloud_->subscribeDownlink(
        payload_type::kService,
        [this](VehicleMessage&& msg) {
            auto decoded = cloud_->decodeDownlink<::vehicle::fota::v1::ControlCommand>(
                payload_type::kControlCommand, msg);
            if (decoded) {
                orch_->applyControl(*decoded);
            }
        });

    // 3. reconcile / 恢复动作（durable 状态恢复）。
    if (orchCfg_.reconcileOnStart) orch_->reconcileOnStart();

    // 4. 初始推进。
    orch_->step();

    // 5. 开放业务触发（定时 step）。
    if (businessTrigger_) {
        std::lock_guard<std::mutex> lock(timerMutex_);
        if (timerThread_.joinable()) timerThread_.join();
        timerThread_ = std::thread([this] { timerLoop(); });
    }
}

void FotaDaemon::stop() {
    if (!running_.exchange(false) && stopping_.load()) return;
    stopping_.store(true);

    // 1. 拒绝新调用（transport Stopping；orchestrator 不再被触发）。
    // 2. 停止 timer。
    {
        std::lock_guard<std::mutex> lock(timerMutex_);
    }
    timerCv_.notify_all();
    if (timerThread_.joinable()) timerThread_.join();

    // 3. 保存业务状态（orchestrator 每次 step/applyControl 已 durable；显式 flush）。
    cloudStore_->flush();

    // 4. 取消下行订阅。
    downlinkSub_.cancel();

    // 5. 收敛 in-flight 并销毁 transport（内部：拒绝新调用 -> 取消下行 -> 收敛 -> release）。
    transport_->stop();

    running_.store(false);
}

void FotaDaemon::timerLoop() {
    const auto interval = orchCfg_.taskCheckInterval;
    std::unique_lock<std::mutex> lock(timerMutex_);
    while (!stopping_.load()) {
        // 无锁等待，被 stop() 唤醒后退出。
        timerCv_.wait_for(lock, interval, [this] { return stopping_.load(); });
        if (stopping_.load()) break;
        lock.unlock();
        orch_->step();   // 分步推进；失败保留 durable 状态，下次重试
        lock.lock();
    }
}

ota::SomeIpVehicleMessageTransport& FotaDaemon::transport() const {
    return *transport_;
}
ota::FotaCloudProxyViaTransport& FotaDaemon::cloud() const { return *cloud_; }
ota::FotaOrchestrator& FotaDaemon::orchestrator() const { return *orch_; }

} // namespace daemon
} // namespace ccu_fota
