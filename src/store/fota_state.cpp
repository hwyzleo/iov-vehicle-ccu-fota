// =============================================================================
// src/store/fota_state.cpp
// CCU-FOTA 状态模型辅助实现 (CCU-FOTA-DSN-CR-005)
// =============================================================================

#include "ccu/fota/store/fota_state.hpp"

namespace ccu_fota {
namespace store {

bool jobPhaseFromString(const std::string& s, JobPhase& out) {
    if (s == "Accepted")                  { out = JobPhase::Accepted; return true; }
    if (s == "Collecting")                { out = JobPhase::Collecting; return true; }
    if (s == "Assembled")                 { out = JobPhase::Assembled; return true; }
    if (s == "SubmitPrepared")            { out = JobPhase::SubmitPrepared; return true; }
    if (s == "SubmitUnknown")             { out = JobPhase::SubmitUnknown; return true; }
    if (s == "RetryWaiting")              { out = JobPhase::RetryWaiting; return true; }
    if (s == "CompletedPendingCleanup")   { out = JobPhase::CompletedPendingCleanup; return true; }
    return false;
}

bool triggerReasonFromString(const std::string& s, TriggerReason& out) {
    if (s == "AutoStart")     { out = TriggerReason::AutoStart; return true; }
    if (s == "ChangeEvent")   { out = TriggerReason::ChangeEvent; return true; }
    if (s == "CloudRequest")  { out = TriggerReason::CloudRequest; return true; }
    if (s == "Recovery")      { out = TriggerReason::Recovery; return true; }
    return false;
}

} // namespace store
} // namespace ccu_fota
