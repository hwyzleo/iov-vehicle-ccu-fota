#pragma once

/**
 * CCU-FOTA 上下文传播辅助
 *
 * 使用 ccu-framework-log 提供的 ContextScope 和 LogContext
 * 实现 trace_id / request_id 跨 SOME/IP 调用链的传播
 * （CCU-FOTA-DSN-CR-003 §上下文传播）
 */

#include "log.h"
#include "log_types.h"
#include <string>
#include <sstream>
#include <iomanip>

namespace ccu_fota {

/// 创建顶层 ContextScope 的便捷函数
/// 用法：auto scope = make_context_scope(trace_id, request_id);
[[nodiscard]] ccu::fw::log::ContextScope make_context_scope(
    const std::string& trace_id,
    const std::string& request_id,
    const std::string& session_id = ""
);

/// 创建带 SOME/IP 子作用域的 ContextScope（用于调用 CCU-DIAG / TBOX-SOMEIP 时）
/// 继承当前上下文的 trace_id/request_id，附加目标 Service/Method 信息
[[nodiscard]] ccu::fw::log::ContextScope make_someip_child_scope(
    const std::string& service_id,
    const std::string& method_id,
    const std::string& client_id = "",
    const std::string& remote_endpoint = ""
);

/// 创建带 SOME/IP 上下文的顶层 ContextScope（用于 Provider 收到入站请求时）
[[nodiscard]] ccu::fw::log::ContextScope make_someip_context_scope(
    const std::string& trace_id,
    const std::string& request_id,
    const std::string& service_id,
    const std::string& method_id,
    const std::string& client_id = "",
    const std::string& remote_endpoint = ""
);

/// 生成 trace ID（格式: fota-trace-<timestamp>-<random>）
std::string generate_trace_id();

/// 生成 request ID（格式: fota-req-<timestamp>-<random>）
std::string generate_request_id();

/// 获取当前上下文（便捷封装）
inline const ccu::fw::log::LogContext* current_context() {
    return ccu::fw::log::ContextScope::current();
}

/// 从当前上下文获取 report_id（存储在 session_id 字段中）
inline std::string current_report_id() {
    const auto* ctx = ccu::fw::log::ContextScope::current();
    return ctx ? ctx->session_id : "";
}

/// 从当前上下文获取 trace_id
inline std::string current_trace_id() {
    const auto* ctx = ccu::fw::log::ContextScope::current();
    return ctx ? ctx->trace_id : "";
}

/// 从当前上下文获取 request_id
inline std::string current_request_id() {
    const auto* ctx = ccu::fw::log::ContextScope::current();
    return ctx ? ctx->request_id : "";
}

/// 将 uint16_t 格式为规范十六进制字符串（如 0x1110）
inline std::string hex_id(uint16_t id) {
    std::ostringstream oss;
    oss << "0x" << std::setfill('0') << std::setw(4) << std::hex << id;
    return oss.str();
}

} // namespace ccu_fota
