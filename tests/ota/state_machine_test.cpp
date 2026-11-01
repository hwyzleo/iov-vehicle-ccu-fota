// =============================================================================
// tests/ota/state_machine_test.cpp
// CCU-FOTA VehicleTask / Execution 状态机单元测试 (CCU-FOTA-DSN-CR-009 §13.2 / CR-011)
// =============================================================================

#include "ccu/fota/ota/state/execution_state.hpp"
#include "ccu/fota/ota/state/state_proto.hpp"
#include "ccu/fota/ota/state/vehicle_task_state.hpp"

#include <gtest/gtest.h>

using namespace ccu_fota::ota;

// ---------------------------------------------------------------------------
// VehicleTask 状态机：文档转换路径
// ---------------------------------------------------------------------------
TEST(VehicleTaskStateMachine, HappyPathTransitions) {
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::None, VehicleTaskState::Discovered));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Discovered, VehicleTaskState::ConsentPending));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::ConsentPending, VehicleTaskState::DownloadPending));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::DownloadPending, VehicleTaskState::Downloading));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Downloading, VehicleTaskState::Ready));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Ready, VehicleTaskState::WaitingWindow));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::WaitingWindow, VehicleTaskState::PermitPending));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::PermitPending, VehicleTaskState::Executing));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Executing, VehicleTaskState::Completed));
}

TEST(VehicleTaskStateMachine, NoConsentSkipsConsentPending) {
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Discovered, VehicleTaskState::DownloadPending));
}

TEST(VehicleTaskStateMachine, RetryAndRollbackCycles) {
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Executing, VehicleTaskState::RetryPending));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::RetryPending, VehicleTaskState::PermitPending));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Executing, VehicleTaskState::RollbackPending));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::RollbackPending, VehicleTaskState::Executing));
}

TEST(VehicleTaskStateMachine, PauseFromSafePoints) {
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Discovered, VehicleTaskState::Paused));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Ready, VehicleTaskState::Paused));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Executing, VehicleTaskState::Paused));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Paused, VehicleTaskState::Discovered));
}

TEST(VehicleTaskStateMachine, ConsentRejectedEnds) {
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::ConsentPending, VehicleTaskState::Ended));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Paused, VehicleTaskState::Ended));
}

TEST(VehicleTaskStateMachine, GlobalCancelEscapeToEnded) {
    // 任意非终态可被 cancel/supersede/abort 终止。
    for (int i = 0; i <= static_cast<int>(VehicleTaskState::RollbackPending); ++i) {
        if (i == static_cast<int>(VehicleTaskState::Completed) ||
            i == static_cast<int>(VehicleTaskState::Ended)) continue;
        EXPECT_TRUE(isValidVehicleTaskTransition(static_cast<VehicleTaskState>(i),
                                                 VehicleTaskState::Ended))
            << "from state " << i;
    }
}

TEST(VehicleTaskStateMachine, IllegalTransitionsRejected) {
    EXPECT_FALSE(isValidVehicleTaskTransition(VehicleTaskState::None, VehicleTaskState::Executing));
    EXPECT_FALSE(isValidVehicleTaskTransition(VehicleTaskState::Downloading, VehicleTaskState::PermitPending));
    EXPECT_FALSE(isValidVehicleTaskTransition(VehicleTaskState::PermitPending, VehicleTaskState::Ready));
    EXPECT_FALSE(isValidVehicleTaskTransition(VehicleTaskState::Discovered, VehicleTaskState::Completed));
}

TEST(VehicleTaskStateMachine, TerminalStatesAreStuck) {
    ASSERT_TRUE(isTerminalVehicleTaskState(VehicleTaskState::Completed));
    ASSERT_TRUE(isTerminalVehicleTaskState(VehicleTaskState::Ended));
    EXPECT_FALSE(isValidVehicleTaskTransition(VehicleTaskState::Completed, VehicleTaskState::Discovered));
    EXPECT_FALSE(isValidVehicleTaskTransition(VehicleTaskState::Ended, VehicleTaskState::Discovered));
    EXPECT_FALSE(isValidVehicleTaskTransition(VehicleTaskState::Completed, VehicleTaskState::Ended));
}

TEST(VehicleTaskStateMachine, SelfTransitionIdempotent) {
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Executing, VehicleTaskState::Executing));
    EXPECT_TRUE(isValidVehicleTaskTransition(VehicleTaskState::Completed, VehicleTaskState::Completed));
}

