// =============================================================================
// src/store/fota_state_store.cpp
// CCU-FOTA 运行状态存储封装实现 (CCU-FOTA-DSN-CR-005)
// =============================================================================

#include "ccu/fota/store/fota_state_store.hpp"

#include "constants.h"
#include "fota_log_adapter.h"
#include "fota_log_context.h"

#include <chrono>

namespace ccu_fota {
namespace store {

namespace {
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// 将框架 StoreException code 映射为日志 error_code（CCU-FW-0101~0105）。
std::string storeErrCode(const ccu::fw::store::StoreException& e) { return e.code; }

void logStoreError(const char* event, const std::string& key,
                   const std::string& errorCode, const std::string& stage = "") {
    FotaLogAdapter::store().error(
        event,
        "Store operation failed",
        {flog::f_str("key", key),
         flog::f_str("error_code", errorCode),
         flog::f_str("stage", stage)});
}
} // namespace

// ===========================================================================
// 构造与打开
// ===========================================================================
FotaStateStore::FotaStateStore(ccu::fw::store::Store s, std::filesystem::path root,
                               std::uint32_t dedupeMaxEntries, std::int64_t dedupeTtlMs)
    : store_(std::move(s))
    , root_(std::move(root))
    , dedupeMaxEntries_(dedupeMaxEntries)
    , dedupeTtlMs_(dedupeTtlMs) {}

FotaStateStore FotaStateStore::open(const std::filesystem::path& root,
                                    std::uint32_t dedupeMaxEntries,
                                    std::int64_t dedupeTtlMs) {
    ccu::fw::store::StoreOptions opts;
    opts.root = root;
    opts.flushMode = ccu::fw::store::FlushMode::Synchronous; // sequence/last_success/active_job 必须 durable
    // dedupe 初期也同步；后续 CR 可评估显式 debounce

    auto openStore = [&]() -> ccu::fw::store::Store {
        try {
            return ccu::fw::store::Store::open("fota", opts);
        } catch (const ccu::fw::store::StoreException& e) {
            // CCU-FW-0101 路径/权限失败；CCU-FW-0103 lock 失败
            logStoreError(fota_events::STORE_OPEN_FAILED, "(root)", e.code, e.stage);
            throw; // fail-closed，不得切 /tmp
        }
    };

    FotaStateStore self(openStore(), root, dedupeMaxEntries, dedupeTtlMs);

    // 对既有 key 执行格式检查 / 迁移 / 校验。各 key 损坏按其语义置标志。
    bool seqCorrupted = false;
    bool lastSuccessCorrupted = false;
    bool dedupeCorrupted = false;
    bool activeJobCorrupted = false;
    self.migrateAndValidate<SequenceState>(keys::SEQUENCE, schema::SEQUENCE_VERSION,
                                           &decodeSequence, &encodeSequence,
                                           seqCorrupted);
    self.migrateAndValidate<LastSuccessState>(keys::LAST_SUCCESS, schema::LAST_SUCCESS_VERSION,
                                              &decodeLastSuccess, &encodeLastSuccess,
                                              lastSuccessCorrupted);
    self.migrateAndValidate<DedupeState>(keys::DEDUPE, schema::DEDUPE_VERSION,
                                         &decodeDedupe, &encodeDedupe,
                                         dedupeCorrupted);
    self.migrateAndValidate<ActiveJobState>(keys::ACTIVE_JOB, schema::ACTIVE_JOB_VERSION,
                                            &decodeActiveJob, &encodeActiveJob,
                                            activeJobCorrupted);
    // 语义映射：sequence/active_job 损坏即阻断/保守；last_success/dedupe 损坏即不可用。
    self.sequenceBlocked_ = seqCorrupted;
    self.lastSuccessAvailable_ = !lastSuccessCorrupted;
    self.dedupeAvailable_ = !dedupeCorrupted;
    self.activeJobCorrupted_ = activeJobCorrupted;

    return self;
}

// ---------------------------------------------------------------------------
// migrateAndValidate - 对单个 key 执行格式检查 / 迁移 / 校验
// corruptionFlag 语义因 key 而异：
//   sequence/active_job: true = 损坏（阻断/保守重采集）
//   last_success/dedupe: true = 损坏（不可用），由 open() 反转为 available 标志
// ---------------------------------------------------------------------------
template <typename StateT>
void FotaStateStore::migrateAndValidate(const char* key, std::uint32_t currentVersion,
                                        StateT (*decodeFn)(const std::string&),
                                        std::string (*encodeFn)(const StateT&),
                                        bool& corruptionFlag) {
    bool exists = false;
    try {
        exists = store_.has(key);
    } catch (const ccu::fw::store::StoreException& e) {
        // 0101/0102 路径/IO 错误 -> 损坏
        corruptionFlag = true;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, key, e.code, "has");
        return;
    }
    if (!exists) return; // 缺失 -> 各 load 方法按缺失/首次启动处理

