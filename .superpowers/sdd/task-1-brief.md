# 任务 1：更新常量定义

**文件：**
- 修改：`include/constants.h`
- 测试：无（纯常量定义）

- [ ] **步骤 1：添加 FOTA Provider 常量**

```cpp
// 在 constants.h 中添加

// FOTA Provider 服务 (CCU-FOTA-DSN-CR-002)
constexpr uint16_t FOTA_PROVIDER_SERVICE_ID = 0x1120;
constexpr uint16_t FOTA_PROVIDER_INSTANCE_ID = 0x0001;
constexpr uint16_t FOTA_PROVIDER_PORT = 51120;

// FOTA Provider Method IDs (service-scoped)
constexpr uint16_t METHOD_REQUEST_SOFTWARE_INVENTORY = 0x0001;
```

- [ ] **步骤 2：更新 TBOX 服务常量**

```cpp
// 修改 constants.h 中的 TBOX 常量

// TBOX 服务 (CCU-FOTA-DSN-CR-002 更新)
constexpr uint16_t DEFAULT_TBOX_SERVICE_ID = 0x6101;  // 原 0x0002
constexpr uint16_t DEFAULT_TBOX_INSTANCE_ID = 0x0001;
constexpr uint16_t DEFAULT_TBOX_PORT = 56101;  // 新增端口常量
```

- [ ] **步骤 3：验证编译**

运行：`cd /Users/hwyz_leo/Projects/open-iov/vehicle/ccu/iov-vehicle-ccu-fota && mkdir -p build && cd build && cmake .. && make`
预期：编译成功，无错误

- [ ] **步骤 4：Commit**

```bash
cd /Users/hwyz_leo/Projects/open-iov/vehicle/ccu/iov-vehicle-ccu-fota
git add include/constants.h
git commit -m "feat(cr-002): add FOTA Provider and update TBOX service constants"
```
