// =============================================================================
// src/someip/error_mapper.cpp - CCU-FOTA SOME/IP 错误映射实现 (CCU-FOTA-DSN-CR-007)
// =============================================================================

#include "ccu/fota/someip/error_mapper.hpp"

#include <algorithm>
#include <string>

namespace ccu_fota {
namespace someip {

namespace {

// 从 "CCU-FW-0305" / "SOMEIP/0x05" / "0305" 中提取规范 4 位数字串。
std::string normalizeCode(const std::string& code) {
    // 优先匹配 "CCU-FW-NNNN" 形式
    const std::string prefix = "CCU-FW-";
    auto pos = code.find(prefix);
    if (pos != std::string::npos) {
        std::string rest = code.substr(pos + prefix.size());
        // 取连续数字
        std::string num;
        for (char c : rest) {
            if (c >= '0' && c <= '9') num += c; else break;
        }
        while (num.size() < 4) num = "0" + num;
        if (num.size() == 4) return num;
    }
    // 兜底：取首个连续 4 位数字串
    std::string num;
    for (char c : code) {
        if (c >= '0' && c <= '9') num += c;
        else if (num.size() >= 4) break;
        else if (!num.empty()) num.clear();
    }
    while (num.size() < 4 && !num.empty()) num = "0" + num;
    return num.size() == 4 ? num : code;
}

} // namespace

std::string frameworkErrorNumber(const std::string& code) {
    return normalizeCode(code);
}

MappedError mapDiagCallError(const ccu::fw::someip::CallResult& result) {
    MappedError e;
    e.cause = result.code;
    e.message = result.message;
    std::string num = normalizeCode(result.code);

    if (num == "0304") {
        // DIAG 不可用/版本不匹配：采集失败，保留原因链
        e.code = ErrorCode::CCU_FOTA_1006;
    } else if (num == "0305") {
        // DIAG timeout/transport：采集超时
        e.code = ErrorCode::CCU_FOTA_1006;
    } else if (num == "0306") {
        // payload/codec：解析失败
        e.code = ErrorCode::CCU_DIAG_1005;
    } else if (num == "0303") {
        // discovery/offer/request 失败：采集失败
        e.code = ErrorCode::CCU_FOTA_1006;
    } else if (num == "0307" || num == "0309" || num == "0310") {
        e.code = ErrorCode::CCU_DIAG_1006;
    } else {
        // 未知 framework 错误：通用采集失败
        e.code = ErrorCode::CCU_DIAG_1006;
    }
    return e;
}

MappedError mapTboxCallError(const ccu::fw::someip::CallResult& result) {
    MappedError e;
    e.cause = result.code;
    e.message = result.message;
    std::string num = normalizeCode(result.code);

    if (num == "0304") {
        // TBOX 不可用/版本不匹配：TBOX 不可达
        e.code = ErrorCode::CCU_FOTA_1005;
    } else if (num == "0306") {
        // payload/codec/size：快照编码失败
        e.code = ErrorCode::CCU_FOTA_1003;
    } else if (num == "0305") {
        // TBOX timeout：上报超时
        e.code = ErrorCode::CCU_FOTA_1006;
    } else if (num == "0303") {
        // send/transport failure：车内发送失败，结果未知
        e.code = ErrorCode::CCU_FOTA_1004;
    } else if (num == "0307" || num == "0309" || num == "0310") {
        // handler/response/resource/shutdown：车内发送失败
        e.code = ErrorCode::CCU_FOTA_1004;
    } else {
        // 未知 framework 错误：车内发送失败
        e.code = ErrorCode::CCU_FOTA_1004;
    }
    return e;
}

bool isStartupBlocking(const std::string& frameworkCode) {
    std::string num = normalizeCode(frameworkCode);
    return num == "0301" || num == "0302";
}

bool isResourceLimit(const std::string& frameworkCode) {
    return normalizeCode(frameworkCode) == "0309";
}

} // namespace someip
} // namespace ccu_fota
