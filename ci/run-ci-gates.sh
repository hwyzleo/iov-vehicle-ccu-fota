#!/bin/bash
# ci/run-ci-gates.sh - CCU-FOTA-DSN-CR-008 §12.7 提交级 CI 门禁编排
#
# Runs the FOTA-repo CR-008 commit-level gates in order:
#   1. CMake lint (static build contract)
#   2. systemd unit contract
#   3. configure + build (HOST) + unit/contract tests (CcuFotaTests)
#   4. installed Framework SDK consumer (relocatable find_package + 5 targets)
#   5. DESTDIR install + install-manifest (exact ccu-fota-runtime file set)
#   6. daemon lifecycle smoke (start -> init -> SIGTERM -> bounded stop)
#
# TARGET cross-compile / ELF / ABI / NEEDED / RPATH / target-machine business
# smoke are run by CCU-BUILD (CR-008 §12.6/§12.7) and are not covered here.
#
# Usage: ci/run-ci-gates.sh [--sdk-prefix <dir>] [--build-dir <dir>]
# Exit: 0 = all pass, 1 = any gate failed.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_PREFIX="${CCU_FRAMEWORK_PREFIX:-$HOME/.local}"
BUILD_DIR="${ROOT}/build"
while [[ $# -gt 0 ]]; do
    case $1 in
        --sdk-prefix) SDK_PREFIX="$2"; shift 2 ;;
        --build-dir)  BUILD_DIR="$2";  shift 2 ;;
        --help) echo "Usage: $0 [--sdk-prefix <dir>] [--build-dir <dir>]"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; NC='\033[0m'
gate() { echo -e "\n${BLUE}=== GATE $1: $2 ===${NC}"; }
pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; }

FAILURES=0
run() { # run <name> <cmd...>
    local name="$1"; shift
    if "$@"; then pass "$name"; else fail "$name"; FAILURES=$((FAILURES+1)); fi
}

# 0. 旧 FOTA 契约制品扫描（CR-011 测试矩阵 7）
gate 0 "legacy ota artifact scan (CR-011)"
run "no-legacy-ota" bash "${ROOT}/ci/verify-no-legacy-ota.sh"

# 0b. 清单常量扫描（CCU-FOTA-DSN-CR-013 / US-036 验收标准 1）
# 清单内容唯一来源是 CCU-DIAG：生产源码/制品不得重新引入 ECU 标识、件号、基线常量。
gate "0b" "no hardcoded ECU inventory constants (CR-013)"
run "no-inventory-constants" bash "${ROOT}/ci/verify-no-inventory-constants.sh"

# 1. CMake lint
gate 1 "CMake lint"
run "cmake-lint" bash "${ROOT}/ci/verify-cmake-lint.sh"

# 2. systemd unit
gate 2 "systemd unit contract"
run "systemd-unit" bash "${ROOT}/ci/verify-systemd-unit.sh"

# 3. configure + build + tests
gate 3 "configure + build + unit/contract tests"
rm -rf "$BUILD_DIR"
if cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
        -DCCUFramework_DIR="$SDK_PREFIX/lib/cmake/CCUFramework" >/dev/null 2>&1 \
   && cmake --build "$BUILD_DIR" -j >/dev/null 2>&1 \
   && ctest --test-dir "$BUILD_DIR" --output-on-failure >/dev/null 2>&1; then
    pass "build+tests"
else
    fail "build+tests"; FAILURES=$((FAILURES+1))
fi

# 4. installed Framework SDK consumer
gate 4 "installed Framework SDK consumer"
run "installed-framework-consumer" cmake -DSDK_PREFIX="$SDK_PREFIX" -P "${ROOT}/ci/verify-installed-framework.cmake"

# 5. DESTDIR install + install manifest
gate 5 "DESTDIR install + install manifest"
INSTALL_ROOT="$(mktemp -d -t fota-install.XXXXXX)"
if env DESTDIR="$INSTALL_ROOT" cmake --install "$BUILD_DIR" --prefix /usr --component ccu-fota-runtime >/dev/null 2>&1; then
    run "install-manifest" python3 "${ROOT}/ci/verify-install-manifest.py" "$INSTALL_ROOT"
else
    fail "DESTDIR install"; FAILURES=$((FAILURES+1))
fi
rm -rf "$INSTALL_ROOT"

# 6. daemon lifecycle smoke
gate 6 "daemon lifecycle smoke"
run "smoke-fota" bash "${ROOT}/tests/smoke/smoke-fota.sh" --build-dir "$BUILD_DIR"

echo -e "\n========================================"
if [ "$FAILURES" -eq 0 ]; then
    echo -e "${GREEN}ALL CR-008 CI GATES PASSED${NC}"
    exit 0
else
    echo -e "${RED}${FAILURES} GATE(S) FAILED${NC}"
    exit 1
fi