TEST(VehicleTaskStateMachine, StringRoundTrip) {
    for (int i = 0; i <= static_cast<int>(VehicleTaskState::Ended); ++i) {
        auto s = static_cast<VehicleTaskState>(i);
        const char* name = vehicleTaskStateToString(s);
        VehicleTaskState back;
        ASSERT_TRUE(vehicleTaskStateFromString(name, back)) << name;
        EXPECT_EQ(back, s) << name;
    }
    VehicleTaskState dummy;
    EXPECT_FALSE(vehicleTaskStateFromString("BOGUS", dummy));
}

// ---------------------------------------------------------------------------
// Execution 状态机
// ---------------------------------------------------------------------------
TEST(ExecutionStateMachine, HappyPath) {
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::PermitPersisted, ExecutionState::Ready));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::Ready, ExecutionState::InstallStarted));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::InstallStarted, ExecutionState::Install));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::Install, ExecutionState::Reboot));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::Reboot, ExecutionState::PostCheck));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::PostCheck, ExecutionState::ResultPending));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::ResultPending, ExecutionState::Succeeded));
}

TEST(ExecutionStateMachine, RollbackBranch) {
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::Install, ExecutionState::Rollback));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::PostCheck, ExecutionState::Rollback));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::Rollback, ExecutionState::ResultPending));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::ResultPending, ExecutionState::RolledBack));
}

TEST(ExecutionStateMachine, FailedAndCanceledTerminals) {
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::ResultPending, ExecutionState::Failed));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::ResultPending, ExecutionState::Canceled));
    // 任意非终态可被 ABORT 取消。
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::Install, ExecutionState::Canceled));
    EXPECT_TRUE(isValidExecutionTransition(ExecutionState::Reboot, ExecutionState::Canceled));
}

TEST(ExecutionStateMachine, IllegalTransitionsRejected) {
    EXPECT_FALSE(isValidExecutionTransition(ExecutionState::Ready, ExecutionState::Install));
    EXPECT_FALSE(isValidExecutionTransition(ExecutionState::InstallStarted, ExecutionState::Reboot));
    EXPECT_FALSE(isValidExecutionTransition(ExecutionState::Succeeded, ExecutionState::Failed));
    EXPECT_FALSE(isValidExecutionTransition(ExecutionState::RolledBack, ExecutionState::Succeeded));
}

TEST(ExecutionStateMachine, TerminalsStuck) {
    for (auto t : {ExecutionState::Succeeded, ExecutionState::Failed,
                   ExecutionState::RolledBack, ExecutionState::Canceled}) {
        EXPECT_TRUE(isTerminalExecutionState(t));
        EXPECT_FALSE(isValidExecutionTransition(t, ExecutionState::Install));
    }
}

TEST(ExecutionStateMachine, StringRoundTrip) {
    for (int i = 0; i <= static_cast<int>(ExecutionState::Canceled); ++i) {
        auto s = static_cast<ExecutionState>(i);
        const char* name = executionStateToString(s);
        ExecutionState back;
        ASSERT_TRUE(executionStateFromString(name, back)) << name;
        EXPECT_EQ(back, s) << name;
    }
}

// ---------------------------------------------------------------------------
// Protobuf 映射往返（vehicle.fota.v1，CR-011）
// ---------------------------------------------------------------------------
TEST(StateProtoMapping, VehicleTaskRoundTrip) {
    for (int i = 0; i <= static_cast<int>(VehicleTaskState::Ended); ++i) {
        auto s = static_cast<VehicleTaskState>(i);
        auto p = toProto(s);
        if (s == VehicleTaskState::None) {
            // 新契约无 VEHICLE_TASK_STATUS_NONE；None 映射为 UNSPECIFIED（不设置）
            EXPECT_EQ(p, ::vehicle::fota::v1::VEHICLE_TASK_STATUS_UNSPECIFIED) << i;
        } else {
            EXPECT_NE(p, ::vehicle::fota::v1::VEHICLE_TASK_STATUS_UNSPECIFIED) << i;
        }
        EXPECT_EQ(fromProtoVehicleTask(p), s) << i;
    }
}

TEST(StateProtoMapping, ExecutionRoundTrip) {
    for (int i = 0; i <= static_cast<int>(ExecutionState::Canceled); ++i) {
        auto s = static_cast<ExecutionState>(i);
        auto p = toProto(s);
        EXPECT_NE(p, ::vehicle::fota::v1::EXECUTION_STATUS_UNSPECIFIED) << i;
        EXPECT_EQ(fromProtoExecution(p), s) << i;
    }
}
