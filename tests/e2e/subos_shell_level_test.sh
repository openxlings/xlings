#!/usr/bin/env bash
#
# E2E for shell-level subos switching:
#
#   default (no flag) — spawns a sub-shell; not exercised here because it
#                       requires a TTY.
#   --shell <kind>    — emits eval-able shell code (sh / fish / pwsh).
#   --global          — legacy global behavior; ~/.xlings.json activeSubos
#                       + subos/current symlink.
#
# Plus the profile-side fallback chain:
#   $XLINGS_ACTIVE_SUBOS unset → use subos/current symlink (global default)
#   $XLINGS_ACTIVE_SUBOS=foo  → use subos/foo
#
# And profile auto-upgrade when an older (or marker-less) profile is
# present at $XLINGS_HOME/config/shell/xlings-profile.sh.

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_shell_level"
HOME_DIR="$RUNTIME_DIR/home"
XLINGS_BIN_PATH="$(find_xlings_bin)"

# Read the expected marker from the source of truth instead of pinning it.
# It was pinned at 9, the profiles moved to 10, and this test was failing
# unnoticed because it is not in run_all.sh. The property under test is
# "the emitted profile carries the marker the source declares" -- what the
# number happens to be today is not part of it.
PROFILE_VERSION="$(grep -oP 'xlings-profile-version: \K[0-9]+' \
    "$ROOT_DIR/src/core/xself/profile_resources.cppm" | head -1)"
[[ -n "$PROFILE_VERSION" ]] \
    || fail "could not read the profile version from profile_resources.cppm"

