#!/bin/bash
#
# 量产构建 Mock 隔离扫描 (CCU-FOTA-DSN-CR-015 §D1 测试矩阵 7)
#
# 用法: ./scripts/check_no_test_doubles.sh [path/to/ccu-fota]
#
# 断言 OFF（量产）产物中不存在 Mock/测试桩符号与联调场景/测试地址：
#   * ccu_fota::ota::mock::* / Mock* / Fake* 实现符号；
#   * scenario_script 场景解析与 NON_PRODUCTION 场景路径。
# 事件名常量（如 fota.mock.activated）不属于 Mock 实现，不在扫描范围。
#
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-${PROJECT_ROOT}/build/ccu-fota}"

fail() { echo "FAIL: $*" >&2; exit 1; }

[[ -x "$BIN" ]] || fail "binary not found: $BIN"
command -v nm >/dev/null 2>&1 || fail "nm not available"

symbols="$(nm -C "$BIN" 2>/dev/null || true)"
if echo "$symbols" | grep -Eq 'ccu_fota::ota::mock::|MockPackageDownloader|MockInstallExecutor|MockVehicleConditionProvider|MockConsentProvider|MockLogCollector|FakeInstalledVersionRegistrar|mock::loadScenario|mock::parseScenario|assembleMockVehiclePorts'; then
    echo "$symbols" | grep -E 'ccu_fota::ota::mock::|MockPackageDownloader|MockInstallExecutor|MockVehicleConditionProvider|MockConsentProvider|MockLogCollector|FakeInstalledVersionRegistrar|mock::loadScenario|mock::parseScenario|assembleMockVehiclePorts' >&2
    fail "Mock/test-double symbols found in production artifact: $BIN"
fi

strings_out="$(strings "$BIN" 2>/dev/null || true)"
if echo "$strings_out" | grep -Eq 'nonproduction/scenarios|scenario_script\.cpp|mock://cdn/'; then
    fail "NON_PRODUCTION scenario/test address strings found in production artifact"
fi

echo "OK: no Mock/test-double symbols or NON_PRODUCTION scenario strings in $BIN"
