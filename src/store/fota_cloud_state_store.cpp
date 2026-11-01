// =============================================================================
// src/store/fota_cloud_state_store.cpp
// CCU-FOTA 车云 FOTA 状态存储封装实现 (CCU-FOTA-DSN-CR-011 §Store)
// =============================================================================

#include "ccu/fota/store/fota_cloud_state_store.hpp"

#include <utility>

namespace ccu_fota {
namespace store {
namespace fota {

FotaCloudStateStore::FotaCloudStateStore(ccu::fw::store::Store store)
    : store_(std::move(store)) {}

// ---- fota.vehicle_task ----
std::optional<FotaVehicleTaskRecord> FotaCloudStateStore::loadVehicleTask() {
    if (!store_.has(keys::VEHICLE_TASK)) return std::nullopt;
    return decodeVehicleTask(store_.load<std::string>(keys::VEHICLE_TASK));
}
void FotaCloudStateStore::saveVehicleTask(const FotaVehicleTaskRecord& r) {
    store_.save<std::string>(keys::VEHICLE_TASK, encodeVehicleTask(r));
}
void FotaCloudStateStore::removeVehicleTask() {
    store_.remove(keys::VEHICLE_TASK);
}

// ---- fota.inventory ----
std::optional<FotaInventoryRecord> FotaCloudStateStore::loadInventory() {
    if (!store_.has(keys::INVENTORY)) return std::nullopt;
    return decodeInventory(store_.load<std::string>(keys::INVENTORY));
}
void FotaCloudStateStore::saveInventory(const FotaInventoryRecord& r) {
    store_.save<std::string>(keys::INVENTORY, encodeInventory(r));
}
void FotaCloudStateStore::removeInventory() {
    store_.remove(keys::INVENTORY);
}

// ---- fota.consent ----
std::optional<FotaConsentRecord> FotaCloudStateStore::loadConsent() {
    if (!store_.has(keys::CONSENT)) return std::nullopt;
    return decodeConsent(store_.load<std::string>(keys::CONSENT));
}
void FotaCloudStateStore::saveConsent(const FotaConsentRecord& r) {
    store_.save<std::string>(keys::CONSENT, encodeConsent(r));
}
void FotaCloudStateStore::removeConsent() {
    store_.remove(keys::CONSENT);
}

// ---- fota.downloads:<package_id> ----
std::optional<FotaDownloadRecord> FotaCloudStateStore::loadDownload(const std::string& packageId) {
    const auto key = keys::downloadKey(packageId);
    if (!store_.has(key)) return std::nullopt;
    return decodeDownload(store_.load<std::string>(key));
}
void FotaCloudStateStore::saveDownload(const FotaDownloadRecord& r) {
    store_.save<std::string>(keys::downloadKey(r.packageId), encodeDownload(r));
}
void FotaCloudStateStore::removeDownload(const std::string& packageId) {
    store_.remove(keys::downloadKey(packageId));
}

// ---- fota.execution ----
std::optional<FotaExecutionRecord> FotaCloudStateStore::loadExecution() {
    if (!store_.has(keys::EXECUTION)) return std::nullopt;
    return decodeExecution(store_.load<std::string>(keys::EXECUTION));
}
void FotaCloudStateStore::saveExecution(const FotaExecutionRecord& r) {
    store_.save<std::string>(keys::EXECUTION, encodeExecution(r));
}
void FotaCloudStateStore::removeExecution() {
    store_.remove(keys::EXECUTION);
}

// ---- fota.event_outbox_meta ----
std::optional<FotaEventOutboxMeta> FotaCloudStateStore::loadEventOutboxMeta() {
    if (!store_.has(keys::EVENT_OUTBOX_META)) return std::nullopt;
    return decodeEventOutboxMeta(store_.load<std::string>(keys::EVENT_OUTBOX_META));
}
void FotaCloudStateStore::saveEventOutboxMeta(const FotaEventOutboxMeta& r) {
    store_.save<std::string>(keys::EVENT_OUTBOX_META, encodeEventOutboxMeta(r));
}

// ---- fota.event_outbox:<seq> ----
std::optional<FotaEventOutboxRecord> FotaCloudStateStore::loadEvent(std::uint64_t sequenceNo) {
    const auto key = keys::eventKey(sequenceNo);
    if (!store_.has(key)) return std::nullopt;
    return decodeEventOutbox(store_.load<std::string>(key));
}
void FotaCloudStateStore::saveEvent(const FotaEventOutboxRecord& r) {
    store_.save<std::string>(keys::eventKey(r.sequenceNo), encodeEventOutbox(r));
}
void FotaCloudStateStore::removeEvent(std::uint64_t sequenceNo) {
    store_.remove(keys::eventKey(sequenceNo));
}

// ---- fota.controls:<revision> ----
std::optional<FotaControlRecord> FotaCloudStateStore::loadControl(std::uint64_t revision) {
    const auto key = keys::controlKey(revision);
    if (!store_.has(key)) return std::nullopt;
    return decodeControl(store_.load<std::string>(key));
}
void FotaCloudStateStore::saveControl(const FotaControlRecord& r) {
    store_.save<std::string>(keys::controlKey(r.controlRevision), encodeControl(r));
}
void FotaCloudStateStore::removeControl(std::uint64_t revision) {
    store_.remove(keys::controlKey(revision));
}

// ---- fota.policy ----
std::optional<FotaPolicyRecord> FotaCloudStateStore::loadPolicy() {
    if (!store_.has(keys::POLICY)) return std::nullopt;
    return decodePolicy(store_.load<std::string>(keys::POLICY));
}
void FotaCloudStateStore::savePolicy(const FotaPolicyRecord& r) {
    store_.save<std::string>(keys::POLICY, encodePolicy(r));
}
void FotaCloudStateStore::removePolicy() {
    store_.remove(keys::POLICY);
}

// ---- fota.log_jobs:<log_request_id> ----
std::optional<FotaLogJobRecord> FotaCloudStateStore::loadLogJob(const std::string& logRequestId) {
    const auto key = keys::logJobKey(logRequestId);
    if (!store_.has(key)) return std::nullopt;
    return decodeLogJob(store_.load<std::string>(key));
}
void FotaCloudStateStore::saveLogJob(const FotaLogJobRecord& r) {
    store_.save<std::string>(keys::logJobKey(r.logRequestId), encodeLogJob(r));
}
void FotaCloudStateStore::removeLogJob(const std::string& logRequestId) {
    store_.remove(keys::logJobKey(logRequestId));
}

} // namespace fota
} // namespace store
} // namespace ccu_fota
