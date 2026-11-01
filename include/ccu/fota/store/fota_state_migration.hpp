#pragma once

// =============================================================================
// include/ccu/fota/store/fota_state_migration.hpp
// CCU-FOTA 旧 ota.* 状态迁移器 (CCU-FOTA-DSN-CR-011 §Store 迁移流程)
// =============================================================================
// 一次性 release-set 迁移：将旧 vehicle.ota.v1 时代的 ota.* durable key 迁移至
// fota.*，并在开放 transport 与触发任务前完成。流程遵循设计：
//   1. 检测旧 ota.* key；无旧 key 直接返回。
//   2. 读取并校验旧 schema、完整性（未知新格式/损坏 -> fail-closed 抛异常）。
//   3. 保存旧状态备份（旧 key 在完成 marker 前不被删除）+ migration marker。
//   4. 将类型标识转换为 FOTA 新命名，保留业务 ID/revision/sequence/摘要/offset/
//      时间语义，原子写入 fota.*。
//   5. 重读并执行跨 key 一致性检查。
//   6. 写入完成 marker 后才清理旧 key；中断后幂等重试。
//   7. 未知新格式、冲突双份状态或校验失败 fail-closed，不得用时间戳猜测覆盖。
//
// 说明：旧 opaque proto-binary hex（旧 vehicle.ota.v1 消息编码）在同一 release-set
// 内随旧 package 删除且不保留双栈，无法按新契约解释，因此迁移仅保留结构化业务
// 字段（ID/revision/sequence/摘要 hex/offset/时间/状态），opaque proto 载荷不迁移；
// 迁移后由 FotaOrchestrator 经 TaskCheck/reconcile 重新同步（新契约恢复路径）。
// =============================================================================

#include "store.h"   // ccu::fw::store::Store

#include <cstddef>
#include <cstdint>
#include <string>

namespace ccu_fota {
namespace store {

// ---------------------------------------------------------------------------
// FotaMigrationResult - 迁移结果
// ---------------------------------------------------------------------------
struct FotaMigrationResult {
    bool ran = false;                 // 是否检测到旧 key 并执行（或完成清理）
    bool completed = true;            // 是否全部完成（旧 key 已清理）
    std::size_t migratedKeys = 0;     // 迁移/清理的旧 key 数量
    std::string detail;               // 摘要（不含状态内容/VIN/token）
};

// ---------------------------------------------------------------------------
// 迁移入口。幂等、fail-closed：
//   - 无旧 key：返回 {ran=false, completed=true}
//   - 有旧 key：迁移 -> 写完成 marker -> 清理旧 key；中断后幂等重试。
//   - 未知新格式/校验失败：抛异常（旧 key 保留，可重试），不部分清理。
// ---------------------------------------------------------------------------
FotaMigrationResult migrateOtaToFota(ccu::fw::store::Store& store);

} // namespace store
} // namespace ccu_fota
