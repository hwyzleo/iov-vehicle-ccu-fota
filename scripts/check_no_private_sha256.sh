#!/bin/bash
#
# scripts/check_no_private_sha256.sh
# CCU-FOTA-DSN-CR-006 §私有 SHA-256 清理 / §测试设计(清理)
#
# 阻止 FOTA 仓库重新引入私有 SHA-256 Core、轮常量、直接 OpenSSL/其他后端调用、
# 第二套 Hex 编码，或绕过 ccu-framework-hash。SHA-256 摘要及其 Hex 必须仅经
# <ccu/fw/hash/sha256.hpp>（ccu::fw::hash::sha256 / sha256_hex）；非摘要的可逆
# Hex 编解码经 <ccu/fw/hash/hex.hpp>（ccu::fw::hash::bytesToHex / hexToBytes）。
#
# 用法: ./scripts/check_no_private_sha256.sh
# 退出码: 0=通过, 1=发现违规
#

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATUS=0

# ---------------------------------------------------------------------------
# 1. 生产源码（src/ include/）不得出现私有 SHA-256 后端调用 / 轮常量 / 初始值。
#    允许：ccu-framework-hash 的 API 引用与 include。
# ---------------------------------------------------------------------------
FORBIDDEN=$(grep -rnE \
    'EVP_Digest|SHA256_Init|SHA256_Update|SHA256_Final|SHA256_Transform|openssl|OPENSSL|0x428a2f98|0x5be0cd19|0x6a09e667' \
    "$ROOT/src" "$ROOT/include" 2>/dev/null || true)
VIOLATIONS=$(echo "$FORBIDDEN" \
    | grep -vE 'ccu::fw::hash|ccu/fw/hash' \
    | grep -vE '^[[:space:]]*$' || true)
if [ -n "$VIOLATIONS" ]; then
    echo "[ERROR] forbidden private SHA-256 / direct backend patterns:" >&2
    echo "$VIOLATIONS" >&2
    STATUS=1
fi

# ---------------------------------------------------------------------------
# 2. 生产源码不得定义/调用私有 sha256() 函数（排除 framework 调用与 include）。
#    匹配 sha256( 但排除 ccu::fw::hash::sha256 与 <ccu/fw/hash/sha256.hpp>。
# ---------------------------------------------------------------------------
PRIVATE_FN=$(grep -rnEi 'sha256[[:space:]]*\(' "$ROOT/src" "$ROOT/include" 2>/dev/null \
    | grep -vEi 'ccu::fw::hash::sha256|ccu/fw/hash' || true)
if [ -n "$PRIVATE_FN" ]; then
    echo "[ERROR] private sha256() call not routed through framework:" >&2
    echo "$PRIVATE_FN" >&2
    STATUS=1
fi

# ---------------------------------------------------------------------------
# 3. 生产源码不得实现自定义 Hex 编码器（典型 0123456789abcdef 查表）。
#    摘要 Hex 必须来自 framework sha256_hex（Fingerprint.hex() 仅返回 framework
#    结果）；非摘要的可逆 Hex 编解码经 framework 通用工具 <ccu/fw/hash/hex.hpp>
#    (ccu::fw::hash::bytesToHex / hexToBytes)。FOTA 仓内禁止任何私有 Hex 查表。
# ---------------------------------------------------------------------------
HEX_ENCODER=$(grep -rnE '0123456789abcdef|0123456789ABCDEF' \
    "$ROOT/src" "$ROOT/include" 2>/dev/null || true)
if [ -n "$HEX_ENCODER" ]; then
    echo "[ERROR] private hex encoder detected in production code:" >&2
    echo "$HEX_ENCODER" >&2
    STATUS=1
fi

# ---------------------------------------------------------------------------
# 4. CMakeLists.txt 必须链接 ccu-framework-hash（生产二进制只经 framework 使用 SHA-256）。
# ---------------------------------------------------------------------------
if ! grep -q 'CCUFramework::ccu-framework-hash' "$ROOT/CMakeLists.txt"; then
    echo "[ERROR] CMakeLists.txt does not link CCUFramework::ccu-framework-hash" >&2
    STATUS=1
fi

if [ "$STATUS" -eq 0 ]; then
    echo "[OK] no private SHA-256 / hex encoder; ccu-framework-hash linked."
else
    echo "[FAIL] CCU-FOTA-DSN-CR-006 private SHA-256 guard violated." >&2
    exit 1
fi
