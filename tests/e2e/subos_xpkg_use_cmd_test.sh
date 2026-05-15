#!/usr/bin/env bash
# E2E test: `xlings subos use <name> --cmd "<command>"` non-interactive exec.
#
# Validates:
#   1. --cmd runs the command via `sh -c` (POSIX) and exits with the
#      command's exit code (basic shell-level path)
#   2. stdout is captured properly
#   3. --cmd is incompatible with --global / --shell (early-error)
#
# Sandbox mode is not exercised here (requires bwrap/proot install +
# capabilities — covered separately in CI matrix tests).
#
# Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M3)
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_xpkg_use_cmd"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "subos": { "default": { "dir": "" } }
}
EOF

log "Creating test subos..."
run_xlings "$HOME_DIR" "$ROOT_DIR" subos new test-cmd --storage shared >/dev/null 2>&1 \
    || fail "subos new test-cmd failed"

# 1. Basic --cmd: output capture
log "Running --cmd 'echo hello-from-subos'..."
OUTPUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" subos use test-cmd --cmd "echo hello-from-subos" 2>&1 | tr -d '\0')"
if ! grep -qF "hello-from-subos" <<< "$OUTPUT"; then
    echo "Output was: $OUTPUT"
    fail "--cmd 'echo' didn't produce expected output"
fi
log "  ok: --cmd produces expected stdout"

# 2. Exit code propagation
log "Running --cmd 'exit 42'..."
set +e
run_xlings "$HOME_DIR" "$ROOT_DIR" subos use test-cmd --cmd "exit 42" >/dev/null 2>&1
RC=$?
set -e
if [[ "$RC" -ne 42 ]]; then
    fail "exit code not propagated (got $RC, want 42)"
fi
log "  ok: exit code 42 propagated correctly"

# 3. --cmd with --global should fail (incompatible)
log "Verifying --cmd + --global is rejected..."
set +e
ERR_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" subos use test-cmd --global --cmd "true" 2>&1)"
RC=$?
set -e
if [[ "$RC" -eq 0 ]]; then
    fail "--cmd + --global should have errored but didn't"
fi
if ! grep -qF "incompatible" <<< "$ERR_OUT"; then
    echo "Error output: $ERR_OUT"
    fail "--cmd + --global error message should mention 'incompatible'"
fi
log "  ok: --cmd + --global rejected with clear message"

# 4. --cmd with --shell should fail (incompatible)
log "Verifying --cmd + --shell <kind> is rejected..."
set +e
ERR_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" subos use test-cmd --shell sh --cmd "true" 2>&1)"
RC=$?
set -e
if [[ "$RC" -eq 0 ]]; then
    fail "--cmd + --shell should have errored but didn't"
fi
log "  ok: --cmd + --shell rejected"

# 5. Equals-style --cmd=<value>
log "Verifying --cmd=<value> equals-style works..."
OUTPUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" subos use test-cmd --cmd="echo eqstyle" 2>&1 | tr -d '\0')"
if ! grep -qF "eqstyle" <<< "$OUTPUT"; then
    echo "Output: $OUTPUT"
    fail "--cmd=<value> form didn't work"
fi
log "  ok: --cmd=<value> form works"

log "PASS: subos use --cmd works end-to-end (M3)"
