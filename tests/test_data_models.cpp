#include "data_models.h"
#include <gtest/gtest.h>

using namespace ccu_fota;

TEST(DataModelsTest, EcuVersionEntryCreation) {
    EcuVersionEntry entry;
    entry.ecu_id = "ECU001";
    entry.part_number = "PN123";
    entry.sw_version = "1.0.0";
    entry.hw_version = "HW1.0";
    entry.source = VersionSource::UDS_0x22;
    entry.status = EcuStatus::OK;

    EXPECT_EQ(entry.ecu_id, "ECU001");
    EXPECT_EQ(entry.part_number.value(), "PN123");
    EXPECT_EQ(entry.sw_version.value(), "1.0.0");
    EXPECT_EQ(entry.hw_version.value(), "HW1.0");
    EXPECT_EQ(entry.source, VersionSource::UDS_0x22);
    EXPECT_EQ(entry.status, EcuStatus::OK);
}

TEST(DataModelsTest, VehicleSoftwareSnapshotCreation) {
    VehicleSoftwareSnapshot snapshot;
    snapshot.vin = "12345678901234567";
    snapshot.baseline_id = "BASELINE001";
    snapshot.baseline_source = BaselineSource::FACTORY;
    snapshot.registry_version = "1.0.0";
    snapshot.collected_at = "2026-07-21T10:00:00Z";
    snapshot.overall_result = CollectionStatus::ALL_OK;
    snapshot.snapshot_seq = 1;

    EcuVersionEntry entry1;
    entry1.ecu_id = "ECU001";
    entry1.status = EcuStatus::OK;

    EcuVersionEntry entry2;
    entry2.ecu_id = "ECU002";
    entry2.status = EcuStatus::OK;

    snapshot.ecu_list = {entry1, entry2};

    EXPECT_EQ(snapshot.vin, "12345678901234567");
    EXPECT_EQ(snapshot.baseline_id.value(), "BASELINE001");
    EXPECT_EQ(snapshot.baseline_source, BaselineSource::FACTORY);
    EXPECT_EQ(snapshot.registry_version, "1.0.0");
    EXPECT_EQ(snapshot.overall_result, CollectionStatus::ALL_OK);
    EXPECT_EQ(snapshot.snapshot_seq, 1);
    EXPECT_EQ(snapshot.ecu_list.size(), 2);
}

TEST(DataModelsTest, SnapshotSeqIncrement) {
    VehicleSoftwareSnapshot snapshot;
    snapshot.snapshot_seq = 1;

    // Simulate increment
    snapshot.snapshot_seq++;

    EXPECT_EQ(snapshot.snapshot_seq, 2);
}