    std::string env;
    try {
        env = store_.load<std::string>(key);
    } catch (const ccu::fw::store::StoreException& e) {
        // 0104 type-mismatch（非 string），0101/0102 IO
        corruptionFlag = true;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, key, e.code, "load");
        return;
    }

    EnvelopeHeader h;
    try {
        h = parseEnvelopeHeader(env);
    } catch (const StateDecodeError& e) {
        // envelope 损坏（截断/错 magic/错长度/minReader 过高）
        corruptionFlag = true;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, key, "CCU-FW-0104", "envelope");
        return;
    }

    if (h.schemaVersion > currentVersion) {
        // 未知更新版本 -> fail-closed，不覆盖、不降级
        corruptionFlag = true;
        FotaLogAdapter::store().error(
            fota_events::STORE_MIGRATION_FAILED,
            "Unknown newer schema version, fail-closed",
            {flog::f_str("key", key),
             flog::f_int("format_version", static_cast<std::int64_t>(h.schemaVersion))});
        return;
    }

    if (h.schemaVersion == currentVersion) {
        // 当前版本：完整解码 + 业务不变量校验
        try {
            decodeFn(env);
        } catch (const StateDecodeError& e) {
            corruptionFlag = true;
            logStoreError(fota_events::STORE_MIGRATION_FAILED, key, "CCU-FW-0104", "validate");
        }
        return;
    }

    // h.schemaVersion < currentVersion：迁移
    // 1. 保存迁移备份（至少保留一个可恢复版本）
    std::string backupKey = std::string("migration.backup.") + key +
                            ".v" + std::to_string(h.schemaVersion);
    try {
        store_.save<std::string>(backupKey, env);
    } catch (const ccu::fw::store::StoreException& e) {
        // 备份失败 -> 不迁移原 key，保留旧可恢复版本
        corruptionFlag = true;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, key, e.code, "backup");
        return;
    }

    // 2. 解码（内部迁移至当前版本）-> 3. 原子保存新格式
    try {
        StateT s = decodeFn(env);
        store_.save<std::string>(key, encodeFn(s));
    } catch (const StateDecodeError& e) {
        corruptionFlag = true;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, key, "CCU-FW-0104", "migrate");
        return;
    } catch (const ccu::fw::store::StoreException& e) {
        corruptionFlag = true;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, key, e.code, "rewrite");
        return;
    }

    // 4. 重新读取并验证（幂等：重复执行得相同结果）
    try {
        decodeFn(store_.load<std::string>(key));
    } catch (const std::exception&) {
        // 迁移后验证失败：原 key 已被覆盖，但备份仍保留可恢复版本
        corruptionFlag = true;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, key, "CCU-FW-0104", "verify");
        return;
    }

    FotaLogAdapter::store().info(
        fota_events::STORE_MIGRATION_FAILED,
        "Key migrated to current schema",
        {flog::f_str("key", key),
         flog::f_int("from_version", static_cast<std::int64_t>(h.schemaVersion)),
         flog::f_int("to_version", static_cast<std::int64_t>(currentVersion))});
}

