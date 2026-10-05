#!/usr/bin/env bash
#
# E2E for `xlings __complete` — the hidden command the shell profiles call on
# Tab. The engine is unit-tested (tests/unit/test_completion.cpp); this checks
# the wired-up command and its output shape end to end.
#
# Deliberately no `self init`: `__complete` must answer before a home is laid
# out, and staying off the init path keeps this test independent of the index.

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/completion"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$HOME_DIR"

# Values only: candidates are `value<TAB>description`.
values() { printf '%s\n' "$1" | cut -f1; }

comp() {
    run_xlings "$HOME_DIR" "$ROOT_DIR" __complete "$@"
}

contains() {
    values "$1" | grep -qx -- "$2"
}

# ── 1. Root: commands, offered even before a home exists ───────────────
log "completion: root commands"
out="$(comp "")"
contains "$out" "install" || fail "S1: 'install' not offered"
contains "$out" "subos"   || fail "S1: 'subos' not offered"
contains "$out" "--agent" || fail "S1: global '--agent' not offered"

# ── 2. Prefix filtering ────────────────────────────────────────────────
log "completion: prefix filtering"
out="$(comp "su")"
contains "$out" "subos" || fail "S2: 'subos' missing for prefix 'su'"
if contains "$out" "install"; then fail "S2: 'install' survived prefix 'su'"; fi

# ── 3. Nested subcommands ──────────────────────────────────────────────
log "completion: nested subcommands"
out="$(comp "subos" "")"
for want in new use list remove info stop runtime; do
    contains "$out" "$want" || fail "S3: 'subos $want' not offered"
done
if contains "$out" "install"; then fail "S3: root commands leaked into 'subos'"; fi

# ── 4. Options once a dash is typed ────────────────────────────────────
log "completion: options after a dash"
out="$(comp "subos" "use" "--")"
contains "$out" "--global"  || fail "S4: '--global' not offered"
contains "$out" "--sandbox" || fail "S4: '--sandbox' not offered"
if contains "$out" "new"; then fail "S4: subcommand offered as an option"; fi

# ── 5. Enum option values ──────────────────────────────────────────────
log "completion: enum option values"
out="$(comp "config" "--lang" "")"
contains "$out" "auto" || fail "S5: '--lang' value 'auto' not offered"
contains "$out" "en"   || fail "S5: '--lang' value 'en' not offered"
contains "$out" "zh"   || fail "S5: '--lang' value 'zh' not offered"

out="$(comp "subos" "use" "--shell" "")"
contains "$out" "fish" || fail "S5: '--shell' value 'fish' not offered"
contains "$out" "pwsh" || fail "S5: '--shell' value 'pwsh' not offered"

# ── 6. A leading program token (fish's `commandline -opc`) is dropped ──
log "completion: leading program token is ignored"
out="$(comp "xlings" "")"
contains "$out" "install" || fail "S6: 'xlings' program token was not dropped"

# ── 7. `--opt=value` completes the value and answers with the whole word ──
log "completion: inline option value"
out="$(comp "config" "--lang=z")"
contains "$out" "--lang=zh" || fail "S7: '--lang=zh' not offered"
out="$(comp "subos" "use" "--shell=bash" "--")"
contains "$out" "--global" || fail "S7: '--shell=bash' swallowed the next word"

# ── 8. After `--` nothing is an option or a subcommand ─────────────────
log "completion: nothing after the terminator"
out="$(comp "subos" "--" "")"
if contains "$out" "use"; then fail "S8: subcommand offered after --"; fi
if contains "$out" "--yes"; then fail "S8: option offered after --"; fi

log "PASS: completion (1-8)"
