#!/bin/bash
# tests/smoke/smoke-fota.sh - CCU-FOTA-DSN-CR-008 §12.1/§12.7 business smoke entry
#
# Canonical smoke test referenced by the CCU-BUILD service manifest
# (smoke_test: tests/smoke/smoke-fota.sh).
#
# HOST scope (this script): verify the installed ccu-fota daemon lifecycle --
#   * binary runs
#   * config loads, store opens, SOME/IP runtime initializes (daemon reaches the
#     main loop and stays alive -- any init failure causes immediate exit)
#   * SIGTERM triggers a bounded clean shutdown within the systemd
#     TimeoutStopSec budget (CR-008 §12.4)
#
# Target-machine scope (run by CCU-BUILD when a target is available, CR-008
# §12.7): daemon-reload, start/stop/restart, FOTA Provider offer, DIAG collect,
# TBOX submit, network fault, resource cleanup, full rollback. That acceptance
# is NOT replaced by this HOST script.
#
# Usage: tests/smoke/smoke-fota.sh [--build-dir <dir>] [--show-logs]
# Exit: 0 = pass, 1 = fail

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${FOTA_BUILD_DIR:-${PROJECT_ROOT}/build}"

FOTA_LOG="${PROJECT_ROOT}/fota_output.log"
SMOKE_TMP=""
FOTA_PID=""

# CR-008 §12.4: unit TimeoutStopSec=30s. Smoke allows a small margin over it.
STOP_BUDGET_S=35
# Time to let the daemon finish init and reach the main loop.
READY_WAIT_S=4

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }
fail()    { echo -e "${RED}[FAIL]${NC} $1"; }

cleanup() {
    if [ -n "$FOTA_PID" ] && kill -0 "$FOTA_PID" 2>/dev/null; then
        kill "$FOTA_PID" 2>/dev/null || true
        wait "$FOTA_PID" 2>/dev/null || true
    fi
    [ -n "$SMOKE_TMP" ] && rm -rf "$SMOKE_TMP"
}
trap cleanup EXIT INT TERM

SHOW_LOGS=false
while [[ $# -gt 0 ]]; do
    case $1 in
        --show-logs) SHOW_LOGS=true; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --help) echo "Usage: $0 [--build-dir <dir>] [--show-logs]"; exit 0 ;;
        *) fail "Unknown option: $1"; exit 1 ;;
    esac
done

FOTA_BIN="${BUILD_DIR}/ccu-fota"
if [ ! -x "$FOTA_BIN" ]; then
    fail "ccu-fota executable not found at ${FOTA_BIN}. Build first."
    exit 1
fi

# --- build a dev config overlay with a writable store root ---
# The repo dev fixture points common.store.root at /var/lib/ccu (production).
# For unprivileged smoke/CI, overlay it with a temp store root. Production
# /etc/ccu is owned by CCU-BUILD (CR-008 §12.4/§8).
SMOKE_TMP="$(mktemp -d -t fota-smoke.XXXXXX)"
SMOKE_STATE="${SMOKE_TMP}/state"
mkdir -p "${SMOKE_TMP}/conf.d" "${SMOKE_STATE}"
cp "${PROJECT_ROOT}/config/common.yaml" "${SMOKE_TMP}/common.yaml"
cp "${PROJECT_ROOT}/config/fota.yaml" "${SMOKE_TMP}/conf.d/fota.yaml"
python3 - "${SMOKE_TMP}/common.yaml" "$SMOKE_STATE" <<'PY'
import sys, re
p, root = sys.argv[1], sys.argv[2]
s = open(p).read()
s = re.sub(r'root:\s*/var/lib/ccu.*', 'root: ' + root, s)
open(p, 'w').write(s)
PY
SMOKE_CONFIG_ROOT="$SMOKE_TMP"

rm -f "$FOTA_LOG"

# --- start ccu-fota ---
# Run with cwd = SMOKE_CONFIG_ROOT so the config loader's cwd-stage does not
# pick up the repo dev fixture (config/common.yaml with /var/lib/ccu).
info "Starting ccu-fota (smoke config root)..."
( cd "$SMOKE_CONFIG_ROOT" && "$FOTA_BIN" "$SMOKE_CONFIG_ROOT" > "$FOTA_LOG" 2>&1 ) &
FOTA_PID=$!

# --- readiness: daemon stays alive through init -> main loop ---
# Any init failure (config/store/SOME/IP) makes the daemon exit immediately,
# so surviving READY_WAIT_S proves it reached the running state.
info "Waiting ${READY_WAIT_S}s for daemon to finish init..."
sleep "$READY_WAIT_S"
if ! kill -0 "$FOTA_PID" 2>/dev/null; then
    fail "ccu-fota exited during init (config/store/SOME/IP failure)"
    [ -s "$FOTA_LOG" ] && { echo "=== fota log ==="; cat "$FOTA_LOG"; }
    exit 1
fi
success "ccu-fota running (init passed: config + store + SOME/IP)"

# --- SIGTERM -> bounded clean shutdown (CR-008 §12.4 TimeoutStopSec) ---
info "Sending SIGTERM, expecting clean shutdown within ${STOP_BUDGET_S}s..."
t0=$(date +%s)
kill -TERM "$FOTA_PID" 2>/dev/null
deadline=$((t0 + STOP_BUDGET_S))
while kill -0 "$FOTA_PID" 2>/dev/null; do
    now=$(date +%s)
    if [ "$now" -ge "$deadline" ]; then
        fail "ccu-fota did not stop within ${STOP_BUDGET_S}s (TimeoutStopSec budget)"
        kill -KILL "$FOTA_PID" 2>/dev/null || true
        wait "$FOTA_PID" 2>/dev/null || true
        [ -s "$FOTA_LOG" ] && { echo "=== fota log ==="; cat "$FOTA_LOG"; }
        exit 1
    fi
    sleep 1
done
t1=$(date +%s)
success "ccu-fota stopped cleanly in $((t1 - t0))s (within TimeoutStopSec budget)"

if [ "$SHOW_LOGS" = true ] && [ -s "$FOTA_LOG" ]; then
    echo "=== fota log ==="; cat "$FOTA_LOG"
fi

success "smoke-fota PASSED"
exit 0