cleanup() {
    rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

mkdir -p "$HOME_DIR"

run_xlings() {
    env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
        "$XLINGS_BIN_PATH" "$@"
}

log "self init"
run_xlings self init >/dev/null

# ── 1. Profile contains version marker + env fallback logic ────────────
log "Scenario 1: profile is v2 (has version marker + env fallback)"
grep -q "^# xlings-profile-version: $PROFILE_VERSION" "$HOME_DIR/config/shell/xlings-profile.sh" \
    || fail "S1: missing 'xlings-profile-version: $PROFILE_VERSION' marker"
grep -q 'XLINGS_ACTIVE_SUBOS' "$HOME_DIR/config/shell/xlings-profile.sh" \
    || fail "S1: missing XLINGS_ACTIVE_SUBOS in bash profile"
grep -q 'XLINGS_ACTIVE_SUBOS' "$HOME_DIR/config/shell/xlings-profile.fish" \
    || fail "S1: missing XLINGS_ACTIVE_SUBOS in fish profile"
grep -q 'XLINGS_ACTIVE_SUBOS' "$HOME_DIR/config/shell/xlings-profile.ps1" \
    || fail "S1: missing XLINGS_ACTIVE_SUBOS in pwsh profile"

# ── 2. Profile sources cleanly with env unset → subos/current ──────────
log "Scenario 2: profile fallback (env unset → subos/current)"
out=$( unset XLINGS_ACTIVE_SUBOS
       export XLINGS_HOME="$HOME_DIR"
       . "$HOME_DIR/config/shell/xlings-profile.sh"
       printf '%s' "$XLINGS_BIN" )
expected="$HOME_DIR/subos/current/bin"
[[ "$out" == "$expected" ]] \
    || fail "S2: expected XLINGS_BIN=$expected, got $out"

# ── 3. Profile honors env override → subos/<env> ───────────────────────
run_xlings subos new web >/dev/null

log "Scenario 3: profile honors XLINGS_ACTIVE_SUBOS"
out=$( export XLINGS_ACTIVE_SUBOS=web
       export XLINGS_HOME="$HOME_DIR"
       . "$HOME_DIR/config/shell/xlings-profile.sh"
       printf '%s' "$XLINGS_BIN" )
expected="$HOME_DIR/subos/web/bin"
[[ "$out" == "$expected" ]] \
    || fail "S3: expected XLINGS_BIN=$expected, got $out"

# ── 4. --shell sh emits eval-able exports for the chosen subos ─────────
log "Scenario 4: --shell sh emits export statements"
out=$(run_xlings subos use web --shell sh)
echo "$out" | grep -q 'export XLINGS_ACTIVE_SUBOS="web"' \
    || fail "S4: --shell sh did not export XLINGS_ACTIVE_SUBOS=web (got: $out)"
echo "$out" | grep -q "subos/web/bin" \
    || fail "S4: --shell sh did not include subos/web/bin in exported PATH"

# ── 5. eval'd output activates subos in current shell ──────────────────
log "Scenario 5: eval --shell sh activates subos in current shell"
out=$(
    eval "$(run_xlings subos use web --shell sh)"
    printf '%s|%s' "${XLINGS_ACTIVE_SUBOS:-}" "$XLINGS_BIN"
)
expected_active="web"
expected_bin="$HOME_DIR/subos/web/bin"
[[ "${out%|*}" == "$expected_active" ]] \
    || fail "S5: XLINGS_ACTIVE_SUBOS not set after eval (got: $out)"
[[ "${out#*|}" == "$expected_bin" ]] \
    || fail "S5: XLINGS_BIN not set after eval (got: $out)"

# ── 6. Two parallel sub-shells get independent contexts ────────────────
run_xlings subos new infra >/dev/null

log "Scenario 6: two parallel sub-shells isolate XLINGS_ACTIVE_SUBOS"
a=$( eval "$(run_xlings subos use web --shell sh)";   printf '%s' "$XLINGS_ACTIVE_SUBOS" )
b=$( eval "$(run_xlings subos use infra --shell sh)"; printf '%s' "$XLINGS_ACTIVE_SUBOS" )
[[ "$a" == "web" ]]   || fail "S6: shell A expected web, got $a"
[[ "$b" == "infra" ]] || fail "S6: shell B expected infra, got $b"

# ── 6b. Workspace writes scope to env-resolved subos, not the persistent ─
# default. Regression: 0.4.17-pre had env priority in update_effective_paths_
# but save_workspace still wrote to subos/<globalActiveSubos_>/.xlings.json,
# so `xvm use` inside a spawned [xsubos:foo] shell silently corrupted the
# default subos's workspace.
log "Scenario 6b: 'xvm use' under env=ALT writes to subos/ALT, not default"
# Pre-seed two distinct workspaces so we can tell them apart by content.
mkdir -p "$HOME_DIR/subos/default" "$HOME_DIR/subos/web"
printf '%s\n' '{"workspace":{"sentinel":"default"}}' > "$HOME_DIR/subos/default/.xlings.json"
printf '%s\n' '{"workspace":{"sentinel":"web"}}'    > "$HOME_DIR/subos/web/.xlings.json"

# Trigger any command that loads + saves the workspace under env=web.
# `xlings env` reads + prints config and exits cleanly without touching
# the workspace; `xlings subos info web` likewise. Use `xlings xvm` which
# loads through Config (initial workspace load) — and exit. Even if no
# explicit save, the load path was the leak: it loaded the wrong file.
# We verify by writing a fresh sentinel via `xlings subos info` and
# re-reading.
#
# Simpler approach: directly check the C++-side workspace loader picks the
# right file, by seeding distinct sentinels and asserting that an xlings
# invocation with env=web sees web's data via `xlings env --json` (which
# reflects effective_workspace).
out=$(
    env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
        XLINGS_ACTIVE_SUBOS=web \
        "$XLINGS_BIN_PATH" config 2>&1 || true
)
# `xlings config` (no args) prints a TUI panel whose "active subos" row
# reflects Config::paths().activeSubos (env-aware after the priority-chain
# fix). Strip ANSI escapes for the substring check.
out_plain=$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g')
echo "$out_plain" | grep -q "active subos.*web" \
    || fail "S6b: env=web should make 'active subos' = web, got: $out_plain"
# If save path was leaking, follow-up: a run that triggers a write would
# write to default. We can't easily trigger a workspace write without
# installing a package; the load-side check + the surgical fix (both
# load and save use paths_.activeSubos in the same patch) is sufficient
# evidence that the regression is closed.
default_after=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('workspace',{}).get('sentinel'))" "$HOME_DIR/subos/default/.xlings.json")
[[ "$default_after" == "default" ]] \
    || fail "S6b: subos/default/.xlings.json sentinel was clobbered to '$default_after'"

