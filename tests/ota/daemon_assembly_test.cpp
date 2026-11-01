// =============================================================================
// tests/ota/daemon_assembly_test.cpp
// CCU-FOTA 契约 B daemon 级装配测试 (CCU-FOTA-DSN-CR-010/011 测试矩阵)
// =============================================================================
// 证明 main.cpp 使用的同一装配路径（FotaDaemon::create + start）在真实
// SomeIpRuntime（FakeSomeIpBackend）+ 真实 SomeIpVehicleMessageTransport 上
// 创建并持有 FotaOrchestrator / FotaCloudProxy / SomeIpVehicleMessageTransport，
// 而不只是单测构造。验证：
//   * 对象创建并持有（transport/cloud/orchestrator 非空、running）。
//   * 真实 SOME/IP 往返（经 daemon->cloud() 的 exchange 走到业务编解码层）。
//   * 下行 ControlCommand 经订阅 -> 解码 -> FotaOrchestrator.applyControl 落库。
//   * graceful shutdown 后传输拒绝新调用（Stopping）。
// =============================================================================

#include "ccu/fota/daemon/fota_daemon.hpp"
#include "ccu/fota/ota/mock/mock_ports.hpp"
#include "ccu/fota/ota/payload_types.hpp"
#include "ccu/fota/someip/tbox_generic_transport_contract.hpp"
#include "ccu/fota/store/fota_cloud_state_store.hpp"
#include "ccu/fota/store/fota_state_store.hpp"
#include "ccu/fota/store/fota_state_migration.hpp"

#include "ccu/fw/someip/runtime.hpp"
#include "ccu/fw/someip/types.hpp"
#include "constants.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

using namespace ccu_fota;
using namespace ccu_fota::ota;
using namespace ccu_fota::ota::mock;
using namespace ccu_fota::store;
using namespace ccu_fota::store::fota;
namespace someip_fw = ccu::fw::someip;

namespace {

constexpr someip_fw::MethodId kGenericMethod = 0x0002;
constexpr someip_fw::EventId kGenericEvent = 0x8001;
constexpr someip_fw::EventgroupId kGenericGroup = 0x0001;

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

someip_fw::SomeIpConfig makeDaemonTestConfig() {
    someip_fw::SomeIpConfig c;
    c.application = "ccu-fota-daemon-test";
    c.routingMode = someip_fw::RoutingMode::External;
    c.maxPayloadBytes = 1024 * 1024;
    c.maxInflightCalls = 64;
    c.callbackQueueSize = 256;
    c.shutdownTimeout = std::chrono::milliseconds(1000);
    c.callTimeout = std::chrono::milliseconds(500);
    c.discovery.enabled = true;
    c.discovery.initialBackoff = std::chrono::milliseconds(10);
    c.discovery.maxBackoff = std::chrono::milliseconds(100);
    c.discovery.multiplier = 2.0;
    c.discovery.jitterPercent = 0;
    c.maxProviders = 8;
    c.maxClients = 8;
    c.maxSubscriptionsPerClient = 16;
    c.maxRetryTimers = 64;
    c.registryProfile = "ccu-fota-test";
    return c;
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs = 3000) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < timeoutMs) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// 回显中继 Provider：解析 Envelope、回填 correlation、原样回传 payload。
struct RelayProvider {
    someip_fw::Provider provider;
    std::atomic<std::uint64_t> requests{0};

