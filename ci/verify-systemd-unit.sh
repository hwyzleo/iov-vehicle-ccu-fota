#!/bin/bash
# ci/verify-systemd-unit.sh - CCU-FOTA-DSN-CR-008 §12.4
#
# Verify packaging/systemd/ccu-fota.service against the CR-008 baseline unit
# contract: ExecStart, ordering, restart policy, bounded stop timeout, and
# sandbox hardening. Runs systemd-analyze verify when available; otherwise
# performs static directive checks.
#
# Exit: 0 = pass, 1 = violation.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT="${ROOT}/packaging/systemd/ccu-fota.service"
STATUS=0

violation() { echo "[FAIL] $1" >&2; STATUS=1; }
ok()        { echo "[OK]   $1"; }

[ -f "$UNIT" ] || { echo "[FAIL] unit not found: $UNIT" >&2; exit 1; }

require() { grep -qE "^$1=" "$UNIT" && ok "$1" || violation "missing $1"; }
require_value() {
    grep -qE "^$1=$2" "$UNIT" && ok "$1=$2" || violation "$1 != $2"
}

require_value "ExecStart" "/usr/bin/ccu-fota"
require_value "Type" "simple"
require_value "Restart" "on-failure"
require_value "TimeoutStopSec" "30s"
require_value "NoNewPrivileges" "yes"
require_value "PrivateTmp" "yes"
require_value "ProtectSystem" "strict"
require_value "ProtectHome" "yes"
require_value "ReadWritePaths" "/var/lib/ccu/fota"
require_value "WantedBy" "multi-user.target"

# Ordering: after network and ccu-diag; wants ccu-diag.
grep -qE '^After=.*ccu-diag\.service' "$UNIT" \
    && ok "After ccu-diag.service" || violation "missing After=ccu-diag.service"
grep -qE '^After=.*network\.target' "$UNIT" \
    && ok "After network.target" || violation "missing After=network.target"
grep -qE '^Wants=ccu-diag\.service' "$UNIT" \
    && ok "Wants ccu-diag.service" || violation "missing Wants=ccu-diag.service"

# Baseline must NOT default to root (CR-008 §12.4: User/Group come from
# Platform Manifest drop-in, not the baseline unit).
if grep -qE '^User=root|^Group=root' "$UNIT"; then
    violation "baseline unit defaults to User/Group=root (must come from manifest drop-in)"
fi
# No secrets / personal paths / build dirs baked into the unit.
if grep -qE '/home/|/Users/|/build|password|secret|token' "$UNIT"; then
    violation "unit contains personal path / secret"
fi

# systemd-analyze verify (best-effort; not available on macOS/dev hosts).
if command -v systemd-analyze >/dev/null 2>&1; then
    if systemd-analyze verify "$UNIT" 2>/dev/null; then
        ok "systemd-analyze verify"
    else
        violation "systemd-analyze verify failed"
    fi
else
    ok "systemd-analyze not available (skipped; static checks done)"
fi

[ "$STATUS" -eq 0 ] && echo "systemd-unit: PASS" || echo "systemd-unit: FAIL"
exit $STATUS