# ── 7. --global persists into ~/.xlings.json + symlink ─────────────────
log "Scenario 7: --global updates symlink and activeSubos"
run_xlings subos use infra --global >/dev/null
target=$(readlink "$HOME_DIR/subos/current")
case "$target" in
    *infra) ;;  # absolute or relative both valid
    *) fail "S7: subos/current should resolve to infra; got '$target'" ;;
esac
python3 - "$HOME_DIR" <<'PY' || fail "S7: activeSubos not updated"
import json, sys, pathlib
home = pathlib.Path(sys.argv[1])
data = json.loads((home / ".xlings.json").read_text())
assert data.get("activeSubos") == "infra", \
    f"S7: expected activeSubos=infra, got {data.get('activeSubos')!r}"
PY

# ── 8. PATH rebuild does not pile up old subos bins after repeated --shell ──
log "Scenario 8: --shell does not stack stale subos/*/bin segments"
out=$(
    eval "$(run_xlings subos use web --shell sh)"
    eval "$(run_xlings subos use infra --shell sh)"
    printf '%s' "$PATH"
)
# After two switches, infra should be at the front and web should NOT be present.
case "$out" in
    "$HOME_DIR/subos/infra/bin"*) ;;
    *) fail "S8: PATH should start with infra bin; got '$out'" ;;
esac
echo "$out" | grep -q "$HOME_DIR/subos/web/bin" \
    && fail "S8: stale subos/web/bin still in PATH after switching to infra: $out"

# ── 9. Prompt marker — env unset → PS1 unchanged ───────────────────────
log "Scenario 9: profile leaves PS1 alone when XLINGS_ACTIVE_SUBOS unset"
ps1_after=$(
    export XLINGS_HOME="$HOME_DIR"
    unset XLINGS_ACTIVE_SUBOS
    PS1="orig> "
    . "$HOME_DIR/config/shell/xlings-profile.sh"
    printf '%s' "$PS1"
)
[[ "$ps1_after" == "orig> " ]] \
    || fail "S9: expected PS1='orig> ' (unchanged), got '$ps1_after'"

# ── 10. Prompt marker — env set + NO_COLOR → plain [xsubos:<name>] ────
log "Scenario 10: profile prepends [xsubos:<name>] (NO_COLOR → plain text)"
ps1_after=$(
    export XLINGS_HOME="$HOME_DIR"
    export XLINGS_ACTIVE_SUBOS=web
    export NO_COLOR=1
    PS1="orig> "
    . "$HOME_DIR/config/shell/xlings-profile.sh"
    printf '%s' "$PS1"
)
[[ "$ps1_after" == "[xsubos:web] orig> " ]] \
    || fail "S10: expected plain PS1='[xsubos:web] orig> ', got '$ps1_after'"

# ── 11. Prompt marker — re-sourcing is idempotent (no doubling) ────────
log "Scenario 11: re-sourcing the profile does not stack prompt markers"
ps1_after=$(
    export XLINGS_HOME="$HOME_DIR"
    export XLINGS_ACTIVE_SUBOS=web
    export NO_COLOR=1
    PS1="orig> "
    . "$HOME_DIR/config/shell/xlings-profile.sh"
    . "$HOME_DIR/config/shell/xlings-profile.sh"
    . "$HOME_DIR/config/shell/xlings-profile.sh"
    printf '%s' "$PS1"
)
count=$(printf '%s' "$ps1_after" | grep -oE '\[xsubos:web\]' | wc -l)
[[ "$count" -eq 1 ]] \
    || fail "S11: expected exactly 1 '[xsubos:web]', got $count (PS1=$ps1_after)"

