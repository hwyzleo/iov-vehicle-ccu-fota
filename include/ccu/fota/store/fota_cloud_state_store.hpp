#pragma once

// =============================================================================
// include/ccu/fota/store/fota_cloud_state_store.hpp
// CCU-FOTA 车云 FOTA 状态存储封装 (CCU-FOTA-DSN-CR-011 §Store)
// =============================================================================
// 包装 ccu-framework-store，为车云 FOTA key 提供类型化 load/save/remove。
// 与 FotaStateStore 共享同一 service "fota" 存储（通过 underlyingStore()），
// 避免重复 open 造成锁冲突。同步 durable 写入；未知新版本 fail-closed。
//
// 多条目类别（downloads/event_outbox/controls/log_jobs）按 item 拆为独立 key
// （框架 Store 禁止 '/'，用 ':' 分隔）。每 item 独立原子写；跨 key 一致性由
// 严格写入顺序、稳定 taskRevision/executionId/sequenceNo/controlRevision/
// idempotencyKey 与启动对账保证。
//
// 启动时（开放 transport 与触发任务前）应先执行 FotaStateMigrator::migrate()
// 完成旧 ota.* -> fota.* 迁移（见 fota_state_migration.hpp）。
// =============================================================================

#include "ccu/fota/store/fota_cloud_state.hpp"
#include "ccu/fota/store/fota_cloud_state_serializer.hpp"

#include "store.h"          // ccu::fw::store::Store
#include "store_types.h"    // StoreException

#include <cstdint>
#include <optional>
#include <string>

namespace ccu_fota {
namespace store {
namespace fota {

class FotaCloudStateStore {
public:
    // 从已打开的 framework Store 构造（与 FotaStateStore 共享同一 service 存储）。
    explicit FotaCloudStateStore(ccu::fw::store::Store store);

    // ---- fota.vehicle_task ----
    std::optional<FotaVehicleTaskRecord> loadVehicleTask();
    void saveVehicleTask(const FotaVehicleTaskRecord& r);
    void removeVehicleTask();

    // ---- fota.inventory ----
    std::optional<FotaInventoryRecord> loadInventory();
    void saveInventory(const FotaInventoryRecord& r);
    void removeInventory();

    // ---- fota.consent ----
    std::optional<FotaConsentRecord> loadConsent();
    void saveConsent(const FotaConsentRecord& r);
    void removeConsent();

    // ---- fota.downloads:<package_id> ----
    std::optional<FotaDownloadRecord> loadDownload(const std::string& packageId);
    void saveDownload(const FotaDownloadRecord& r);
    void removeDownload(const std::string& packageId);

    // ---- fota.execution ----
    std::optional<FotaExecutionRecord> loadExecution();
    void saveExecution(const FotaExecutionRecord& r);
    void removeExecution();

    // ---- fota.event_outbox_meta ----
    std::optional<FotaEventOutboxMeta> loadEventOutboxMeta();
    void saveEventOutboxMeta(const FotaEventOutboxMeta& r);

    // ---- fota.event_outbox:<seq> ----
    std::optional<FotaEventOutboxRecord> loadEvent(std::uint64_t sequenceNo);
    void saveEvent(const FotaEventOutboxRecord& r);
    void removeEvent(std::uint64_t sequenceNo);

    // ---- fota.controls:<revision> ----
    std::optional<FotaControlRecord> loadControl(std::uint64_t revision);
    void saveControl(const FotaControlRecord& r);
    void removeControl(std::uint64_t revision);

    // ---- fota.policy ----
    std::optional<FotaPolicyRecord> loadPolicy();
    void savePolicy(const FotaPolicyRecord& r);
    void removePolicy();

    // ---- fota.log_jobs:<log_request_id> ----
    std::optional<FotaLogJobRecord> loadLogJob(const std::string& logRequestId);
    void saveLogJob(const FotaLogJobRecord& r);
    void removeLogJob(const std::string& logRequestId);

    // 同步 flush（no-op for Synchronous）。
    void flush() { store_.flush(); }

private:
    ccu::fw::store::Store store_;
};

} // namespace fota
} // namespace store
} // namespace ccu_fota
