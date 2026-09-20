#!/usr/bin/env bash
# E2E: an empty subos name must never reach the filesystem.
#
# WHAT THIS DEFENDS
#
# Found while measuring the blast radius of the `required` validation, not by
# reading the code. On xlings 2026.9.16.1, two malformed agent calls:
#
#     xlings interface create_subos --args '{}'   -> {"exitCode":0}
#     xlings interface remove_subos --args '{}'   -> {"exitCode":0}
#
# registered a subos named "" and then removed it — and since every path in
# subos.cpp is built as `<home>/subos/<name>`, an empty name resolves to the
# subos ROOT. The second call deleted `default`, `current` and every other
# subos in the home. Both calls reported success.
#
# The capability layer passed `json.value("name", "")` straight through. The
# CLI never could: its parser demands the argument. So the hole was reachable
# only from the published agent interface, which is exactly where nobody was
# watching.
#
# TWO LAYERS, AND THE TEST PINS BOTH SEPARATELY:
#
#   1. the interface dispatcher refuses a params object that omits a declared
#      `required` field — the request is not well-formed;
#   2. `subos::create` / `subos::remove` refuse an empty name — the operation's
#      own precondition, which holds whoever asks. `{"name":""}` is a
#      well-formed request, so layer 1 lets it through on purpose; only layer 2
#      stops it.
#
# The assertions are on the EFFECT, not the exit code. A non-zero exit after
# the directory is already gone would be no comfort.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

HOME_DIR="$(runtime_home_dir empty_subos_name)"
assert_home_is_isolated "$HOME_DIR"
BIN="$(find_xlings_bin)"
cleanup() { rm -rf "$HOME_DIR"; }
trap cleanup EXIT
cleanup

run_x() { env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" "$BIN" "$@"; }

mkdir -p "$HOME_DIR"
run_x self init >/dev/null 2>&1 || fail "self init failed"
run_x subos new keepme >/dev/null 2>&1 || fail "could not create the subos under test"

snapshot() { ls "$HOME_DIR/subos" 2>/dev/null | sort | tr '\n' ' '; }
BEFORE="$(snapshot)"
case "$BEFORE" in
  *keepme*) ;;
  *) fail "precondition: expected a 'keepme' subos, got [$BEFORE]" ;;
esac
log "  precondition: [$BEFORE]"

assert_intact() {
  local why="$1" now; now="$(snapshot)"
  [ "$now" = "$BEFORE" ] || fail "$why changed the subos tree:
  before: [$BEFORE]
  after:  [$now]"
  [ -d "$HOME_DIR/subos" ] || fail "$why removed the subos root entirely"
  log "  ok: $why left every subos in place"
}

# ── layer 1: the request is not well-formed ──────────────────────────
for cap in create_subos remove_subos switch_subos; do
  set +e
  OUT="$(run_x interface "$cap" --args '{}' 2>/dev/null)"
  RC=$?
  set -e
  [ "$RC" -ne 0 ] || fail "$cap with no params exited 0: $OUT"
  case "$OUT" in
    *E_INVALID_INPUT*) ;;
    *) fail "$cap with no params did not report E_INVALID_INPUT: $OUT" ;;
  esac
done
assert_intact "the three no-params capability calls"

# The original sequence, in order — create registered the empty name, and
# remove is what used the registration to delete the root.
run_x interface create_subos --args '{}' >/dev/null 2>&1 || true
run_x interface remove_subos --args '{}' >/dev/null 2>&1 || true
assert_intact "create-then-remove with no params"

# ── layer 2: well-formed, and still refused by the operation ─────────
# `{"name":""}` satisfies `required` — the field is present. Only the
# operation's own precondition stops this one, which is why it is a separate
# guard and not a duplicate of the first.
set +e
OUT="$(run_x interface remove_subos --args '{"name":""}' 2>/dev/null)"
RC=$?
set -e
[ "$RC" -ne 0 ] || fail "remove_subos with an explicitly empty name exited 0: $OUT"
case "$OUT" in
  *"empty name resolves to the subos root"*) ;;
  *) fail "the refusal did not explain itself: $OUT" ;;
esac
run_x interface create_subos --args '{"name":""}' >/dev/null 2>&1 || true
run_x interface remove_subos --args '{"name":""}' >/dev/null 2>&1 || true
assert_intact "create-then-remove with an explicitly empty name"

# ── the control: a real name still works, both ways ──────────────────
# Without this, "refuse everything" passes every row above.
run_x subos new throwaway >/dev/null 2>&1 || fail "creating a named subos broke"
[ -d "$HOME_DIR/subos/throwaway" ] || fail "subos new did not create the directory"
run_x interface remove_subos --args '{"name":"throwaway"}' >/dev/null 2>&1 \
  || fail "removing a named subos through the interface broke"
[ -d "$HOME_DIR/subos/throwaway" ] && fail "remove_subos did not remove the named subos"
log "  ok: a real name still creates and removes"
assert_intact "the named create/remove round trip"

log "PASS: an empty subos name never reaches the filesystem"
