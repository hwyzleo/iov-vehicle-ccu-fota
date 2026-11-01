#pragma once

// =============================================================================
// include/ccu/fota/ota/event_sink.hpp
// CCU-FOTA 执行事件回调接口 (CCU-FOTA-DSN-CR-009 §事件/控制 / CR-011 类型校准)
// =============================================================================
// Executor 通过 EventSink 产生阶段事件。EventJournal 先 durable 写入 payload，
// 再分配/提交 sequenceNo。本接口仅由 FotaOrchestrator 实现，Executor 不直接接触
// store 或 CloudProxy。
// =============================================================================

#include "vehicle/fota/v1/execution.pb.h"

namespace ccu_fota {
namespace ota {

// Executor 通过该接口投递阶段事件。实现负责 durable 持久化 + 序号分配 + 发送。
class EventSink {
public:
    virtual ~EventSink() = default;

    // 投递一条执行事件。返回是否已 durable 接受（false 表示 journal 已满或关闭）。
    // sequenceNo 由实现分配，Executor 不指定。
    virtual bool emit(const ::vehicle::fota::v1::ExecutionEvent& evt) = 0;
};

} // namespace ota
} // namespace ccu_fota
