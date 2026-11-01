// =============================================================================
// include/ccu/fota/someip/diag_inventory_client.hpp
// CCU-FOTA DIAG Inventory Client 适配器 (CCU-FOTA-DSN-CR-007 §Client 设计)
// =============================================================================
// 业务适配器仅包含 IDL codec、DTO 映射和 orchestrator 调用；不得包含协议栈
// Runtime、SD、socket、session 表、重连 timer 或后端类型 (CR-007 §代码布局)。
//
// 角色与 Registry 绑定：Client 请求 CCU-DIAG Service 0x1110 / Instance 0x0001 /
// TCP 51110 的版本清单接口。Service/Instance/Method/端口仅来自 Registry，由
// Runtime::createClient 绑定 ServiceKey，不在源码重新分配 (CR-007 §角色与 Registry)。
//
// 调用语义：
//   - requestService 后监听 availability；Unavailable/VersionMismatch 不发起调用
//   - 每次采集使用 CallOptions{timeout=fota.diag.collect_timeout_ms, retry=None}
//   - framework 不自动重试；orchestrator 根据 fota.diag.retry_* 产生新 request/session
//   - 成功 response 由 DIAG IDL codec 解码并保留 DIAG 业务错误
//   - late/duplicate response 不得完成其他请求（framework 保证）
// =============================================================================

#pragma once

#include "data_models.h"
#include "ccu/fw/someip/client.hpp"
#include "ccu/fw/someip/types.hpp"

#include <chrono>
#include <memory>

namespace ccu_fota {
namespace someip {

// DIAG 版本清单采集 Client。业务方法保持 virtual 以便单元测试 mock
// （沿用现有可 mock 模式）；生产实现经 framework Client 调用。
class DiagInventoryClient {
public:
    // 生产构造：包装 framework Client，注入采集超时（fota.diag.collect_timeout_ms）。
    DiagInventoryClient(ccu::fw::someip::Client client,
                        std::chrono::milliseconds collectTimeout);
    virtual ~DiagInventoryClient();

    // ---- framework 生命周期（透传）----
    void requestService();
    void releaseService();
    ccu::fw::someip::Availability availability() const;
    ccu::fw::someip::Subscription
    onAvailability(ccu::fw::someip::AvailabilityCallback callback);

    // ---- 业务接口（virtual 供 mock）----
    // 采集整车版本清单。单次 framework call，retry=None。
    // 失败时保留 framework cause（经 error_mapper 映射）。
    virtual bool collectVehicleInventory(VehicleSoftwareSnapshot& snapshot);

    // 单独读取 VIN（METHOD_READ_VIN）。供 collectVehicleInventory 内部及测试使用。
    virtual bool getVin(std::string& vin);

protected:
    // 测试子类构造：不持有 framework Client（业务方法由子类 override）。
    DiagInventoryClient();

private:
    ccu::fw::someip::Client client_;
    std::chrono::milliseconds collectTimeout_;
    bool hasClient_;
};

} // namespace someip
} // namespace ccu_fota