// ===========================================================================
// 序号分配
// ===========================================================================
std::uint64_t FotaStateStore::allocateSnapshotSeq() {
    if (sequenceBlocked_) {
        FotaLogAdapter::store().error(
            fota_events::STORE_SEQ_BLOCKED,
            "Sequence allocation blocked (corruption/overflow)",
            {flog::f_str("key", keys::SEQUENCE)});
        throw SeqAllocBlocked("sequence blocked", "STATE_BLOCKED");
    }

    bool exists = false;
    SequenceState cur;
    cur.highestAllocated = 0;
    cur.updatedAt = 0;

    try {
        exists = store_.has(keys::SEQUENCE);
        if (exists) {
            cur = decodeSequence(store_.load<std::string>(keys::SEQUENCE));
        }
    } catch (const ccu::fw::store::StoreException& e) {
        sequenceBlocked_ = true;
        logStoreError(fota_events::STORE_SEQUENCE_FAILED, keys::SEQUENCE, e.code, "load");
        throw SeqAllocBlocked("sequence load failed", e.code);
    } catch (const StateDecodeError& e) {
        // 已部署设备序号损坏 -> 阻断，禁止自动归零
        sequenceBlocked_ = true;
        logStoreError(fota_events::STORE_SEQUENCE_FAILED, keys::SEQUENCE, "CCU-FW-0104", "decode");
        throw SeqAllocBlocked("sequence corrupted", "CCU-FW-0104");
    }

    // 首次初始化必须由“无既有部署状态”的明确条件判定（exists == false）。
    // exists == true 但解码失败已在上方阻断。

    // 溢出检查
    if (cur.highestAllocated >= SNAPSHOT_SEQ_MAX) {
        sequenceBlocked_ = true;
        FotaLogAdapter::store().error(
            fota_events::STORE_SEQ_BLOCKED,
            "Sequence overflow",
            {flog::f_str("key", keys::SEQUENCE),
             flog::f_int("highest_allocated", static_cast<std::int64_t>(cur.highestAllocated))});
        throw SeqAllocBlocked("sequence overflow", "OVERFLOW");
    }

    std::uint64_t next = cur.highestAllocated + 1;
    SequenceState newState;
    newState.schemaVersion = schema::SEQUENCE_VERSION;
    newState.highestAllocated = next;
    newState.updatedAt = nowMs();

    try {
        // save() 完成 temp->fsync->rename->dir fsync 后才返回（同步 durable）。
        store_.save<std::string>(keys::SEQUENCE, encodeSequence(newState));
    } catch (const ccu::fw::store::StoreException& e) {
        // 写入失败：序号未持久化，不得使用 -> 阻断
        sequenceBlocked_ = true;
        logStoreError(fota_events::STORE_SEQUENCE_FAILED, keys::SEQUENCE, e.code, "save");
        throw SeqAllocBlocked("sequence save failed", e.code);
    }

    FotaLogAdapter::store().info(
        fota_events::STORE_SEQ_ALLOCATED,
        "Snapshot sequence allocated",
        {flog::f_str("key", keys::SEQUENCE),
         flog::f_int("snapshot_seq", static_cast<std::int64_t>(next))});
    return next;
}

std::optional<SequenceState> FotaStateStore::loadSequence() {
    if (sequenceBlocked_) return std::nullopt;
    try {
        if (!store_.has(keys::SEQUENCE)) return std::nullopt;
        return decodeSequence(store_.load<std::string>(keys::SEQUENCE));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// ===========================================================================
// 最近成功快照
// ===========================================================================
std::optional<LastSuccessState> FotaStateStore::loadLastSuccess() {
    if (!lastSuccessAvailable_) return std::nullopt;
    try {
        if (!store_.has(keys::LAST_SUCCESS)) return std::nullopt;
        return decodeLastSuccess(store_.load<std::string>(keys::LAST_SUCCESS));
    } catch (const ccu::fw::store::StoreException& e) {
        // 损坏 -> 标记不可用，保留文件供诊断
        lastSuccessAvailable_ = false;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::LAST_SUCCESS, e.code, "load");
        return std::nullopt;
    } catch (const StateDecodeError&) {
        lastSuccessAvailable_ = false;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::LAST_SUCCESS, "CCU-FW-0104", "decode");
        return std::nullopt;
    }
}

