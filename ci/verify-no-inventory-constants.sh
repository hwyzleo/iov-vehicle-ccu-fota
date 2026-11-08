#!/bin/bash
#
# ci/verify-no-inventory-constants.sh - CCU-FOTA-DSN-CR-013 / US-036 扫描门禁
#
# CR-013 后 ECU 清单内容的唯一权威是 CCU-DIAG：FOTA 源码与量产制品中不得再出现
# ECU 标识 / 件号 / 基线版本等清单常量（历史实现在
# src/someip/diag_inventory_client.cpp 内写死 ECU001/ECU002、PN001/PN002、
# BASELINE001、registry_version=1.0.0 与固定时间戳，导致云端长期看到假数据）。
#
# 扫描范围：生产源码、打包/部署制品与脚本（tests/ 与 scripts/ 下的开发夹具不在
# 量产制品内，允许使用任意测试数据）。
#
# 用法：ci/verify-no-inventory-constants.sh [--root <dir>]
# Exit: 0 = pass, 1 = violation found.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
while [[ $# -gt 0 ]]; do
    case $1 in
        --root) ROOT="$2"; shift 2 ;;
        --help) echo "Usage: $0 [--root <dir>]"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

STATUS=0
SEARCH_DIRS=("${ROOT}/src" "${ROOT}/include" "${ROOT}/packaging")

# 禁止的清单常量（单词边界，避免误匹配注释中的说明性文字时也保持严格）。
#   ECU001/ECU002/ECU003... 历史硬编码 ECU 标识
#   PN001/PN002             历史硬编码件号
#   BASELINE001             历史硬编码基线
FORBIDDEN_PATTERN='\bECU00[0-9]\b|\bPN00[0-9]\b|\bBASELINE001\b'

echo "[i] scanning production sources for hardcoded inventory constants"
for dir in "${SEARCH_DIRS[@]}"; do
    [[ -d "$dir" ]] || continue
    hits="$(grep -rInE "$FORBIDDEN_PATTERN" \
                --include='*.c' --include='*.cc' --include='*.cpp' \
                --include='*.h' --include='*.hpp' --include='*.yaml' \
                --include='*.yml' --include='*.json' --include='*.service' \
                --include='*.sh' \
                "$dir" 2>/dev/null || true)"
    if [[ -n "$hits" ]]; then
        echo "[FAIL] hardcoded ECU inventory constants found in ${dir}:" >&2
        echo "$hits" >&2
        STATUS=1
    else
        echo "[OK]   clean: ${dir}"
    fi
done

# 清单接口必须真实调用 DIAG（不得回退为“只读 VIN + 本地造清单”）。
CLIENT="${ROOT}/src/someip/diag_inventory_client.cpp"
if [[ -f "$CLIENT" ]]; then
    if grep -q 'METHOD_COLLECT_VEHICLE_INVENTORY' "$CLIENT"; then
        echo "[OK]   diag_inventory_client calls METHOD_COLLECT_VEHICLE_INVENTORY"
    else
        echo "[FAIL] diag_inventory_client does not call the DIAG inventory method" >&2
        STATUS=1
    fi
    if grep -q 'DiagInventoryCodec::decode' "$CLIENT"; then
        echo "[OK]   diag_inventory_client decodes payload via DiagInventoryCodec"
    else
        echo "[FAIL] diag_inventory_client does not use DiagInventoryCodec" >&2
        STATUS=1
    fi
else
    echo "[FAIL] ${CLIENT} not found" >&2
    STATUS=1
fi

exit $STATUS
