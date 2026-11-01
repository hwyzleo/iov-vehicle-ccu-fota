#pragma once

#include "ccu/fota/someip/diag_inventory_client.hpp"
#include "data_models.h"

namespace ccu_fota {
namespace test {

// CCU-FOTA-DSN-CR-007: 测试子类继承 framework-backed 适配器，override 业务方法
// 以避免真实 SOME/IP 调用（适配器的 protected 默认构造不持有 framework Client）。

// Testable DIAG 采集 Client：override collectVehicleInventory 直接返回测试快照。
class TestableDiagInventoryClient : public someip::DiagInventoryClient {
public:
    TestableDiagInventoryClient() : DiagInventoryClient() {}

    bool collectVehicleInventory(VehicleSoftwareSnapshot& snapshot) override {
        if (test_vin_.empty()) {
            return false;
        }
        snapshot.vin = test_vin_;
        snapshot.baseline_id = "BASELINE001";
        snapshot.baseline_source = BaselineSource::FACTORY;
        snapshot.registry_version = "1.0.0";
        snapshot.collected_at = "2026-07-21T10:00:00Z";
        snapshot.overall_result = CollectionStatus::ALL_OK;
        snapshot.snapshot_seq = 1;

        EcuVersionEntry entry1;
        entry1.ecu_id = "ECU001";
        entry1.part_number = "PN001";
        entry1.sw_version = "1.0.0";
        entry1.hw_version = "HW1.0";
        entry1.source = VersionSource::UDS_0x22;
        entry1.status = EcuStatus::OK;
        snapshot.ecu_list = {entry1};
        return true;
    }

    bool getVin(std::string& vin) override {
        if (test_vin_.empty()) return false;
        vin = test_vin_;
        return true;
    }

    void setTestVin(const std::string& vin) { test_vin_ = vin; }

private:
    std::string test_vin_ = "12345678901234567";
};

} // namespace test
} // namespace ccu_fota
