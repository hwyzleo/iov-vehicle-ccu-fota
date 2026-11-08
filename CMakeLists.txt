# ============================================================================
# CCU-FOTA - CMake build (CCU-FOTA-DSN-CR-008)
#
# CR-008 §12 contract:
#   * cmake >= 3.24, project versioned, LANGUAGES CXX, cxx_std_20.
#   * No compiler / sysroot / target-triple / host absolute path set here.
#   * Consume installed Framework SDK package only (find_package + imported
#     targets). No add_subdirectory(CCU-FW), no source/private include, no
#     build-tree library, no link_directories, no hand-written archive path.
#   * GNUInstallDirs; standard configure->build->install with PREFIX=/usr and
#     DESTDIR. Production target fixed as ccu-fota.
#   * ccu-fota-runtime component installs daemon + systemd unit + default
#     config + schema only. Internal lib / tests / dev fixtures never install.
#   * Tests are independent, non-installed HOST targets.
# ============================================================================
cmake_minimum_required(VERSION 3.24)
project(CcuFota VERSION 1.0 LANGUAGES CXX)

include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(FotaOptions)
include(FotaWarnings)
include(FotaProtobuf)

# ----------------------------------------------------------------------------
# CR-008 §12.2: consume installed Framework SDK package only.
# Exact package/target names follow the CCU-FW installed export, which is
# find_package(CCUFramework) + CCUFramework::ccu-framework-*. The CR text's
# ccu:: namespace is illustrative; the installed export is authoritative.
# ----------------------------------------------------------------------------
find_package(CCUFramework CONFIG REQUIRED)

# nlohmann_json: direct TARGET dependency (store serializer). Controlled
# package resolution only; protobuf is NOT directly used -> not added
# (CR-008: "仅直接使用时").
find_package(nlohmann_json CONFIG REQUIRED)

# ----------------------------------------------------------------------------
# Sources
# ----------------------------------------------------------------------------
set(FOTA_SOURCES
    src/error_codes.cpp
    src/data_models.cpp
    src/fingerprint/canonical_encoder.cpp
    src/fingerprint/inventory_fingerprint.cpp
    src/config/fota_config.cpp
    src/store/fota_state.cpp
    src/store/fota_state_serializer.cpp
    src/store/fota_state_store.cpp
    src/store/fota_state_recovery.cpp
    src/store/fota_cloud_state_serializer.cpp
    src/store/fota_cloud_state_store.cpp
    src/store/fota_state_migration.cpp
    src/snapshot_assembler.cpp
    src/someip/diag_inventory_client.cpp
    src/someip/diag_inventory_codec.cpp
    src/someip/diag_installed_version_registrar.cpp
    src/someip/error_mapper.cpp
    src/someip/tbox_generic_transport_contract.cpp
    src/fota_log_adapter.cpp
    src/fota_log_context.cpp
    src/ota/state/vehicle_task_state.cpp
    src/ota/state/execution_state.cpp
    src/ota/state/state_proto.cpp
    src/ota/fota_orchestrator.cpp
    src/ota/inventory_identity_planner.cpp
    src/ota/fota_cloud_proxy_via_transport.cpp
    src/ota/someip_vehicle_message_transport.cpp
    src/ota/production_ports.cpp
    src/daemon/fota_daemon.cpp
)