void FotaStateStore::saveLastSuccess(const LastSuccessState& s) {
    try {
        store_.save<std::string>(keys::LAST_SUCCESS, encodeLastSuccess(s));
        lastSuccessAvailable_ = true;
    } catch (const ccu::fw::store::StoreException& e) {
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::LAST_SUCCESS, e.code, "save");
        throw;
    }
}

// ===========================================================================
// 去重窗口
// ===========================================================================
DedupeState FotaStateStore::loadDedupe() {
    if (!dedupeAvailable_) {
        DedupeState empty;
        empty.maxEntries = dedupeMaxEntries_;
        empty.ttlMs = dedupeTtlMs_;
        return empty;
    }
    try {
        if (!store_.has(keys::DEDUPE)) {
            DedupeState empty;
            empty.maxEntries = dedupeMaxEntries_;
            empty.ttlMs = dedupeTtlMs_;
            return empty;
        }
        DedupeState d = decodeDedupe(store_.load<std::string>(keys::DEDUPE));
        // 确保配置边界生效
        d.maxEntries = dedupeMaxEntries_;
        d.ttlMs = dedupeTtlMs_;
        return d;
    } catch (const ccu::fw::store::StoreException& e) {
        dedupeAvailable_ = false;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::DEDUPE, e.code, "load");
        DedupeState empty;
        empty.maxEntries = dedupeMaxEntries_;
        empty.ttlMs = dedupeTtlMs_;
        return empty;
    } catch (const StateDecodeError&) {
        dedupeAvailable_ = false;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::DEDUPE, "CCU-FW-0104", "decode");
        DedupeState empty;
        empty.maxEntries = dedupeMaxEntries_;
        empty.ttlMs = dedupeTtlMs_;
        return empty;
    }
}

void FotaStateStore::saveDedupe(const DedupeState& s) {
    try {
        store_.save<std::string>(keys::DEDUPE, encodeDedupe(s));
        dedupeAvailable_ = true;
    } catch (const ccu::fw::store::StoreException& e) {
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::DEDUPE, e.code, "save");
        throw;
    }
}

// ===========================================================================
// 在途/重试检查点
// ===========================================================================
std::optional<ActiveJobState> FotaStateStore::loadActiveJob() {
    // 损坏时返回 nullopt（不得恢复为成功）；isActiveJobCorrupted() 反映损坏状态。
    try {
        if (!store_.has(keys::ACTIVE_JOB)) return std::nullopt;
        return decodeActiveJob(store_.load<std::string>(keys::ACTIVE_JOB));
    } catch (const ccu::fw::store::StoreException& e) {
        activeJobCorrupted_ = true;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::ACTIVE_JOB, e.code, "load");
        return std::nullopt;
    } catch (const StateDecodeError&) {
        activeJobCorrupted_ = true;
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::ACTIVE_JOB, "CCU-FW-0104", "decode");
        return std::nullopt;
    }
}

void FotaStateStore::saveActiveJob(const ActiveJobState& s) {
    try {
        store_.save<std::string>(keys::ACTIVE_JOB, encodeActiveJob(s));
    } catch (const ccu::fw::store::StoreException& e) {
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::ACTIVE_JOB, e.code, "save");
        throw;
    }
}

void FotaStateStore::removeActiveJob() {
    try {
        store_.remove(keys::ACTIVE_JOB);
    } catch (const ccu::fw::store::StoreException& e) {
        logStoreError(fota_events::STORE_MIGRATION_FAILED, keys::ACTIVE_JOB, e.code, "remove");
        throw;
    }
}

} // namespace store
} // namespace ccu_fota
