#!/bin/bash
#
# ci/verify-no-legacy-ota.sh - CCU-FOTA-DSN-CR-011 制品扫描（测试矩阵 7）
#
# 一次性 release-set 迁移后，除迁移模块及其测试外，任何源码、配置、测试、proto、
# 构建脚本和制品中不得残留未批准的旧 FOTA 契约：
#   * package 路径/符号：vehicle/ota/v1、vehicle::ota、vehicle.ota.v1
#   * service / protocol：service=vehicle.ota、"ota-v1"
#   * 字段：ota_master_version（\b 词边界，避免误匹配 fota_master_version）
#   * Store key：ota.vehicle_task / ota.inventory / ota.consent / ota.downloads /
#     ota.execution / ota.event_outbox / ota.controls / ota.policy / ota.log_jobs
#
# 迁移模块（fota_state_migration.*）与迁移测试（fota_state_migration_test.cpp）为
# 已批准的迁移期例外，必须引用旧 key 以完成 ota.* -> fota.*；本脚本显式排除。
#
# 用法: ci/verify-no-legacy-ota.sh
# Exit: 0 = 无残留，1 = 发现残留。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PATTERN='vehicle/ota/v1|vehicle::ota|vehicle\.ota\.v1|service.?=?["'"'"']vehicle\.ota["'"'"']|\bota_master_version\b|protocol_version.?[:=].?["'"'"']ota-v1["'"'"']|["'"'"']ota\.(vehicle_task|inventory|consent|downloads|execution|event_outbox|controls|policy|log_jobs)["'"'"']'

MATCHES="$(
  grep -rnE "$PATTERN" \
      --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.yaml' \
      --include='*.proto' --include='*.cmake' --include='*.txt' --include='*.py' \
      --include='*.sh' --include='*.idl' --include='*.service' \
      . 2>/dev/null \
  | grep -vE '^\./(build|graphify-out|\.git)/' \
  | grep -vE 'ci/verify-no-legacy-ota\.sh' \
  | grep -vE 'src/store/fota_state_migration\.cpp|include/ccu/fota/store/fota_state_migration\.hpp|tests/ota/fota_state_migration_test\.cpp'
)"

if [ -n "$MATCHES" ]; then
    echo "LEGACY OTA ARTIFACTS FOUND:"
    echo "$MATCHES"
    exit 1
fi

echo "OK: no legacy vehicle.ota / ota_master_version / ota-v1 / ota.* store keys outside migration module"
exit 0
