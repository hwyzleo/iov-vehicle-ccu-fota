#include "error_codes.h"

namespace ccu_fota {

std::string errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::SUCCESS:
            return "Success";
        case ErrorCode::CCU_DIAG_1001:
            return "ECU 不可达";
        case ErrorCode::CCU_DIAG_1002:
            return "诊断超时";
        case ErrorCode::CCU_DIAG_1003:
            return "NRC 响应";
        case ErrorCode::CCU_DIAG_1004:
            return "DID 缺失";
        case ErrorCode::CCU_DIAG_1005:
            return "解析失败";
        case ErrorCode::CCU_DIAG_1006:
            return "未知错误";
        case ErrorCode::CCU_FOTA_1003:
            return "清单组装失败";
        case ErrorCode::CCU_FOTA_1004:
            return "车内发送失败";
        case ErrorCode::CCU_FOTA_1005:
            return "TBOX 不可达";
        case ErrorCode::CCU_FOTA_1006:
            return "上报超时";
        case ErrorCode::CONFIG_ERROR:
            return "配置错误";
        case ErrorCode::SOMEIP_CONNECTION_ERROR:
            return "SOME/IP 连接错误";
        case ErrorCode::SOMEIP_TIMEOUT_ERROR:
            return "SOME/IP 超时错误";
        case ErrorCode::SOMEIP_PROTOCOL_ERROR:
            return "SOME/IP 协议错误";
        default:
            return "未知错误";
    }
}

bool isSuccess(ErrorCode code) {
    return code == ErrorCode::SUCCESS;
}

} // namespace ccu_fota
