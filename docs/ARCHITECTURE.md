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
