#include "snapshot_assembler.h"
#include "ccu/fota/store/fota_state_store.hpp"
#include "error_codes.h"
#include "fota_log_adapter.h"
#include "fota_log_context.h"
#include "constants.h"
#include <chrono>

namespace ccu_fota {

SnapshotAssembler::SnapshotAssembler(std::shared_ptr<someip::DiagInventoryClient> client)
    : client_(client)
    , current_seq_(1)
    , throttle_interval_ms_(5000) // Default 5 seconds
    , max_ecu_count_(100) // Default 100 ECUs
    , last_report_time_(std::chrono::steady_clock::now() - std::chrono::hours(1)) // Allow immediate first report
{
}

bool SnapshotAssembler::assembleSnapshot(VehicleSoftwareSnapshot& snapshot) {
    return assembleSnapshot(snapshot, SeqPolicy::Allocate);
}

bool SnapshotAssembler::assembleSnapshot(VehicleSoftwareSnapshot& snapshot,
                                         SeqPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto start_time = std::chrono::steady_clock::now();

    // Check throttling
    if (isThrottled()) {
        FotaLogAdapter::snapshot_assembler().warn(
            "fota.snapshot.throttled",
            "Reporting throttled, skipping"
        );
        return false;
    }

    // Create child scope for DIAG call (Service 0x1110)
    auto diag_scope = make_someip_child_scope(
        hex_id(DEFAULT_DIAG_SERVICE_ID),
        hex_id(METHOD_COLLECT_VEHICLE_INVENTORY)
    );

    // Collect vehicle inventory from CCU-DIAG (VIN comes from DIAG)
    // CCU-FOTA-DSN-CR-007: framework retry=None；业务重试由 orchestrator 执行。
    // 每次重试产生新 framework request/session，保持同一 trace/report 上下文。
    bool collected = false;
    uint32_t max_attempts = use_diag_retry_ ? (diag_retry_max_attempts_ + 1) : 1;
    for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
        if (client_->collectVehicleInventory(snapshot)) {
            collected = true;
            break;
        }
        if (attempt + 1 < max_attempts) {
            FotaLogAdapter::snapshot_assembler().warn(
                "fota.diag.retry",
                "Retrying DIAG collection",
                {flog::f_str("report_id", current_report_id()),
                 flog::f_int("attempt", static_cast<int64_t>(attempt + 2)),
                 flog::f_int("max_attempts", static_cast<int64_t>(max_attempts)),
                 flog::f_int("retry_interval_ms", static_cast<int64_t>(diag_retry_backoff_ms_))});
            std::this_thread::sleep_for(std::chrono::milliseconds(diag_retry_backoff_ms_));
        }
    }
    if (!collected) {
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        FotaLogAdapter::snapshot_assembler().error(
            fota_events::DIAG_COLLECT_FAILED,
            "Failed to collect vehicle inventory from DIAG",
            {flog::f_str("report_id", current_report_id()),
             flog::f_str("someip_service_id", hex_id(DEFAULT_DIAG_SERVICE_ID)),
             flog::f_str("someip_method_id", hex_id(METHOD_COLLECT_VEHICLE_INVENTORY)),
             flog::f_int("duration_ms", duration_ms),
             flog::f_str("error_code", "CCU-FOTA-1006")}
        );
        return false;
    }

    auto collect_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    FotaLogAdapter::snapshot_assembler().info(
        fota_events::DIAG_COLLECT_SUCCEEDED,
        "DIAG returned version inventory",
        {flog::f_str("report_id", current_report_id()),
         flog::f_str("registry_version", snapshot.registry_version),
         flog::f_str("overall_result", collectionStatusToString(snapshot.overall_result)),
         flog::f_int("duration_ms", collect_duration_ms)}
    );

    // Validate snapshot
    if (!validateSnapshot(snapshot)) {
        FotaLogAdapter::snapshot_assembler().error(
            fota_events::SNAPSHOT_ASSEMBLE_FAILED,
            "Snapshot validation failed",
            {flog::f_str("report_id", current_report_id()),
             flog::f_str("error_code", "CCU-FOTA-1003")}
        );
        return false;
    }

    // Update sequence number: durable allocation if store present (CCU-FOTA-DSN-CR-005)
    // CCU-FOTA-DSN-CR-012 D2：Defer 路径不得在采集阶段分配序号（分配点只在上报
    // 决策点，由 InventoryIdentityPlanner 统一执行）。
    if (policy == SeqPolicy::Defer) {
        snapshot.snapshot_seq = 0;
    } else if (state_store_) {
        try {
            snapshot.snapshot_seq = state_store_->allocateSnapshotSeq();
        } catch (const ccu_fota::store::SeqAllocBlocked&) {
            FotaLogAdapter::snapshot_assembler().error(
                fota_events::SNAPSHOT_ASSEMBLE_FAILED,
                "Snapshot assembly blocked: sequence allocation failed",
                {flog::f_str("report_id", current_report_id()),
                 flog::f_str("error_code", "STATE_BLOCKED")}
            );
            return false;
        }
    } else {
        snapshot.snapshot_seq = current_seq_++;
    }

    // Update last report time
    updateLastReportTime();

    FotaLogAdapter::snapshot_assembler().info(
        fota_events::SNAPSHOT_ASSEMBLED,
        "Snapshot assembled successfully",
        {flog::f_str("report_id", current_report_id()),
         flog::f_int("snapshot_seq", static_cast<int64_t>(snapshot.snapshot_seq)),
         flog::f_str("registry_version", snapshot.registry_version),
         flog::f_str("overall_result", collectionStatusToString(snapshot.overall_result)),
         // Payload: only record count, not full ECU list
         flog::f_int("ecu_count", static_cast<int64_t>(snapshot.ecu_list.size()))}
    );

    return true;
}

void SnapshotAssembler::setThrottleInterval(uint32_t interval_ms) {
    throttle_interval_ms_ = interval_ms;
}

void SnapshotAssembler::setMaxEcuCount(uint32_t max_count) {
    max_ecu_count_ = max_count;
}

void SnapshotAssembler::setStateStore(std::shared_ptr<store::FotaStateStore> store) {
    state_store_ = std::move(store);
}

void SnapshotAssembler::setDiagRetryPolicy(uint32_t max_attempts, uint32_t backoff_ms) {
    diag_retry_max_attempts_ = max_attempts;
    diag_retry_backoff_ms_ = backoff_ms;
    use_diag_retry_ = true;
}

bool SnapshotAssembler::isThrottled() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report_time_);
    return elapsed.count() < throttle_interval_ms_;
}

void SnapshotAssembler::updateLastReportTime() {
    last_report_time_ = std::chrono::steady_clock::now();
}

bool SnapshotAssembler::validateSnapshot(const VehicleSoftwareSnapshot& snapshot) {
    // Check VIN is not empty
    if (snapshot.vin.empty()) {
        return false;
    }

    // Check ECU count doesn't exceed maximum
    if (snapshot.ecu_list.size() > max_ecu_count_) {
        return false;
    }

    // Check overall result is valid
    if (snapshot.overall_result != CollectionStatus::ALL_OK &&
        snapshot.overall_result != CollectionStatus::PARTIAL &&
        snapshot.overall_result != CollectionStatus::FAILED) {
        return false;
    }

    return true;
}

} // namespace ccu_fota