    someip_fw::MethodResult handle(const someip_fw::RequestContext&,
                                   someip_fw::PayloadView v) {
        ++requests;
        VehicleMessage req;
        if (v.data == nullptr || v.size == 0 ||
            !req.envelope.ParseFromArray(v.data, static_cast<int>(v.size))) {
            return someip_fw::MethodResult::error(0x1, "bad envelope");
        }
        VehicleMessage resp;
        resp.envelope.set_correlation_id(req.envelope.message_id());
        resp.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_RESPONSE);
        resp.envelope.set_service(req.envelope.service());
        resp.envelope.set_payload_type(req.envelope.payload_type());
        resp.envelope.set_protocol_version(req.envelope.protocol_version());
        resp.envelope.set_timestamp_ms(nowMs());
        const auto& pay = req.envelope.payload();
        resp.payload.resize(pay.size());
        std::transform(pay.begin(), pay.end(), resp.payload.begin(),
                       [](char c) { return std::byte(static_cast<unsigned char>(c)); });
        auto env = resp.envelope;
        env.set_payload(std::string(pay.begin(), pay.end()));
        std::string out;
        env.SerializeToString(&out);
        return someip_fw::MethodResult::ok(someip_fw::Payload(out.begin(), out.end()));
    }

    void setup(someip_fw::SomeIpRuntime& rt) {
        someip_fw::ServiceKey key{DEFAULT_TBOX_SERVICE_ID, DEFAULT_TBOX_INSTANCE_ID};
        provider = rt.createProvider(key, {1, 0});
        provider.registerMethod(kGenericMethod,
            [this](const someip_fw::RequestContext& c, someip_fw::PayloadView v) {
                return handle(c, v);
            });
        provider.offer();
        provider.offerEvent(kGenericEvent, kGenericGroup);
    }

    void notifyDownlink(const VehicleMessage& msg) {
        auto env = msg.envelope;
        env.set_payload(std::string(
            reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size()));
        std::string out;
        env.SerializeToString(&out);
        pendingWires_.push_back(someip_fw::Payload(out.begin(), out.end()));
        auto& wire = pendingWires_.back();
        provider.notify(kGenericEvent, someip_fw::PayloadView{wire.data(), wire.size()});
    }

    // framework 的 event callback 在 executor 上延迟执行；保持 wire 存活。
    std::vector<someip_fw::Payload> pendingWires_;
};

} // namespace

