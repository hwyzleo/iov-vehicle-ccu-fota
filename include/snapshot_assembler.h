#pragma once

#include "data_models.h"
#include "ccu/fota/someip/diag_inventory_client.hpp"
#include <memory>
#include <chrono>
#include <mutex>
#include <thread>

namespace ccu_fota {

namespace store { class FotaStateStore; }  // CCU-FOTA-DSN-CR-005

// 序号分配策略 (CCU-FOTA-DSN-CR-012 D2)。
//   Allocate: 旧启动自动上报路径，采集成功后立即 durable 分配 snapshotSeq。
//   Defer:    车云任务检测路径，只产出快照内容；序号只在上报决策点由
//             InventoryIdentityPlanner 分配（采集阶段严禁分配）。
enum class SeqPolicy { Allocate, Defer };

class SnapshotAssembler {
public:
    // CCU-FOTA-DSN-CR-007: DIAG 采集 Client 改为 framework-backed 适配器。
    SnapshotAssembler(std::shared_ptr<someip::DiagInventoryClient> client);
    virtual ~SnapshotAssembler() = default;

    // 兼容旧调用：等价于 assembleSnapshot(snapshot, SeqPolicy::Allocate)。
    virtual bool assembleSnapshot(VehicleSoftwareSnapshot& snapshot);

    // 按显式序号策略组装快照。Defer 时不分配 snapshotSeq（保持 0），
    // 供车云任务检测路径复用采集结果而不消耗版本序号。
    virtual bool assembleSnapshot(VehicleSoftwareSnapshot& snapshot, SeqPolicy policy);

    void setThrottleInterval(uint32_t interval_ms);
    void setMaxEcuCount(uint32_t max_count);

    // 注入状态存储以启用 durable 序号分配 (CCU-FOTA-DSN-CR-005)。
    // 未注入时回退到内存序号（仅用于测试/mock）。
    void setStateStore(std::shared_ptr<store::FotaStateStore> store);

    // CCU-FOTA-DSN-CR-007: DIAG 业务重试由 orchestrator 统一执行（framework
    // retry=None）。配置 fota.diag.retry_max_attempts / retry_backoff_ms。
    void setDiagRetryPolicy(uint32_t max_attempts, uint32_t backoff_ms);

private:
    std::shared_ptr<someip::DiagInventoryClient> client_;
    std::shared_ptr<store::FotaStateStore> state_store_;
    uint64_t current_seq_;
    uint32_t throttle_interval_ms_;
    uint32_t max_ecu_count_;
    std::chrono::steady_clock::time_point last_report_time_;
    std::mutex mutex_;

    // DIAG 业务重试 (CR-007)
    uint32_t diag_retry_max_attempts_{0};
    uint32_t diag_retry_backoff_ms_{0};
    bool use_diag_retry_{false};

    bool isThrottled();
    void updateLastReportTime();
    bool validateSnapshot(const VehicleSoftwareSnapshot& snapshot);
};

} // namespace ccu_fota