# Internal object collection (NOT installed / NOT exported). Lets the daemon
# and HOST tests share compiled objects without exposing a public target.
add_library(ccu_fota_lib STATIC ${FOTA_SOURCES})
target_include_directories(ccu_fota_lib
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
target_compile_features(ccu_fota_lib PUBLIC cxx_std_20)
target_link_libraries(ccu_fota_lib
    PUBLIC
        CCUFramework::ccu-framework-config
        CCUFramework::ccu-framework-log
        CCUFramework::ccu-framework-store
        CCUFramework::ccu-framework-hash
        CCUFramework::ccu-framework-someip
        nlohmann_json::nlohmann_json
        ccu_fota_proto)
fota_apply_warnings(ccu_fota_lib)

# ----------------------------------------------------------------------------
# Production daemon target (CR-008: fixed name ccu-fota).
# CR-009: daemon NEVER enables test doubles; 量产 preset 必须为 OFF。
# ----------------------------------------------------------------------------
add_executable(ccu-fota src/main.cpp)
target_link_libraries(ccu-fota PRIVATE ccu_fota_lib)
target_compile_features(ccu-fota PRIVATE cxx_std_20)
fota_apply_warnings(ccu-fota)

# ----------------------------------------------------------------------------
# Tests: independent, non-installed HOST targets (CR-008 §12.3).
# HOST self-verification may use the HOST compiler but still consumes the
# installed framework package; HOST success does not replace TARGET
# architecture/ABI/link verification.
# ----------------------------------------------------------------------------
if(FOTA_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)

    add_executable(CcuFotaTests
        tests/test_data_models.cpp
        tests/test_snapshot_assembler.cpp
        tests/test_fota_config.cpp
        tests/test_config_load.cpp
        tests/test_log_adapter.cpp
        tests/test_log_events.cpp
        tests/test_log_integration.cpp
        tests/test_fota_state_serializer.cpp
        tests/test_fota_state_store.cpp
        tests/test_fota_state_recovery.cpp
        tests/test_store_integration.cpp
        tests/test_canonical_encoder.cpp
        tests/test_inventory_fingerprint.cpp
        tests/test_fingerprint_store_migration.cpp
        tests/someip/error_mapper_test.cpp
        tests/someip/registry_contract_test.cpp
        tests/someip/clients_test.cpp
        tests/someip/diag_inventory_codec_test.cpp
        tests/someip/diag_installed_version_registrar_test.cpp
        tests/someip/tbox_generic_transport_contract_test.cpp
        tests/ota/state_machine_test.cpp
        tests/ota/fota_cloud_state_store_test.cpp
        tests/ota/fota_state_migration_test.cpp
        tests/ota/ota_integration_test.cpp
        tests/ota/transport_contract_test.cpp
        tests/ota/someip_transport_contract_test.cpp
        tests/ota/daemon_assembly_test.cpp
        tests/ota/inventory_identity_planner_test.cpp
        tests/ota/inventory_revision_test.cpp
        tests/ota/inventory_provider_test.cpp
        tests/ota/installed_version_registration_test.cpp
        tests/ota/diag_inventory_end_to_end_test.cpp
        # Mock scenario 仅测试 target 编译（NON_PRODUCTION，CR-009 §构建与运行隔离）
        src/ota/mock/scenario_script.cpp
    )
    target_link_libraries(CcuFotaTests
        PRIVATE
            ccu_fota_lib
            ccu_fota_proto
            GTest::GTest
            GTest::gmock
            GTest::Main)
    # CR-009: tests compile with test doubles enabled (NON_PRODUCTION profile).
    target_compile_definitions(CcuFotaTests PRIVATE FOTA_ENABLE_TEST_DOUBLES=1)
    fota_apply_warnings(CcuFotaTests)
    add_test(NAME CcuFotaTests COMMAND CcuFotaTests)
endif()

# ============================================================================
# Install: ccu-fota-runtime component (CR-008 §12.3).
# Supports CMAKE_INSTALL_PREFIX=/usr and DESTDIR=<install-root>. No
# install-time network/device access, no absolute staging write, no
# post-install service start/stop, no globbing whole config/ or tests/.
# Dev fixtures (config/fota.yaml, config/common.yaml) are NOT installed.
# ============================================================================
install(TARGETS ccu-fota
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT ccu-fota-runtime)

install(FILES packaging/systemd/ccu-fota.service
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
    COMPONENT ccu-fota-runtime)

# NOTE: config uses the FULL (absolute) SYSCONFDIR so that with
# CMAKE_INSTALL_PREFIX=/usr it lands at /etc (FHS), matching CR-008 §12.3
# staging (etc at install-root root) and main.cpp's runtime default
# configRoots="/etc/ccu". The raw relative var would re-root to /usr/etc.
install(FILES config/fota.default.yaml
    DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/ccu/conf.d
    RENAME fota.yaml
    COMPONENT ccu-fota-runtime)

install(FILES config/schema/fota.schema.yaml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ccu-fota/schema
    COMPONENT ccu-fota-runtime)
