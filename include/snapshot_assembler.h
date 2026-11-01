#pragma once

#include "data_models.h"
#include "ccu/fota/someip/diag_inventory_client.hpp"
#include <memory>
#include <chrono>
#include <mutex>
#include <thread>

namespace ccu_fota {

namespace store { class FotaStateStore; }  // CCU-FOTA-DSN-CR-005

class SnapshotAssembler {
public:
    // CCU-FOTA-DSN-CR-007: DIAG 采集 Client 改为 framework-backed 适配器。
    SnapshotAssembler(std::shared_ptr<someip::DiagInventoryClient> client);
    virtual ~SnapshotAssembler() = default;

    virtual bool assembleSnapshot(VehicleSoftwareSnapshot& snapshot);

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
