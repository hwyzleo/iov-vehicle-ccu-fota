// =============================================================================
// src/someip/tbox_generic_transport_contract.cpp
// CCU-FOTA 契约 B TBOX 通用消息服务寻址解析实现 (CCU-FOTA-DSN-CR-010/011)
// =============================================================================
// 对 fota.someip.generic_transport.*（Registry 分配的运行配置）做 fail-closed
// 校验：范围、保留值、与本地已分配 ID 冲突、接口版本一致性。未分配时返回 false
// （契约 B 不启用），绝不猜测 ID、绝不回退 Fake 或旧私有协议。
// =============================================================================

#include "ccu/fota/someip/tbox_generic_transport_contract.hpp"

namespace ccu_fota {
namespace someip {

using ccu::fw::someip::InterfaceVersion;
using ccu::fw::someip::ServiceKey;

namespace {

// 同一 TBOX service（0x6101）上已由 Registry/过渡 SSOT 分配并投入使用的 ID。
// 通用 transport 新分配不得与之冲突（CR-010：契约 A 专用提交路径与契约 B 在同一
// 量产实例中二选一；通用 Method 不得占用专用 Method）。
bool conflictsWithLocalAllocations(const GenericTransportSpec& spec) {
    if (spec.methodId == TBOX_METHOD_REPORT_SOFTWARE_INVENTORY) return true;
    // 本地 TBOX 未分配 Event/Eventgroup；若未来分配，在此追加。
    return false;
}

} // namespace

bool resolveTboxGenericTransport(const FotaConfig& cfg,
                                 TboxGenericTransportAddress& out) {
    const auto& spec = cfg.genericTransport;
    out = TboxGenericTransportAddress{};

    // Registry 尚未分配 generic Method/Event/Eventgroup ID -> 契约 B 未启用。
    // 这是「未分配」而非「非法」：不猜测、不回退 Fake/旧协议。
    if (!spec.anyDeclared()) {
        return false;
    }

    // 部分声明（method/event/eventgroup 缺一）-> fail-closed。
    if (spec.methodId == 0 || spec.eventId == 0 || spec.eventgroupId == 0) {
        throw FotaConfigException(
            "fota.someip.generic_transport: partial Registry allocation "
            "(method_id/event_id/eventgroup_id must all be non-zero)");
    }

    // 范围与保留值校验（framework 协议范围：Method 0x0001..0x7FFF，
    // Event >= 0x8000，Eventgroup 0x0001..0xFFFE；0x0000/0xFFFF 保留）。
    if (spec.methodId == 0x0000 || spec.methodId == 0xFFFF ||
        spec.methodId > 0x7FFF) {
        throw FotaConfigException(
            "fota.someip.generic_transport.method_id out of range");
    }
    if (spec.eventId < 0x8000 || spec.eventId == 0xFFFF) {
        throw FotaConfigException(
            "fota.someip.generic_transport.event_id out of SOME/IP event range");
    }
    if (spec.eventgroupId == 0x0000 || spec.eventgroupId == 0xFFFF) {
        throw FotaConfigException(
            "fota.someip.generic_transport.eventgroup_id reserved value");
    }

    // 与本地已分配 ID 冲突 -> fail-closed（不得双发/占用契约 A 专用 Method）。
    if (conflictsWithLocalAllocations(spec)) {
        throw FotaConfigException(
            "fota.someip.generic_transport.method_id conflicts with local "
            "Registry allocation (TBOX_METHOD_REPORT_SOFTWARE_INVENTORY, contract A)");
    }

    // 接口版本一致性：TBOX-SOMEIP 过渡 SSOT 为 major=1；声明不匹配 fail-closed。
    if (spec.interfaceMajor != 1) {
        throw FotaConfigException(
            "fota.someip.generic_transport.interface_major mismatch: expected 1");
    }

    out.method = spec.methodId;
    out.event = spec.eventId;
    out.eventgroup = spec.eventgroupId;
    out.interfaceVersion = InterfaceVersion{
        static_cast<std::uint8_t>(spec.interfaceMajor), 0};

    if (!out.fullyAllocated()) {
        throw FotaConfigException(
            "fota.someip.generic_transport: unresolved address");
    }
    return true;
}

} // namespace someip
} // namespace ccu_fota
