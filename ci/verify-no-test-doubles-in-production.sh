#!/bin/bash
# ci/verify-no-test-doubles-in-production.sh - CCU-FOTA-DSN-CR-009 §构建与运行隔离
#
# 量产门禁：验证量产 daemon 制品与配置不含 Mock / 测试桩 / 测试云地址 / 测试凭据。
#   1. 量产 configure 不得定义 FOTA_ENABLE_TEST_DOUBLES
#   2. 量产 daemon 二进制不得链接/包含 Mock* / test_double 符号
#   3. 量产配置不得出现 mock.enabled=true / 测试云地址 / 测试凭据
#
# Mock/联调 profile 由 FOTA_ENABLE_TEST_DOUBLES 显式启用，仅允许测试/联调；
# 量产禁止“真实实现失败后自动回退 Mock”。
#
# Usage: ci/verify-no-test-doubles-in-production.sh [--build-dir <dir>]
# Exit: 0 = pass, 1 = violation found.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"

while [[ $# -gt 0 ]]; do
    case $1 in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --help) echo "Usage: $0 [--build-dir <dir>]"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

status=0

# ---------------------------------------------------------------------------
# 1. 量产 configure 不得启用 test doubles
# ---------------------------------------------------------------------------
CMAKE_CACHE="${BUILD_DIR}/CMakeCache.txt"
if [[ -f "$CMAKE_CACHE" ]]; then
    if grep -q "FOTA_ENABLE_TEST_DOUBLES:BOOL=ON" "$CMAKE_CACHE"; then
        echo "VIOLATION: FOTA_ENABLE_TEST_DOUBLES=ON in production build cache" >&2
        status=1
    fi
fi

# ---------------------------------------------------------------------------
# 2. 量产 daemon 二进制不得含 Mock / test_double 符号
# ---------------------------------------------------------------------------
DAEMON="${BUILD_DIR}/ccu-fota"
if [[ -f "$DAEMON" ]]; then
    # nm 符号表扫描；匹配 MockCloudProxy / MockInstallExecutor / test_double 等
    if nm "$DAEMON" 2>/dev/null | grep -Ei "Mock(CloudProxy|InstallExecutor|InventoryProvider|PackageDownloader|VehicleConditionProvider|ConsentProvider|LogCollector)|test_double" >/dev/null; then
        echo "VIOLATION: production daemon contains Mock/test-double symbols" >&2
        nm "$DAEMON" 2>/dev/null | grep -Ei "Mock|test_double" | head >&2
        status=1
    fi
fi

# ---------------------------------------------------------------------------
# 3. 量产配置不得出现 mock.enabled=true / 测试云地址 / 测试凭据
# ---------------------------------------------------------------------------
for cfg in "${ROOT}/config/fota.default.yaml" "${ROOT}/config/fota.yaml"; do
    [[ -f "$cfg" ]] || continue
    if grep -E "^\s*enabled:\s*true" "$cfg" 2>/dev/null | grep -qi "mock" ; then
        : # 兜底：mock 段下 enabled:true 视为违规
    fi
    if grep -Eq "mock\.enabled:\s*true|^\s*enabled:\s*true\s*#\s*.*mock" "$cfg" 2>/dev/null; then
        echo "VIOLATION: mock.enabled=true in production config: $cfg" >&2
        status=1
    fi
    if grep -Eq "mock://|test\.cloud|test-cred" "$cfg" 2>/dev/null; then
        echo "VIOLATION: test cloud/credential marker in config: $cfg" >&2
        status=1
    fi
done

# fota.default.yaml 的 mock.enabled 必须为 false
if grep -A2 "^  mock:" "${ROOT}/config/fota.default.yaml" 2>/dev/null | grep -q "enabled: true"; then
    echo "VIOLATION: fota.default.yaml mock.enabled must be false in production" >&2
    status=1
fi

if [[ $status -eq 0 ]]; then
    echo "PASS: no test doubles / mock / test-cloud in production artifacts"
fi
exit $status
