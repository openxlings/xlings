#!/usr/bin/env bash
# shim_identity_test.sh — a shim runs the entry binary's code, in every subos (#615).
#
# On Windows a shim is a hard link to the entry binary's FILE OBJECT, and an
# upgrade replaces that object: every shim kept running the previous client,
# `self doctor` called them "not ours", and nothing could put them back. POSIX
# shims are symlinks and never had the problem -- which is exactly why this
# test builds the Windows state by hand on Linux (a regular file that is an
# older xlings build) instead of waiting for Windows CI to be the only place
# the code path runs.
#
#   S1  the binary carries the multicall marker (what a future stale copy of
#       it is recognised by) and the legacy fingerprint.
#   S2  legacy stale shims in TWO subos: `self doctor` reports them as an
#       error with a runnable remedy (the entry's full path, not `xlings`).
#   S3  `self doctor --fix` relinks both -- not only the active subos -- and a
#       second run is clean.
#   S4  `self init` alone relinks them too.
#   S5  a stale copy that IS a new build hands off to the entry (execv), with
#       the entry's exit code; a user's own program in the bin directory is
#       never touched by any of it.
#
# Refs: .agents/docs/2026-09-26-issue615-shim-identity-design.md
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/shim_identity"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_LOCK_TIMEOUT=30 "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/bin" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"

# The installed layout (`bin/xlings`), not the bootstrap one: the shims must
# link to the file an installed home dispatches through.
cp "$XLINGS_BIN" "$HOME_DIR/bin/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
RUN subos new s2 >/dev/null 2>&1 || fail "subos new s2 failed"

ENTRY="$HOME_DIR/bin/xlings"
SHIM_DEFAULT="$HOME_DIR/subos/default/bin/xlings"
SHIM_S2="$HOME_DIR/subos/s2/bin/xlings"
THEIRS="$HOME_DIR/subos/default/bin/their-tool"
[[ -x "$ENTRY" ]] || fail "setup: no entry binary at $ENTRY"
[[ -e "$SHIM_DEFAULT" && -e "$SHIM_S2" ]] || fail "setup: subos shims missing"

# A user's own program in a bin directory. Every scenario must leave it alone.
printf '#!/bin/sh\necho theirs\n' > "$THEIRS"
chmod +x "$THEIRS"

# A pre-marker xlings, as an older client left it: a regular file (the shape a
# Windows hard link takes once the entry moved on), carrying only the legacy
# fingerprint. It is never executed here -- classification reads content.
plant_legacy() {
  rm -f "$1"
  printf '#!/bin/sh\n# [xlings:self]: failed to create shim {} - {}\necho legacy\n' > "$1"
  chmod +x "$1"
}

points_at_entry() {
  [[ "$(readlink -f "$1")" == "$(readlink -f "$ENTRY")" ]]
}

# ── S1 ────────────────────────────────────────────────────────────────
log "S1: the build carries the marker and the legacy fingerprint"
grep -qaF 'xlings:multicall-shim:abi=1:caps=handoff:f615c0de' "$XLINGS_BIN" \
  || fail "S1: the multicall marker is not in the binary -- a stale copy of this build would read as somebody's program"
grep -qaF '[xlings:self]: failed to create shim' "$XLINGS_BIN" \
  || fail "S1: the legacy fingerprint left the binary; builds before the marker could no longer be told from a user's program"

# ── S2 ────────────────────────────────────────────────────────────────
log "S2: legacy shims in two subos are an error with a runnable remedy"
plant_legacy "$SHIM_DEFAULT"
plant_legacy "$SHIM_S2"
set +e
out=$(RUN self doctor 2>&1); rc=$?
set -e
[[ $rc -ne 0 ]] || fail "S2: self doctor exited 0 with the previous client on PATH: $out"
grep -q 'outdated shim' <<<"$out" || fail "S2: no 'outdated shim' finding: $out"
grep -q 'subos/s2/bin' <<<"$out" \
  || fail "S2: only the active subos was checked; s2's shim went unreported: $out"
grep -qE "'?${ENTRY}'? self doctor --fix" <<<"$out" \
  || fail "S2: the remedy must name the entry binary itself -- \`xlings\` on PATH is the stale client: $out"

# ── S3 ────────────────────────────────────────────────────────────────
log "S3: --fix relinks every subos, and converges"
set +e
fixout=$(RUN self doctor --fix 2>&1); fixrc=$?
set -e
# A repair that worked must say so in its exit code, or every script that
# runs `--fix` reads a healed home as a failure.
[[ $fixrc -eq 0 ]] || fail "S3: --fix exited $fixrc after repairing everything: $fixout"
points_at_entry "$SHIM_DEFAULT" || fail "S3: default's shim still is not the entry"
points_at_entry "$SHIM_S2" || fail "S3: s2's shim was not relinked -- only the active subos was repaired"
out=$(RUN self doctor 2>&1 || true)
if grep -q 'outdated shim' <<<"$out"; then fail "S3: still reported after --fix: $out"; fi
[[ "$(cat "$THEIRS")" == $'#!/bin/sh\necho theirs' ]] || fail "S3: a user's program was modified"

# ── S4 ────────────────────────────────────────────────────────────────
log "S4: self init relinks them too"
plant_legacy "$SHIM_S2"
RUN self init >/dev/null 2>&1 || fail "S4: self init failed"
points_at_entry "$SHIM_S2" || fail "S4: self init left s2's shim stale"

# ── S5 ────────────────────────────────────────────────────────────────
log "S5: a stale copy of a new build hands off to the entry"
rm -f "$SHIM_S2"
cp "$ENTRY" "$SHIM_S2"
printf 'ZZZZZZZZZZZZZZZZ' >> "$SHIM_S2"     # a different build, same code
chmod +x "$SHIM_S2"
out=$(cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
        XLINGS_HANDOFF_TRACE=1 "$SHIM_S2" --version 2>&1) \
  || fail "S5: the stale copy failed to run: $out"
grep -q 'xlings: handoff' <<<"$out" || fail "S5: no handoff happened: $out"
grep -qF -- "-> $(readlink -f "$ENTRY")" <<<"$out" || grep -qF -- "-> $ENTRY" <<<"$out" \
  || fail "S5: handed off to something other than the entry: $out"
set +e
( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
    "$SHIM_S2" no-such-subcommand-615 >/dev/null 2>&1 ); shim_rc=$?
RUN no-such-subcommand-615 >/dev/null 2>&1; entry_rc=$?
set -e
[[ $entry_rc -ne 0 ]] || fail "S5: an unknown subcommand exited 0"
[[ $shim_rc -eq $entry_rc ]] || fail "S5: handoff exit code $shim_rc != entry's $entry_rc"
out=$(RUN self doctor --all 2>&1 || true)
grep -q 'hands off' <<<"$out" || fail "S5: a handoff-capable stale shim was not reported as one: $out"
RUN self doctor --fix >/dev/null 2>&1 || true
points_at_entry "$SHIM_S2" || fail "S5: --fix did not relink the handoff-capable copy"
[[ -f "$THEIRS" ]] || fail "S5: a user's program was removed"

log "PASS: shim identity (S1-S5)"
