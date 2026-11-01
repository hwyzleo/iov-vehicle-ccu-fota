#pragma once

// =============================================================================
// include/ccu/fota/store/fota_state_store.hpp
// CCU-FOTA 运行状态存储封装 (CCU-FOTA-DSN-CR-005)
// =============================================================================
// 静态链接 ccu-framework-store，业务代码不直接创建/截断/重命名/锁定状态文件。
// 提供 inventory.sequence / last_success / dedupe / active_job 的类型化加载、
// durable 保存与损坏处置。
//
// 启动顺序：Config -> Logger -> Store::open -> 格式检查/迁移 -> 状态恢复 ->
//           业务模块 -> SOME/IP -> 自动任务。
//
// 不提供跨 key ACID；一致性由严格写入顺序、稳定 reportId/snapshotSeq、
// 幂等标识与启动对账保证。
// =============================================================================

#include "ccu/fota/store/fota_state.hpp"
#include "ccu/fota/store/fota_state_serializer.hpp"

#include "store.h"          // ccu::fw::store::Store
#include "store_types.h"    // StoreOptions / StoreException

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace ccu_fota {
namespace store {

// ---------------------------------------------------------------------------
// SeqAllocBlocked - 序号分配被阻断（溢出/回退/损坏/IO 失败）
// 映射为 STATE_BLOCKED，停止自动/主动上报；禁止自动归零。
// ---------------------------------------------------------------------------
class SeqAllocBlocked : public std::runtime_error {
public:
    explicit SeqAllocBlocked(const std::string& message, std::string errorCode = "")
        : std::runtime_error(message), errorCode(std::move(errorCode)) {}
    std::string errorCode;
};

// ---------------------------------------------------------------------------
// FotaStateStore - 框架 Store 的 FOTA 专用封装
// ---------------------------------------------------------------------------
class FotaStateStore {
public:
    // 打开 store：规范化 root、创建 0700 服务目录、对既有 key 执行格式检查/迁移。
    // 路径/权限失败 -> fota.store.open.failed (CCU-FW-0101) 并抛出。
    static FotaStateStore open(const std::filesystem::path& root,
                               std::uint32_t dedupeMaxEntries,
                               std::int64_t dedupeTtlMs);

    // ---- 序号 (inventory.sequence) ----
    // 分配下一个 snapshotSeq：加载 -> 单调校验 -> 溢出检查 -> durable save -> 返回。
    // save() 完成（temp->fsync->rename->dir fsync）后序号才可使用。
    // 溢出/回退/损坏/IO 失败 -> STATE_BLOCKED，抛 SeqAllocBlocked；禁止自动归零。
    std::uint64_t allocateSnapshotSeq();

    // 序号是否处于阻断状态（损坏/溢出）。
    bool isSequenceBlocked() const { return sequenceBlocked_; }

    // 加载当前序号状态（恢复/诊断用）。阻断时返回 nullopt。
    std::optional<SequenceState> loadSequence();

    // ---- 最近成功快照 (inventory.last_success) ----
    // 返回 nullopt 表示缺失；损坏时 isLastSuccessAvailable()=false 并返回 nullopt。
    std::optional<LastSuccessState> loadLastSuccess();
    void saveLastSuccess(const LastSuccessState& s);
    bool isLastSuccessAvailable() const { return lastSuccessAvailable_; }

    // ---- 去重窗口 (inventory.dedupe) ----
    // 返回当前去重状态；缺失或损坏时返回空状态（isDedupeAvailable()=false）。
    DedupeState loadDedupe();
    void saveDedupe(const DedupeState& s);
    bool isDedupeAvailable() const { return dedupeAvailable_; }

    // ---- 在途/重试检查点 (inventory.active_job) ----
    // 返回 nullopt 表示无在途任务；损坏时 isActiveJobCorrupted()=true 并返回 nullopt
    // （不得恢复为成功，由恢复器分配新序号保守重采集）。
    std::optional<ActiveJobState> loadActiveJob();
    void saveActiveJob(const ActiveJobState& s);
    void removeActiveJob();
    bool isActiveJobCorrupted() const { return activeJobCorrupted_; }

    // 同步模式为 no-op；保留供非同步模式使用。
    void flush() { store_.flush(); }

    // CCU-FOTA-DSN-CR-009: 暴露底层 framework Store，供 FotaCloudStateStore 共享同一
    // service "fota" 存储（避免重复 open 造成锁冲突）。
    ccu::fw::store::Store underlyingStore() const { return store_; }

    // 持久化根目录（诊断用）。
    const std::filesystem::path& root() const { return root_; }

private:
    ccu::fw::store::Store store_;
    std::filesystem::path root_;
    std::uint32_t dedupeMaxEntries_;
    std::int64_t  dedupeTtlMs_;
    bool sequenceBlocked_ = false;
    bool lastSuccessAvailable_ = true;
    bool dedupeAvailable_ = true;
    bool activeJobCorrupted_ = false;

    FotaStateStore(ccu::fw::store::Store s, std::filesystem::path root,
                   std::uint32_t dedupeMaxEntries, std::int64_t dedupeTtlMs);

    // 对单个 key 执行格式检查/迁移/校验。corruptionFlag 按各 key 语义设置。
    template <typename StateT>
    void migrateAndValidate(const char* key, std::uint32_t currentVersion,
                            StateT (*decodeFn)(const std::string&),
                            std::string (*encodeFn)(const StateT&),
                            bool& corruptionFlag);
};

} // namespace store
} // namespace ccu_fota
