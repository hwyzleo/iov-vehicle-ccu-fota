# CCU-FOTA Architecture

## System Overview

CCU-FOTA is a vehicle software version management service that collects and reports software inventory from ECUs to the cloud via TBOX.

## Data Flow

```
各 ECU →(UDS/SOME-IP)→ CCU-DIAG →(SOME/IP)→ CCU-FOTA →(SOME/IP)→ TBOX →(MQTT)→ 云端
```

## Components

### SnapshotAssembler

**Responsibility**: Consumes version list from CCU-DIAG and assembles vehicle software snapshot.

**Key Features**:
- Calls `collectVehicleInventory()` from CCU-DIAG
- Manages snapshot sequence numbers
- Implements reporting throttling
- Validates snapshot integrity

**Dependencies**:
- `SomeIpFotaClient`: For CCU-DIAG communication

### SomeIpFotaClient

**Responsibility**: Manages SOME/IP communication with CCU-DIAG.

**Key Features**:
- Connects to CCU-DIAG service
- Provides `collectVehicleInventory()` interface
- Provides `getEcuVersion()` interface
- Provides `getRegistryVersion()` interface

### SomeIpTboxClient

**Responsibility**: Manages SOME/IP communication with TBOX.

**Key Features**:
- Connects to TBOX service
- Provides `reportSoftwareInventory()` interface
- Implements retry mechanism

## Data Models

### VehicleSoftwareSnapshot

```cpp
struct VehicleSoftwareSnapshot {
    std::string vin;
    VinSource vin_source;
    std::optional<std::string> baseline_id;
    BaselineSource baseline_source;
    std::string registry_version;
    std::string collected_at;
    CollectionStatus overall_result;
    uint64_t snapshot_seq;
    std::vector<EcuVersionEntry> ecu_list;
};
```

### EcuVersionEntry

```cpp
struct EcuVersionEntry {
    std::string ecu_id;
    std::optional<std::string> part_number;           // 总成/备件件号 -> hardware_part_number
    std::optional<std::string> software_part_number;  // 独立软件件号 -> software_part_number（CR-014）
    std::optional<std::string> sw_version;
    std::optional<std::string> hw_version;
    VersionSource source;
    EcuStatus status;
    std::optional<std::string> error_code;
};
```

## Configuration

Configuration is loaded from YAML files with the following structure:

```yaml
fota:
  snapshot:
    max_ecu_count: 100
    throttle_interval_ms: 5000
  someip:
    diag_service:
      service_id: 0x0001
      ip_address: "127.0.0.1"
      port: 30501
```

## Error Handling

### Error Codes

- CCU-FOTA-1003: 清单组装失败
- CCU-FOTA-1004: 车内发送失败
- CCU-FOTA-1005: TBOX 不可达
- CCU-FOTA-1006: 上报超时

### Retry Mechanism

The service implements configurable retry mechanism for TBOX communication:
- Configurable maximum retry count
- Configurable retry interval
- Exponential backoff (optional)

## Threading Model

- Main thread: Handles event loop and coordination
- SOME/IP client threads: Handle communication with CCU-DIAG and TBOX
- Configuration thread: Handles hot-reload of configuration (future)

## Security Considerations

- SOME/IP communication should be secured using appropriate mechanisms
- VIN validation should be performed
- Snapshot integrity should be verified
- Rate limiting should be implemented to prevent abuse

## NON_PRODUCTION 真实链路 Mock 联调 (CCU-FOTA-DSN-CR-015)

量产 daemon 固定装配 Conservative 车内端口，并始终使用真实 DIAG 清单、
真实 DIAG 安装版本登记、真实 TBOX 通用传输与可信系统时钟。NON_PRODUCTION
联调只是在此之上按**双重门禁**替换五个车内副作用端口：

| 门禁 | 取值 | 行为 |
|---|---|---|
| 构建 `FOTA_ENABLE_TEST_DOUBLES` | OFF（量产） | 不编译 Mock/场景源码；`fota.mock.enabled=true` 启动失败；无 install 规则 |
| 构建 | ON（独立构建目录） | 编译 Mock 与场景；install 规则关闭，不进入 `ccu-fota-runtime` |
| 运行 `fota.mock.enabled` | `false`（缺省） | 仍装配 Conservative 端口 |
| 运行 | `true` + 合法场景 | 装配 Mock Consent/Download/Condition/Install/Log；Inventory/Registrar/Clock/Transport 保持真实 |

约束：

- `fota.mock.enabled=true` 时 `scenario_path` 必填、可读、场景校验通过；未知故障名
  直接 fail-closed。
- 真实链路联调必须 `fota.mock.virtual_clock=false`（daemon 拒绝虚拟时钟）。
- 场景只描述安装阶段、进度、结果与故障；ECU/包/版本/计划节点等任务事实全部来自
  云端冻结任务（`TaskCheckResponse.task`），Mock 不访问真实 CDN、不刷写 ECU。
- 摘要（package manifest / stage result / execution event / final result /
  local readiness）使用 framework-hash + 版本化规范化输入，不使用占位值。
- 启动日志输出 `fota.service.profile` 与 `fota.mock.activated`（profile、
  场景名、场景摘要），不输出场景原文与 VIN。
- 量产产物可用 `scripts/check_no_test_doubles.sh` 扫描，确认不含 Mock 符号与
  联调场景字符串。

联调入口（不安装）：

```bash
./scripts/run_nonproduction_mock.sh                       # 使用 config/nonproduction
./scripts/run_nonproduction_mock.sh --scenario config/nonproduction/scenarios/install_failed.json
```

前提：`fota.someip.generic_transport.*` 与 `fota.diag.registration.method_id` 已由
VEH-SOMEIP Registry 分配；`common.store.root`（标准 `/var/lib/ccu`）可写；真实
TBOX、非生产云端与真实 CCU-DIAG 可达。
