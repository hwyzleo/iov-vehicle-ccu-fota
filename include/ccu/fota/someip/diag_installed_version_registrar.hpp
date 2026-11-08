#pragma once

// =============================================================================
// include/ccu/fota/someip/diag_installed_version_registrar.hpp
// CCU-DIAG 安装结果登记适配器 (CCU-FOTA-DSN-CR-013 §2 / US-037)
// =============================================================================
// 业务适配器：把 InstalledVersionsReport 编码为 CCU-DIAG 登记 payload，调用
// framework SOME/IP Client，并把 DIAG 的四态处置映射回 RegistrationResult。
// 不包含协议栈 Runtime/socket/session 表；寻址（Service 0x1110 / Instance 0x0001）
// 由 Runtime 绑定，Method ID 与接口版本来自 Registry 分配的运行配置
// （0 = 未分配 -> 能力关闭，不猜 ID、不回退文件，CR-013 D7）。
//
// 编解码与处置映射暴露为静态纯函数，便于在不启动 SOME/IP 的情况下做单元测试。
// =============================================================================

#include "ccu/fota/ota/ports/installed_version_registrar.hpp"
#include "ccu/fw/someip/client.hpp"
#include "ccu/fw/someip/types.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ccu_fota {
namespace someip {

// CCU-DIAG 登记接口的寻址与能力配置（Registry/IDL 运行发布物注入）。
struct DiagInstalledVersionRegistrarConfig {
    // Registry 分配的 METHOD_RECORD_INSTALLED_VERSIONS。0 = 未分配 -> 能力关闭。
    std::uint16_t methodId = 0;
    // DIAG 接口版本（与清单接口同一 Service/Instance）。
    std::uint8_t interfaceMajor = 1;
    std::uint8_t interfaceMinor = 0;
    // 功能开关：false 只表示“能力不可用”（阻止新安装），不是“跳过登记”的许可。
    bool enabled = true;
    // 单次登记调用超时（fota.diag.registration.timeout_ms）。
    std::chrono::milliseconds timeout{30000};
};

class DiagInstalledVersionRegistrar : public ota::InstalledVersionRegistrar {
public:
    // client 与清单适配器共享（同一 DIAG Service 只允许一次 requestService；
    // Client 生命周期由装配层持有）。
    DiagInstalledVersionRegistrar(
        std::shared_ptr<ccu::fw::someip::Client> client,
        DiagInstalledVersionRegistrarConfig cfg);

    ~DiagInstalledVersionRegistrar() override = default;

    // 可用 = 持有 framework Client + 功能未禁用 + Method ID 已分配 + 服务可用。
    bool available() override;

    // 幂等登记（不抛异常；所有失败以 RegistrationResult 表达）。
    ota::RegistrationResult registerInstalledVersions(
        const ota::InstalledVersionsReport& report, ota::CallContext& ctx) override;

    // ---- 纯函数编解码（单元测试可直接验证 wire 形态） ----
    // 请求 payload（schemaVersion=1 的 JSON）。
    static std::string encodeRequest(const ota::InstalledVersionsReport& report);
    // 响应 payload 解码 + 处置映射。空 payload 视为可重试失败。
    static ota::RegistrationResult decodeResponse(std::string_view payload);

private:
    std::shared_ptr<ccu::fw::someip::Client> client_;
    DiagInstalledVersionRegistrarConfig cfg_;
};

} // namespace someip
} // namespace ccu_fota