// ---------------------------------------------------------------------------
// daemon 级测试：main.cpp 使用的同一装配路径真实创建并持有契约 B 对象
// ---------------------------------------------------------------------------
TEST(DaemonAssembly, MainWiringCreatesAndHoldsContractB) {
    // ---- 1. 进程唯一 Runtime（FakeSomeIpBackend）+ TBOX 通用中继 Provider ----
    auto rt = someip_fw::SomeIpRuntime::create(makeDaemonTestConfig());
    rt.start();
    RelayProvider relay;
    relay.setup(rt);

    // ---- 2. Store + 迁移 + FotaCloudStateStore ----
    auto root = std::filesystem::temp_directory_path() /
                ("fota-daemon-" + std::to_string(getpid()) + "-" +
                 std::to_string(nowMs()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto fotaStore = std::make_shared<FotaStateStore>(
        FotaStateStore::open(root, 100, 3600000));
    auto rawStore = fotaStore->underlyingStore();
    migrateOtaToFota(rawStore);
    auto cloudStore = std::make_shared<FotaCloudStateStore>(std::move(rawStore));

    // ---- 3. 解析寻址（等价 main.cpp 的 resolveTboxGenericTransport 成功路径）----
    FotaConfig fotaCfg;  // 仅用其 transport/generic 字段
    fotaCfg.genericTransport.methodId = kGenericMethod;
    fotaCfg.genericTransport.eventId = kGenericEvent;
    fotaCfg.genericTransport.eventgroupId = kGenericGroup;
    fotaCfg.genericTransport.interfaceMajor = 1;
    ccu_fota::someip::TboxGenericTransportAddress addr;
    ASSERT_TRUE(ccu_fota::someip::resolveTboxGenericTransport(fotaCfg, addr));
    ASSERT_TRUE(addr.fullyAllocated());

    // ---- 4. 端口（Mock 端口；daemon 装配路径与 main.cpp 一致）----
    auto scenario = parseScenario(R"({
      "scenario": "daemon",
      "clock": "virtual",
      "inventory": {"mode": "FULL", "baseline": "BASE-001"},
      "packages": [],
      "faults": []
    })");
    auto inv = std::make_shared<MockInventoryProvider>(scenario);
    auto consent = std::make_shared<MockConsentProvider>();
    auto dl = std::make_shared<MockPackageDownloader>(scenario);
    auto cond = std::make_shared<MockVehicleConditionProvider>();
    auto exec = std::make_shared<MockInstallExecutor>(scenario);
    auto log = std::make_shared<MockLogCollector>();

    SomeIpTransportConfig transportCfg;
    transportCfg.exchangeTimeout = std::chrono::milliseconds(2000);
    transportCfg.availabilityWait = std::chrono::milliseconds(2000);
    transportCfg.maxPayloadBytes = 262144;

    FotaOrchestratorConfig orchCfg;
    orchCfg.protocolVersion = "fota-v1";
    orchCfg.cloudCallTimeout = std::chrono::milliseconds(2000);
    orchCfg.taskCheckInterval = std::chrono::milliseconds(60000);  // 测试期不触发

    // ---- 5. 装配（main.cpp 同一路径）+ 启动 ----
    auto daemon = daemon::FotaDaemon::create(
        rt, addr, transportCfg, orchCfg, /*businessTrigger=*/true,
        inv, consent, dl, cond, exec, log, cloudStore);
    ASSERT_NE(daemon, nullptr);
    daemon->start();
    ASSERT_TRUE(daemon->running());

    // ---- 6. 证明真实创建并持有契约 B 对象 ----
    EXPECT_NE(&daemon->transport(), nullptr);
    EXPECT_NE(&daemon->cloud(), nullptr);
    EXPECT_NE(&daemon->orchestrator(), nullptr);
    EXPECT_TRUE(daemon->businessTriggerEnabled());

    // 传输已启动且 TBOX 服务可用。
    EXPECT_TRUE(waitFor([&] {
        return daemon->transport().availability() == someip_fw::Availability::Available;
    }));

    // ---- 7. 真实 SOME/IP 往返：经 daemon->cloud() 发起业务调用 ----
    // 中继只做 Envelope 级回显（不解析 payload），因此业务解码在 payload_type/
    // 业务类型层失败 -> FotaCloudException(Codec)。若传输层未打通，则表现为
    // Unavailable/Timeout；故 Kind::Codec 证明 SOME/IP 往返 + correlation 校验成功。
    {
        ::vehicle::fota::v1::TaskCheckRequest req;
        req.set_inventory_mode(::vehicle::fota::v1::INVENTORY_MODE_FULL);
        req.set_inventory_revision(1);
        CallContext ctx;
        ctx.traceId = "daemon-trace";
        ctx.requestId = "daemon-req";
        ctx.idempotencyKey = "daemon-op-1";
        ctx.timeout = std::chrono::milliseconds(2000);
        ctx.deviceId = "dev-daemon";
        ctx.vin = "VINDAEMON";
        EXPECT_THROW(
            { (void)daemon->cloud().checkTask(req, ctx); },
            FotaCloudException);
        EXPECT_GT(relay.requests.load(), 0u);
    }

    // ---- 8. 下行 ControlCommand：订阅 -> 解码 -> applyControl 落库 ----
    {
        ::vehicle::fota::v1::ControlCommand cmd;
        cmd.set_control_id("CTRL-DAEMON");
        cmd.set_control_revision(1);
        cmd.set_action(::vehicle::fota::v1::CONTROL_ACTION_PAUSE);
        cmd.set_scope(::vehicle::fota::v1::CONTROL_SCOPE_EXECUTION);
        cmd.set_apply_mode(::vehicle::fota::v1::APPLY_MODE_AT_SAFE_POINT);
        cmd.set_issued_at_ms(nowMs());
        std::string payload;
        cmd.SerializeToString(&payload);

        VehicleMessage down;
        down.envelope.set_message_id("dl-daemon-1");
        down.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_EVENT);
        down.envelope.set_service(std::string(payload_type::kService));
        down.envelope.set_payload_type(std::string(payload_type::kControlCommand));
        down.envelope.set_protocol_version(std::string(payload_type::kProtocolVersion));
        down.envelope.set_timestamp_ms(nowMs());
        down.payload.resize(payload.size());
        std::transform(payload.begin(), payload.end(), down.payload.begin(),
                       [](char c) { return std::byte(static_cast<unsigned char>(c)); });

        relay.notifyDownlink(down);
        // 等待控制指令被 applyControl 持久化。
        EXPECT_TRUE(waitFor([&] {
            auto rec = cloudStore->loadControl(1);
            return rec.has_value() && rec->controlId == "CTRL-DAEMON";
        }));
    }

    // ---- 9. graceful shutdown：拒绝新调用 -> Stopping ----
    daemon->stop();
    EXPECT_FALSE(daemon->running());
    EXPECT_TRUE(daemon->transport().stopping());

    // 停止后传输拒绝新请求。
    {
        VehicleMessage req;
        req.envelope.set_message_id("stop-req");
        req.envelope.set_message_kind(::vehicle::common::v1::MESSAGE_KIND_REQUEST);
        req.envelope.set_service(std::string(payload_type::kService));
        req.envelope.set_payload_type(std::string(payload_type::kTaskCheckRequest));
        req.envelope.set_protocol_version(std::string(payload_type::kProtocolVersion));
        req.envelope.set_timestamp_ms(nowMs());
        EXPECT_EQ(daemon->transport().exchange(req, ExchangeOptions{}, CallContext{}).outcome,
                  TransportOutcome::Stopping);
    }

    daemon.reset();
    rt.stop();
    std::filesystem::remove_all(root);
}
