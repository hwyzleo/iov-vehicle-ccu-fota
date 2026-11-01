#!/bin/bash
# ci/verify-cmake-lint.sh - CCU-FOTA-DSN-CR-008 §12.2/§12.3/§12.6
#
# Lint the FOTA CMakeLists.txt against the CR-008 build contract:
#   * cmake >= 3.24, project versioned with LANGUAGES CXX
#   * cxx_std_20 via target_compile_features (no global CMAKE_CXX_FLAGS mutation)
#   * no compiler / sysroot / target-triple / host absolute path set here
#   * consume installed Framework SDK only (find_package + imported targets)
#   * no add_subdirectory(CCU-FW) / source framework include / link_directories
#     / hand-written archive / build-tree package
#   * production target fixed as ccu-fota; ccu-fota-runtime install component
#   * GNUInstallDirs used
#
# Exit: 0 = pass, 1 = violation found.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CML="${ROOT}/CMakeLists.txt"
STATUS=0

violation() { echo "[FAIL] $1" >&2; STATUS=1; }
ok()        { echo "[OK]   $1"; }

[ -f "$CML" ] || { echo "[FAIL] CMakeLists.txt not found" >&2; exit 1; }

# --- required positives ---
grep -qE '^cmake_minimum_required\(VERSION 3\.2[4-9]' "$CML" \
    && ok "cmake_minimum_required >= 3.24" || violation "cmake_minimum_required < 3.24"
grep -qE '^project\(CcuFota VERSION [0-9]+.*LANGUAGES CXX\)' "$CML" \
    && ok "project versioned with LANGUAGES CXX" || violation "project() not versioned/typed"
grep -q 'target_compile_features.*cxx_std_20' "$CML" \
    && ok "cxx_std_20 via target_compile_features" || violation "cxx_std_20 not set via target_compile_features"
grep -q 'find_package(CCUFramework CONFIG REQUIRED)' "$CML" \
    && ok "find_package(CCUFramework CONFIG REQUIRED)" || violation "framework not consumed via find_package CONFIG"
grep -q 'CCUFramework::ccu-framework-config' "$CML" \
    && ok "imports CCUFramework::ccu-framework-config (installed export)" || violation "missing installed framework target link"
grep -q 'include(GNUInstallDirs)' "$CML" \
    && ok "GNUInstallDirs included" || violation "GNUInstallDirs not used"
grep -qE 'add_executable\(ccu-fota\b' "$CML" \
    && ok "production daemon target ccu-fota" || violation "production target not ccu-fota"
grep -q 'COMPONENT ccu-fota-runtime' "$CML" \
    && ok "install component ccu-fota-runtime" || violation "missing ccu-fota-runtime install component"
grep -q 'CMAKE_INSTALL_FULL_SYSCONFDIR' "$CML" \
    && ok "config uses FULL_SYSCONFDIR (FHS /etc via /usr special-case)" || violation "config not using FULL_SYSCONFDIR"

# --- forbidden negatives (CR-008 §12.2/§12.6) ---
# Operate on non-comment lines so explanatory comments do not trip the check.
NCML="$(grep -vE '^[[:space:]]*#' "$CML")"
if echo "$NCML" | grep -qE 'set\(CMAKE_CXX_FLAGS|add_compile_options\(' ; then
    violation "global CMAKE_CXX_FLAGS / add_compile_options mutation (must be per-target via FotaWarnings)"
fi
if echo "$NCML" | grep -qE 'set\(CMAKE_CXX_COMPILER|CMAKE_TOOLCHAIN_FILE|CMAKE_SYSROOT|CMAKE_FIND_ROOT_PATH|set_target_properties.*CMAKE_CXX_STANDARD' ; then
    violation "sets compiler/sysroot/target-triple (must come from CCU-BUILD toolchain)"
fi
if echo "$NCML" | grep -qE 'add_subdirectory\(.*([Ff]ramework|CCU-FW|CCU-Framework)' ; then
    violation "add_subdirectory(framework) forbidden (consume installed SDK only)"
fi
if echo "$NCML" | grep -qE 'link_directories\(' ; then
    violation "link_directories forbidden"
fi
if echo "$NCML" | grep -qE '\$\{PROJECT_SOURCE_DIR\}/third_party|/usr/local|/opt/|\$HOME' ; then
    violation "host/personal absolute path in CMake"
fi
if echo "$NCML" | grep -qE 'install\(TARGETS ccu_fota_lib|install\(TARGETS.*CcuFotaTests' ; then
    violation "internal lib / tests must not be installed"
fi

[ "$STATUS" -eq 0 ] && echo "cmake-lint: PASS" || echo "cmake-lint: FAIL"
exit $STATUS
