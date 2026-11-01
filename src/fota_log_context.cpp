#include "fota_log_context.h"
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace ccu_fota {

ccu::fw::log::ContextScope make_context_scope(
    const std::string& trace_id,
    const std::string& request_id,
    const std::string& session_id
) {
    ccu::fw::log::LogContext ctx;
    ctx.trace_id = trace_id;
    ctx.request_id = request_id;
    ctx.session_id = session_id;
    return ccu::fw::log::ContextScope(std::move(ctx));
}

ccu::fw::log::ContextScope make_someip_child_scope(
    const std::string& service_id,
    const std::string& method_id,
    const std::string& client_id,
    const std::string& remote_endpoint
) {
    ccu::fw::log::LogContext ctx;

    // 继承当前上下文的 trace_id / request_id
    const auto* current = ccu::fw::log::ContextScope::current();
    if (current) {
        ctx.trace_id = current->trace_id;
        ctx.request_id = current->request_id;
        ctx.session_id = current->session_id;
    }

    // 附加 SOME/IP 调用上下文
    ctx.someip = std::make_unique<ccu::fw::log::SomeIpContext>();
    ctx.someip->service_id = service_id;
    ctx.someip->method_id = method_id;
    ctx.someip->client_id = client_id;
    ctx.someip->remote_endpoint = remote_endpoint;

    return ccu::fw::log::ContextScope(std::move(ctx));
}

ccu::fw::log::ContextScope make_someip_context_scope(
    const std::string& trace_id,
    const std::string& request_id,
    const std::string& service_id,
    const std::string& method_id,
    const std::string& client_id,
    const std::string& remote_endpoint
) {
    ccu::fw::log::LogContext ctx;
    ctx.trace_id = trace_id;
    ctx.request_id = request_id;

    ctx.someip = std::make_unique<ccu::fw::log::SomeIpContext>();
    ctx.someip->service_id = service_id;
    ctx.someip->method_id = method_id;
    ctx.someip->client_id = client_id;
    ctx.someip->remote_endpoint = remote_endpoint;

    return ccu::fw::log::ContextScope(std::move(ctx));
}

std::string generate_trace_id() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::ostringstream oss;
    oss << "fota-trace-" << timestamp << "-" << dis(gen);
    return oss.str();
}

std::string generate_request_id() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::ostringstream oss;
    oss << "fota-req-" << timestamp << "-" << dis(gen);
    return oss.str();
}

} // namespace ccu_fota
