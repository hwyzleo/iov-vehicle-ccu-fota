#include "snapshot_assembler.h"
#include "ccu/fota/someip/diag_inventory_client.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace ccu_fota;
using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;

class MockDiagInventoryClient : public someip::DiagInventoryClient {
public:
    MockDiagInventoryClient() : DiagInventoryClient() {}
    MOCK_METHOD(bool, collectVehicleInventory, (VehicleSoftwareSnapshot& snapshot));
    MOCK_METHOD(bool, getVin, (std::string& vin));
};

class SnapshotAssemblerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup test environment
    }

    void TearDown() override {
        // Clean up
    }
};

TEST_F(SnapshotAssemblerTest, AssembleSnapshot) {
    auto mock_client = std::make_shared<MockDiagInventoryClient>();

    VehicleSoftwareSnapshot expected_snapshot;
    expected_snapshot.vin = "12345678901234567";
    expected_snapshot.overall_result = CollectionStatus::ALL_OK;

    EXPECT_CALL(*mock_client, collectVehicleInventory(_))
        .WillOnce(Invoke([&expected_snapshot](VehicleSoftwareSnapshot& snapshot) {
            snapshot = expected_snapshot;
            return true;
        }));

    SnapshotAssembler assembler(mock_client);
    VehicleSoftwareSnapshot snapshot;
    bool result = assembler.assembleSnapshot(snapshot);

    EXPECT_TRUE(result);
    EXPECT_EQ(snapshot.vin, "12345678901234567");
    EXPECT_EQ(snapshot.snapshot_seq, 1);
}

TEST_F(SnapshotAssemblerTest, AssembleSnapshotWithSeqIncrement) {
    auto mock_client = std::make_shared<MockDiagInventoryClient>();

    VehicleSoftwareSnapshot snapshot1;
    snapshot1.vin = "12345678901234567";
    snapshot1.overall_result = CollectionStatus::ALL_OK;

    VehicleSoftwareSnapshot snapshot2;
    snapshot2.vin = "12345678901234567";
    snapshot2.overall_result = CollectionStatus::ALL_OK;

    EXPECT_CALL(*mock_client, collectVehicleInventory(_))
        .WillOnce(Invoke([&snapshot1](VehicleSoftwareSnapshot& snapshot) {
            snapshot = snapshot1;
            return true;
        }))
        .WillOnce(Invoke([&snapshot2](VehicleSoftwareSnapshot& snapshot) {
            snapshot = snapshot2;
            return true;
        }));

    SnapshotAssembler assembler(mock_client);
    assembler.setThrottleInterval(0); // Disable throttle for this test

    VehicleSoftwareSnapshot result1;
    assembler.assembleSnapshot(result1);

    VehicleSoftwareSnapshot result2;
    assembler.assembleSnapshot(result2);

    EXPECT_EQ(result1.snapshot_seq, 1);
    EXPECT_EQ(result2.snapshot_seq, 2);
}

// CCU-FOTA-DSN-CR-012 D2：Defer 策略下采集阶段不得分配序号。
TEST_F(SnapshotAssemblerTest, DeferPolicyDoesNotAllocateSequence) {
    auto mock_client = std::make_shared<MockDiagInventoryClient>();

    VehicleSoftwareSnapshot expected;
    expected.vin = "12345678901234567";
    expected.overall_result = CollectionStatus::ALL_OK;

    EXPECT_CALL(*mock_client, collectVehicleInventory(_))
        .Times(3)
        .WillRepeatedly(Invoke([&expected](VehicleSoftwareSnapshot& s) {
            s = expected;
            return true;
        }));

    SnapshotAssembler assembler(mock_client);
    assembler.setThrottleInterval(0);
    for (int i = 0; i < 3; ++i) {
        VehicleSoftwareSnapshot snap;
        ASSERT_TRUE(assembler.assembleSnapshot(snap, SeqPolicy::Defer));
        EXPECT_EQ(snap.snapshot_seq, 0u) << "iteration " << i;
    }

    // Allocate 策略行为保持不变（兼容旧启动自动上报路径）。
    EXPECT_CALL(*mock_client, collectVehicleInventory(_))
        .WillOnce(Invoke([&expected](VehicleSoftwareSnapshot& s) {
            s = expected;
            return true;
        }));
    VehicleSoftwareSnapshot alloc;
    ASSERT_TRUE(assembler.assembleSnapshot(alloc, SeqPolicy::Allocate));
    EXPECT_EQ(alloc.snapshot_seq, 1u);
}

TEST_F(SnapshotAssemblerTest, AssembleSnapshotFailure) {
    auto mock_client = std::make_shared<MockDiagInventoryClient>();

    EXPECT_CALL(*mock_client, collectVehicleInventory(_))
        .WillOnce(Return(false));

    SnapshotAssembler assembler(mock_client);
    VehicleSoftwareSnapshot snapshot;
    bool result = assembler.assembleSnapshot(snapshot);

    EXPECT_FALSE(result);
}

TEST_F(SnapshotAssemblerTest, ThrottleReporting) {
    auto mock_client = std::make_shared<MockDiagInventoryClient>();

    VehicleSoftwareSnapshot snapshot;
    snapshot.vin = "12345678901234567";
    snapshot.overall_result = CollectionStatus::ALL_OK;

    EXPECT_CALL(*mock_client, collectVehicleInventory(_))
        .WillOnce(Invoke([&snapshot](VehicleSoftwareSnapshot& s) {
            s = snapshot;
            return true;
        }));

    SnapshotAssembler assembler(mock_client);
    assembler.setThrottleInterval(100); // 100ms throttle

    VehicleSoftwareSnapshot result1;
    assembler.assembleSnapshot(result1);

    // Second call should be throttled
    VehicleSoftwareSnapshot result2;
    bool second_result = assembler.assembleSnapshot(result2);

    EXPECT_FALSE(second_result); // Should be throttled
}
