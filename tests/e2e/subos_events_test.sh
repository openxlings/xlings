#!/usr/bin/env bash
# E2E test for subos commands routed through EventStream:
# every subos create/use/remove must surface either a DataEvent
# (rendered as a styled "subos created/switched/removed" line) or
# an ErrorEvent (rendered via log::error with optional hint).
#
# Scenarios:
#   1. create good name              → success line
#   2. create reserved 'current'     → InvalidInput error
#   3. create invalid char           → InvalidInput error
#   4. create duplicate              → InvalidInput error
#   5. switch to non-existent        → NotFound error + nearest-name hint
#   6. switch to existing            → success line
#   7. remove the active subos       → InvalidInput error + hint
#   8. remove 'default'              → InvalidInput error
#   9. remove a non-existent         → NotFound error
#  10. switch back + remove          → success line, dir gone

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_events"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"

log "Initializing sandbox XLINGS_HOME at $HOME_DIR"
RUN self init >/dev/null 2>&1 || fail "self init failed"

# Strip ANSI color sequences for portable substring matching.
strip_ansi() { sed -E 's/\x1b\[[0-9;]*m//g'; }

assert_contains() {
  local needle=$1 haystack=$2 context=$3
  local clean
  clean="$(echo "$haystack" | strip_ansi)"
  echo "$clean" | grep -Fq -- "$needle" \
    || fail "$context: expected to find '$needle' in output:\n$clean"
}

# ── 1. create good name ───────────────────────────────────────────
log "Scenario 1: create good name"
OUT="$(RUN subos new s1 2>&1)" || fail "S1: create exited non-zero"
assert_contains "subos created: s1" "$OUT" "S1"
[[ -d "$HOME_DIR/subos/s1/bin" ]] || fail "S1: subos dir not created"

# ── 2. create reserved 'current' ──────────────────────────────────
log "Scenario 2: create 'current' (reserved)"
OUT="$(RUN subos new current 2>&1)" || true
assert_contains "'current' is a reserved" "$OUT" "S2"

# ── 3. create invalid char ────────────────────────────────────────
log "Scenario 3: create invalid name"
OUT="$(RUN subos new "bad name" 2>&1)" || true
assert_contains "invalid subos name" "$OUT" "S3"

# ── 4. create duplicate ───────────────────────────────────────────
log "Scenario 4: create duplicate"
OUT="$(RUN subos new s1 2>&1)" || true
assert_contains "already exists" "$OUT" "S4"

# ── 5. switch non-existent ────────────────────────────────────────
log "Scenario 5: switch to non-existent"
OUT="$(RUN subos use nope --global 2>&1)" || true
assert_contains "subos 'nope' not found" "$OUT" "S5"
assert_contains "did you mean" "$OUT" "S5 hint"

# ── 6. switch to existing ─────────────────────────────────────────
log "Scenario 6: switch to existing"
OUT="$(RUN subos use s1 --global 2>&1)" || fail "S6: switch exited non-zero"
assert_contains "switched to subos s1" "$OUT" "S6"

# ── 7. remove active subos ────────────────────────────────────────
log "Scenario 7: remove active subos"
OUT="$(RUN subos remove s1 2>&1)" || true
assert_contains "cannot remove the active subos" "$OUT" "S7"
assert_contains "switch first" "$OUT" "S7 hint"

# ── 8. remove 'default' ───────────────────────────────────────────
log "Scenario 8: remove 'default'"
RUN subos use default --global >/dev/null 2>&1
OUT="$(RUN subos remove default 2>&1)" || true
assert_contains "cannot remove the 'default' subos" "$OUT" "S8"

# ── 9. remove non-existent ────────────────────────────────────────
log "Scenario 9: remove non-existent"
OUT="$(RUN subos remove nope 2>&1)" || true
assert_contains "subos 'nope' not found" "$OUT" "S9"

# ── 10. switch back to default + remove s1 ────────────────────────
log "Scenario 10: remove s1 cleanly"
OUT="$(RUN subos remove s1 2>&1)" || fail "S10: remove exited non-zero"
assert_contains "subos removed: s1" "$OUT" "S10"
[[ ! -d "$HOME_DIR/subos/s1" ]] || fail "S10: subos dir still on disk"

log "PASS: subos events scenarios 1-10"
