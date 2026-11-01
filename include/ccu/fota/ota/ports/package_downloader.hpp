#pragma once

// =============================================================================
// include/ccu/fota/ota/ports/package_downloader.hpp
// CCU-FOTA 包下载器端口 (CCU-FOTA-DSN-CR-009 §下载 / CR-011 类型校准)
// =============================================================================
// 为每个 package 持久化下载上下文（由 orchestrator 管理），再申请短期凭证。
// 下载器负责数据面 I/O + 校验，产出单条 StageResultReport（含 Result 与
// hash/signature/decryption 校验标志）。MockDownloader 不访问真实 CDN，按 size
// 分块生成确定字节流，模拟 Range、断网、ETag 变化、摘要/签名/解密结果。
// 下载与校验终态以 stageResultId+stageResultDigest 独立上报。
// =============================================================================

#include "vehicle/fota/v1/package.pb.h"
#include "vehicle/fota/v1/types.pb.h"

#include <cstdint>
#include <string>

namespace ccu_fota {
namespace ota {

// 单包下载+校验结果。
struct DownloadOutcome {
    bool allStagesSucceeded = false;
    ::vehicle::fota::v1::StageResultReport stageResult;  // 单条阶段结果（可选）
    bool hasStageResult = false;
    std::uint64_t bytesDownloaded = 0;
    std::uint64_t finalOffsetBytes = 0;    // STORED_OBJECT 字节偏移（续传）
    std::string errorCode;
    std::string errorDetail;
};

class PackageDownloader {
public:
    virtual ~PackageDownloader() = default;

    // 下载并校验单个包，从 fromOffsetBytes 续传。grant 提供凭证/URL/etag/digest/signature。
    // 实现负责按 size 分块、Range 续传、摘要/签名/证书/解密校验，并产出阶段结果。
    virtual DownloadOutcome
    downloadAndVerify(const ::vehicle::fota::v1::PackageSummary& pkg,
                      const ::vehicle::fota::v1::DownloadGrantResponse& grant,
                      std::uint64_t fromOffsetBytes) = 0;
};

} // namespace ota
} // namespace ccu_fota