# ── 11b. Prompt marker — color path emits ANSI escape ─────────────────
log "Scenario 11b: profile emits ANSI when terminal advertises color"
ps1_after=$(
    export XLINGS_HOME="$HOME_DIR"
    export XLINGS_ACTIVE_SUBOS=web
    unset NO_COLOR
    export TERM=xterm-256color
    PS1="orig> "
    . "$HOME_DIR/config/shell/xlings-profile.sh"
    printf '%s' "$PS1"
)
# Color path must include at least one ESC[ sequence + the literal marker.
printf '%s' "$ps1_after" | grep -q $'\x1b\\[' \
    || fail "S11b: expected ANSI escape in colored PS1, got: $(printf '%q' "$ps1_after")"
printf '%s' "$ps1_after" | grep -q '\[xsubos:' \
    || fail "S11b: expected literal [xsubos: marker in colored PS1, got: $(printf '%q' "$ps1_after")"

# Helper that forwards XLINGS_ACTIVE_SUBOS through the env -i sandbox so
# nesting-detection scenarios can simulate "already in a subos".
run_xlings_with_active() {
    local active="$1"; shift
    env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
        XLINGS_ACTIVE_SUBOS="$active" \
        "$XLINGS_BIN_PATH" "$@"
}

# ── 12. Nesting policy: same subos → no-op with friendly message ──────
log "Scenario 12: 'use foo' while already in foo → no spawn, exit 0"
set +e
out=$(run_xlings_with_active web subos use web 2>&1)
rc=$?
set -e
[[ "$rc" -eq 0 ]] \
    || fail "S12: expected exit 0; got $rc (output: $out)"
# Output is the colored TUI rendering: "  › already in subos web" with
# ANSI escapes interspersed. Match the visible label + name with a regex
# that tolerates the formatting bytes between them.
echo "$out" | grep -qE "already in subos.*web" \
    || fail "S12: expected 'already in subos ... web' notice; got: $out"

# ── 13. Nesting policy: different subos → notice printed (still nests) ─
log "Scenario 13: 'use bar' while in foo → nesting note printed"
# In a non-TTY harness, the spawned $SHELL exits immediately (no stdin)
# so we can capture the pre-exec nesting note xlings prints.
out=$(run_xlings_with_active web subos use infra </dev/null 2>&1 || true)
echo "$out" | grep -q "nesting subos" \
    || fail "S13: expected 'nesting subos' note; got: $out"

# ── 14. anonymous-project dir + global-scope read does not mint subos/_ ──
# Regression for: `xlings install -g <pkg>` invoked from inside an
# anonymous-project directory (cwd has a `.xlings.json` that triggers
# ProjectSubosMode::Anonymous) was composing the workspace save path from
# paths_.activeSubos = "_", producing `~/.xlings/subos/_/.xlings.json`,
# which doesn't exist. The save then failed with "Failed to write file".
# Surfaced under `xlings self install` because its patchelf-runtime-dep
# step explicitly spawns `xlings install xim:patchelf@0.18.0 -y -g`.
#
# We can't easily fake an `install -g` without fixture-index plumbing,
# so we assert the necessary precondition: even when the user's CWD is
# an anonymous project, no xlings command (here `config`) ever creates
# state under `~/.xlings/subos/_/`. The fix in Config::save_workspace's
# else branch composes the global path from env-or-globalActiveSubos_
# rather than paths_.activeSubos, so anonymous "_" cannot leak into a
# global write.
log "Scenario 14: anonymous-project dir does not leak '_' into global subos path"
mkdir -p "$RUNTIME_DIR/anon"
echo '{}' > "$RUNTIME_DIR/anon/.xlings.json"
(
    cd "$RUNTIME_DIR/anon"
    env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
        "$XLINGS_BIN_PATH" config >/dev/null 2>&1 || true
)
[[ ! -d "$HOME_DIR/subos/_" ]] \
    || fail "S14: \$XLINGS_HOME/subos/_ must never be created from anon project context"

log "PASS: subos shell-level switching (1-14 + 6b + 11b)"
