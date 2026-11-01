# cmake/FotaProtobuf.cmake - CCU-FOTA-DSN-CR-011 §Protobuf SSOT 与生成
#
# CCU-FOTA 仓库内冻结的 vehicle.common.v1 / vehicle.fota.v1 proto 契约，
# 与 VEH-PROTO 统一协议仓库逐项一致（迁移步骤1，一次性替换旧 ota 契约）。
# 使用 proto3；protoc 生成 C++；CI 后续可扩展跨语言生成与 breaking-change 检查。
#
# 约束：
#   * 仅消费系统/SDK 提供的 Protobuf + protoc；不从源码构建。
#   * 生成产物落入 ${CMAKE_BINARY_DIR}/gen/proto，作为 PRIVATE 生成源接入内部 lib，
#     不安装、不导出。
#   * proto3 optional 需要 protoc >= 3.12 且传 --experimental_allow_proto3_optional。

find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    # 无 protobuf-config.cmake（旧式安装/部分发行版）时回退 module 模式。
    # 优先 CONFIG 模式可让现代 protobuf（含 absl 传递依赖）正确传递链接库。
    find_package(Protobuf REQUIRED)
endif()

# protoc 可执行：优先 import 的目标，否则回退到变量。
if(TARGET protobuf::protoc)
    set(FOTA_PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)
elseif(DEFINED Protobuf_PROTOC_EXECUTABLE)
    set(FOTA_PROTOC_EXECUTABLE ${Protobuf_PROTOC_EXECUTABLE})
else()
    set(FOTA_PROTOC_EXECUTABLE protoc)
endif()

set(FOTA_PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(FOTA_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen/proto")

# FOTA proto 源（顺序无关；protoc 自行解析 import）。
set(FOTA_PROTO_FILES
    "${FOTA_PROTO_ROOT}/vehicle/common/v1/envelope.proto"
    "${FOTA_PROTO_ROOT}/vehicle/fota/v1/types.proto"
    "${FOTA_PROTO_ROOT}/vehicle/fota/v1/task.proto"
    "${FOTA_PROTO_ROOT}/vehicle/fota/v1/consent.proto"
    "${FOTA_PROTO_ROOT}/vehicle/fota/v1/package.proto"
    "${FOTA_PROTO_ROOT}/vehicle/fota/v1/execution.proto"
    "${FOTA_PROTO_ROOT}/vehicle/fota/v1/log.proto"
    "${FOTA_PROTO_ROOT}/vehicle/fota/v1/reconcile.proto"
    "${FOTA_PROTO_ROOT}/vehicle/fota/v1/policy.proto"
)

# 推导每个 .proto 对应的生成文件路径（.pb.cc / .pb.h）。
set(FOTA_PROTO_GENERATED_SOURCES "")
set(FOTA_PROTO_GENERATED_HEADERS "")
foreach(_pf ${FOTA_PROTO_FILES})
    file(RELATIVE_PATH _rel "${FOTA_PROTO_ROOT}" "${_pf}")
    string(REGEX REPLACE "\\.proto$" ".pb.cc" _cc "${_rel}")
    string(REGEX REPLACE "\\.proto$" ".pb.h"  _hh "${_rel}")
    list(APPEND FOTA_PROTO_GENERATED_SOURCES "${FOTA_PROTO_GEN_DIR}/${_cc}")
    list(APPEND FOTA_PROTO_GENERATED_HEADERS  "${FOTA_PROTO_GEN_DIR}/${_hh}")
endforeach()

# 单一 custom command 生成全部 proto（避免 protoc 多次启动与竞态）。
add_custom_command(
    OUTPUT ${FOTA_PROTO_GENERATED_SOURCES} ${FOTA_PROTO_GENERATED_HEADERS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FOTA_PROTO_GEN_DIR}"
    COMMAND ${FOTA_PROTOC_EXECUTABLE}
            --proto_path=${FOTA_PROTO_ROOT}
            --cpp_out=${FOTA_PROTO_GEN_DIR}
            --experimental_allow_proto3_optional
            ${FOTA_PROTO_FILES}
    DEPENDS ${FOTA_PROTO_FILES}
    COMMENT "Generating C++ from vehicle.* proto (CCU-FOTA-DSN-CR-011)"
    VERBATIM)

# 内部生成库（不安装、不导出）。daemon 与 tests 通过 PRIVATE 链接消费。
add_library(ccu_fota_proto STATIC ${FOTA_PROTO_GENERATED_SOURCES})
target_include_directories(ccu_fota_proto PUBLIC "${FOTA_PROTO_GEN_DIR}")
target_compile_features(ccu_fota_proto PUBLIC cxx_std_20)
target_link_libraries(ccu_fota_proto PUBLIC protobuf::libprotobuf)

# 让 protoc 生成的 .pb.h 在 IDE 中可见。
add_custom_target(ccu_fota_proto_headers DEPENDS ${FOTA_PROTO_GENERATED_HEADERS})
