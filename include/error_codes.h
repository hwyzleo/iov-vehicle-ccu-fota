#pragma once

#include <string>
#include <cstdint>

namespace ccu_fota {

enum class ErrorCode : uint16_t {
    // Success
    SUCCESS = 0,

    // CCU-DIAG errors (1001-1006)
    CCU_DIAG_1001 = 1001,  // ECU 不可达
    CCU_DIAG_1002 = 1002,  // 诊断超时
    CCU_DIAG_1003 = 1003,  // NRC 响应
    CCU_DIAG_1004 = 1004,  // DID 缺失
    CCU_DIAG_1005 = 1005,  // 解析失败
    CCU_DIAG_1006 = 1006,  // 未知错误

    // CCU-FOTA specific errors (1101-1104)
    CCU_FOTA_1003 = 1103,  // 清单组装失败
    CCU_FOTA_1004 = 1104,  // 车内发送失败
    CCU_FOTA_1005 = 1105,  // TBOX 不可达
    CCU_FOTA_1006 = 1106,  // 上报超时

    // Configuration errors
    CONFIG_ERROR = 2001,

    // SOME/IP errors
    SOMEIP_CONNECTION_ERROR = 3001,
    SOMEIP_TIMEOUT_ERROR = 3002,
    SOMEIP_PROTOCOL_ERROR = 3003
};

std::string errorCodeToString(ErrorCode code);
bool isSuccess(ErrorCode code);

} // namespace ccu_fota
