// =============================================================================
// tests/someip/fota_someip_test_util.h
// CCU-FOTA SOME/IP 契约测试工具 (CCU-FOTA-DSN-CR-007 §测试设计)
// =============================================================================
// 使用 ccu-framework-someip 公开 API（SomeIpRuntime::create 默认后端 =
// 确定性 FakeSomeIpBackend，CR-005 §15 step 1）+ FOTA 自实现的 InlineExecutor /
// DiagnosticsSink，不依赖框架私有测试工具。Provider/Client 在进程内直接通信。
// =============================================================================
#pragma once

#include "ccu/fw/someip/runtime.hpp"
#include "ccu/fw/someip/types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ccu_fota_test {

namespace someip_fw = ccu::fw::someip;

// 同步执行所有 post 的任务；确定、单线程，适合大多数单元/契约测试。
class InlineExecutor : public someip_fw::Executor {
public:
    void post(std::function<void()> task) override { task(); }
    void shutdown() override {}
};

// 队列任务；drain() 显式执行。用于 timeout/cancel 测试（handler 不提前执行）。
class DeferredExecutor : public someip_fw::Executor {
public:
    void post(std::function<void()> task) override { tasks_.push_back(std::move(task)); }
    void shutdown() override { tasks_.clear(); }
    std::size_t drain() {
        std::vector<std::function<void()>> t;
        t.swap(tasks_);
        for (auto& f : t) f();
        return t.size();
    }
    std::size_t pending() const { return tasks_.size(); }
private:
    std::vector<std::function<void()>> tasks_;
};

// 捕获所有 diagnostics 事件，供可观测性/安全断言。
class CountingDiagnosticsSink : public someip_fw::DiagnosticsSink {
public:
    void report(const someip_fw::DiagnosticsEvent& ev) override { events_.push_back(ev); }
    const std::vector<someip_fw::DiagnosticsEvent>& events() const { return events_; }
    bool empty() const { return events_.empty(); }
private:
    std::vector<someip_fw::DiagnosticsEvent> events_;
};

// 最小有效测试配置（jitterPercent=0 确定性退避）。
inline someip_fw::SomeIpConfig makeTestConfig(const std::string& app = "ccu-fota-test") {
    someip_fw::SomeIpConfig cfg;
    cfg.application = app;
    cfg.routingMode = someip_fw::RoutingMode::External;
    cfg.maxPayloadBytes = 1024 * 1024;
    cfg.maxInflightCalls = 64;
    cfg.callbackQueueSize = 256;
    cfg.shutdownTimeout = std::chrono::milliseconds(1000);
    cfg.callTimeout = std::chrono::milliseconds(500);
    cfg.discovery.enabled = true;
    cfg.discovery.initialBackoff = std::chrono::milliseconds(10);
    cfg.discovery.maxBackoff = std::chrono::milliseconds(100);
    cfg.discovery.multiplier = 2.0;
    cfg.discovery.jitterPercent = 0;
    cfg.maxProviders = 8;
    cfg.maxClients = 8;
    cfg.maxSubscriptionsPerClient = 16;
    cfg.maxRetryTimers = 64;
    cfg.registryProfile = "ccu-fota-test";
    return cfg;
}

} // namespace ccu_fota_test
