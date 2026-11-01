#pragma once

// =============================================================================
// include/ccu/fota/store/fota_state_serializer.hpp
// CCU-FOTA 状态序列化器 (CCU-FOTA-DSN-CR-005)
// =============================================================================
// 将 fota_state.hpp 中的状态结构序列化为版本化 FOTA envelope，供
// ccu-framework-store 以 std::string 持久化。框架 store 提供外层二进制
// envelope（magic / format-version / type-tag / length）；FOTA 在 payload 内
// 定义业务级 schemaVersion 与显式字段，不保存 C++ 内存布局。
//
// FOTA envelope 布局（小端）：
//   [4]  magic          "FTSE"
//   [4]  schemaVersion  业务 schema 版本（驱动迁移）
//   [4]  writerVersion  写入方二进制版本
//   [4]  minReaderVersion 可读最低版本
//   [8]  writtenAt      写入时间 (ms since epoch)
//   [4]  payloadLen     payload 字节数
//   [N]  payload        JSON（显式字段）
//
// 完整性元数据：本 CR 不新增 SHA-256（指纹算法由后续 hash CR 固化）；完整性由
// magic 校验、长度校验、minReaderVersion 校验与 payload 字段校验共同保证。
//
// 迁移：读取 envelope schemaVersion；若低于当前版本，按 vN->vN+1 链式迁移 payload
// 至当前版本；未知更新版本或 minReaderVersion 超出能力时 fail-closed（抛异常）。
// 迁移函数为纯转换、无外部副作用，保证重复迁移幂等。
// =============================================================================

#include "ccu/fota/store/fota_state.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace ccu_fota {
namespace store {

// ---------------------------------------------------------------------------
// StateDecodeError - 解码/迁移/校验失败（映射为 CCU-FW-0104）
// message 仅含 key/stage/version 摘要，不含快照、VIN、device_sn 或原始字节。
// ---------------------------------------------------------------------------
class StateDecodeError : public std::runtime_error {
public:
    explicit StateDecodeError(const std::string& message);
};

// ---------------------------------------------------------------------------
// Envelope 常量
// ---------------------------------------------------------------------------
namespace envelope {
constexpr char MAGIC[4] = {'F', 'T', 'S', 'E'};
constexpr std::size_t HEADER_SIZE = 4 + 4 + 4 + 4 + 8 + 4; // 28 bytes
} // namespace envelope

// ---------------------------------------------------------------------------
// Envelope 解析结果
// ---------------------------------------------------------------------------
struct EnvelopeHeader {
    std::uint32_t schemaVersion = 0;
    std::uint32_t writerVersion = 0;
    std::uint32_t minReaderVersion = 0;
    std::int64_t  writtenAt = 0;
    std::uint32_t payloadLen = 0;
};

// 解析 envelope 头部并校验 magic / 长度 / minReaderVersion / 未知更新版本。
// 不消费 payload。schemaVersion 低于当前版本时不抛（由调用方迁移）。
EnvelopeHeader parseEnvelopeHeader(const std::string& bytes);

// 提取 payload（调用前应已 parseEnvelopeHeader 成功）。
std::string extractPayload(const std::string& bytes, const EnvelopeHeader& hdr);

// 构造 envelope 字节（struct -> payload JSON -> envelope）。
std::string encodeEnvelope(std::uint32_t schemaVersion, const std::string& payloadJson);

// ---------------------------------------------------------------------------
// 每类状态的 encode（struct -> envelope string）
// ---------------------------------------------------------------------------
std::string encodeSequence(const SequenceState& s);
std::string encodeLastSuccess(const LastSuccessState& s);
std::string encodeDedupe(const DedupeState& s);
std::string encodeActiveJob(const ActiveJobState& s);

// ---------------------------------------------------------------------------
// 每类状态的 decode（envelope string -> struct），含迁移与校验
//   - magic / 长度 / minReaderVersion / 未知新版本失败 -> StateDecodeError
//   - 旧版本经迁移链升至当前版本后完整解码并执行业务不变量校验
// ---------------------------------------------------------------------------
SequenceState    decodeSequence(const std::string& bytes);
LastSuccessState decodeLastSuccess(const std::string& bytes);
DedupeState      decodeDedupe(const std::string& bytes);
ActiveJobState   decodeActiveJob(const std::string& bytes);

// ---------------------------------------------------------------------------
// 迁移：将 payload JSON 从 fromVersion 迁移到 fromVersion+1（纯转换）。
// 迁移到当前版本需调用方按链重复调用。供测试与 store 迁移器使用。
// 若 fromVersion 已是当前版本，原样返回。
// ---------------------------------------------------------------------------
std::string migrateSequencePayload(const std::string& payloadJson,
                                   std::uint32_t fromVersion);
std::string migrateLastSuccessPayload(const std::string& payloadJson,
                                      std::uint32_t fromVersion);
std::string migrateDedupePayload(const std::string& payloadJson,
                                 std::uint32_t fromVersion);
std::string migrateActiveJobPayload(const std::string& payloadJson,
                                    std::uint32_t fromVersion);

} // namespace store
} // namespace ccu_fota
