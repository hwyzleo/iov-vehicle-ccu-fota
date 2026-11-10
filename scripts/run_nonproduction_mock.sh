#!/bin/bash
#
# NON_PRODUCTION 真实链路 Mock 联调入口 (CCU-FOTA-DSN-CR-015 §D1/D2)
#
# 用法:
#   ./scripts/run_nonproduction_mock.sh [选项]
#
# 选项:
#   --scenario <file>    使用指定场景 JSON（渲染临时 config root，不修改仓库夹具）
#   --build-dir <dir>    NON_PRODUCTION 构建目录（缺省 build-nonproduction）
#   --skip-build         跳过构建，直接运行现有产物
#   --help               显示帮助
#
# 前置（fail-closed）：
#   * FOTA_ENABLE_TEST_DOUBLES=ON 独立构建目录（不进入量产 install component）；
#   * config/nonproduction/conf.d/fota.yaml 中 fota.mock.enabled=true 且
#     fota.mock.virtual_clock=false；
#   * fota.someip.generic_transport.* 与 fota.diag.registration.method_id 必须
#     已由 VEH-SOMEIP Registry 分配（0 = 未分配 -> daemon 拒绝启动/禁止安装）；
#   * common.store.root 可写（标准值 /var/lib/ccu，需要 root 或已授权目录）。
#
# 真实链路（不得用 Fake 替代）：真实 TBOX 通用消息服务、非生产云端、真实
# CCU-DIAG 清单与安装登记。Mock 仅替换车内授权/下载/条件/安装/日志副作用。
#
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-nonproduction"
CONFIG_ROOT="${PROJECT_ROOT}/config/nonproduction"
SCENARIO_OVERRIDE=""
SKIP_BUILD=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --scenario) SCENARIO_OVERRIDE="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=true; shift ;;
        --help)
            sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

fail() { echo "ERROR: $*" >&2; exit 2; }

FOTA_YAML="${CONFIG_ROOT}/conf.d/fota.yaml"
COMMON_YAML="${CONFIG_ROOT}/common.yaml"
[[ -f "$FOTA_YAML" ]] || fail "missing NON_PRODUCTION profile: $FOTA_YAML"
[[ -f "$COMMON_YAML" ]] || fail "missing common baseline: $COMMON_YAML"

# 1. 运行门禁必须显式启用 Mock，且真实链路禁止虚拟时钟（只读取 mock 段）。
mock_enabled="$(awk '/^[[:space:]]*mock:/{f=1} f&&/^[[:space:]]*enabled:/{print $2; exit}' "$FOTA_YAML")"
[[ "${mock_enabled:-false}" == "true" ]] \
    || fail "fota.mock.enabled must be true in $FOTA_YAML"
mock_virtual_clock="$(awk '/^[[:space:]]*mock:/{f=1} f&&/^[[:space:]]*virtual_clock:/{print $2; exit}' "$FOTA_YAML")"
if [[ "${mock_virtual_clock:-false}" == "true" ]]; then
    fail "fota.mock.virtual_clock=true is rejected for real-chain NON_PRODUCTION"
fi

# 2. Registry 分配检查（0 = 未分配）：ID 未分配时禁止猜测，直接 fail-closed。
generic_method="$(awk '/^[[:space:]]*generic_transport:/{f=1} f&&/^[[:space:]]*method_id:/{print $2; exit}' "$FOTA_YAML")"
generic_method="${generic_method:-0}"
[[ "$generic_method" != "0" ]] \
    || fail "fota.someip.generic_transport.method_id is 0 (Registry not allocated)"

# 3. store root 可写（标准命名 /var/lib/ccu）。
store_root="$(awk -F': *' '/^[[:space:]]*root:/{print $2; exit}' "$COMMON_YAML")"
store_root="${store_root:-/var/lib/ccu}"
store_root="${store_root%%#*}"
store_root="$(echo "$store_root" | xargs)"
if ! mkdir -p "$store_root" 2>/dev/null || [[ ! -w "$store_root" ]]; then
    fail "store root '$store_root' is not writable; run as root or grant access (do not change the standard path)"
fi

# 4. 场景文件校验。
scenario_path="$(awk -F': *' '/^[[:space:]]*scenario_path:/{print $2; exit}' "$FOTA_YAML")"
scenario_path="$(echo "${scenario_path:-}" | xargs)"
[[ -n "$scenario_path" ]] || fail "fota.mock.scenario_path is empty"
if [[ "$scenario_path" != /* ]]; then scenario_path="${PROJECT_ROOT}/${scenario_path}"; fi
[[ -r "$scenario_path" ]] || fail "scenario not readable: $scenario_path"

# 5. 构建（独立 NON_PRODUCTION 目录，install 规则被关闭）。
if [[ "$SKIP_BUILD" != true ]]; then
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug -DFOTA_ENABLE_TEST_DOUBLES=ON
    cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi
BIN="${BUILD_DIR}/ccu-fota"
[[ -x "$BIN" ]] || fail "daemon binary not found: $BIN"

# 6. 可选场景覆盖：渲染临时 config root（不修改仓库夹具）。
RUN_CONFIG_ROOT="$CONFIG_ROOT"
if [[ -n "$SCENARIO_OVERRIDE" ]]; then
    [[ -r "$SCENARIO_OVERRIDE" ]] || fail "scenario override not readable: $SCENARIO_OVERRIDE"
    TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ccu-fota-nonprod.XXXXXX")"
    mkdir -p "${TEMP_ROOT}/conf.d"
    cp "$COMMON_YAML" "${TEMP_ROOT}/common.yaml"
    sed "s#^\([[:space:]]*scenario_path:\).*#\1 ${SCENARIO_OVERRIDE}#" \
        "$FOTA_YAML" > "${TEMP_ROOT}/conf.d/fota.yaml"
    RUN_CONFIG_ROOT="$TEMP_ROOT"
    trap 'rm -rf "$TEMP_ROOT"' EXIT
    echo "INFO: using temporary config root with scenario override"
fi

echo "INFO: NON_PRODUCTION profile=$RUN_CONFIG_ROOT scenario=$scenario_path"
echo "INFO: binary=$BIN store_root=$store_root"
exec "$BIN" "$RUN_CONFIG_ROOT"
