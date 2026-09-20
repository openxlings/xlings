#!/usr/bin/env bash
# E2E: a PROJECT-scope command must never remove the GLOBAL subos's routing
# entries — and must not rebuild that table at all when it could not read the
# workspace the table is derived from.
#
# WHAT THIS DEFENDS (openxlings/xlings#582, #604)
#
# The shim table is DERIVED: it is rebuilt from the workspace rather than
# audited against it. That is the right design, and it has one sharp edge —
# an input of "nothing" derives "remove everything".
#
# Two independent ways to hand it nothing, and neither subsumes the other:
#
#   C1  the global workspace was read out of `paths_.activeSubos`, which in
#       project scope names the PROJECT's subos. For an anonymous project that
#       is `<home>/subos/_/.xlings.json` (absent -> empty); for a named one it
#       is a DIFFERENT subos's state. Fixed by resolving through one answerer,
#       `Config::global_subos_name_()`.
#   C2  an unreadable workspace file returned an empty workspace, which is
#       indistinguishable from "this subos has nothing active" — a legitimate
#       state for a fresh subos. Fixed by making "not observed" its own value
#       and refusing to rebuild on it.
#
# Measured before the fix, on the maintainer's home: 172 routing entries gone,
# `gcc` active at 16.1.0 and not on PATH. In CI it showed up as every
# `fresh-install (core)` cell failing `self doctor` since 2026-09-03.
#
# WHY THIS BUILDS STATE INSTEAD OF INSTALLING
#
# The invariant is about what a project-scope command does to a table that
# ALREADY has global entries. Installing them first would make the test need
# the network and would not make it a better test — the entries' provenance is
# not what the rule is about. `project_shim_mirror_test.sh` covers the install
# path and the ADD direction; this covers the REMOVE direction.
#
# It also deliberately does NOT wipe the global bin in setup. That wipe is
# exactly why the existing test could never see this bug: with the directory
# empty there is nothing to remove, so the question is never asked.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

HOME_DIR="$(runtime_home_dir project_scope_global_shims)"
assert_home_is_isolated "$HOME_DIR"
PROJECT_DIR="$E2E_RUNTIME_ROOT/project_scope_global_shims_proj"
BIN="$(find_xlings_bin)"

cleanup() { rm -rf "$HOME_DIR" "$PROJECT_DIR"; }
trap cleanup EXIT

GLOBAL_BIN="$HOME_DIR/subos/default/bin"

# A home with two globally-active programs, and a project that declares only
# one of them.
build_home() {
  rm -rf "$HOME_DIR"
  mkdir -p "$HOME_DIR/bin" "$GLOBAL_BIN" "$HOME_DIR/data/xpkgs"
  # The ENTRY binary, not a shim: every shim is a link to this one file, so a
  # test that swapped only the thing under test would still dispatch through
  # the old build.
  cp "$BIN" "$HOME_DIR/bin/xlings"
  local versions=""
  for name in alpha beta; do
    mkdir -p "$HOME_DIR/data/xpkgs/xim-x-$name/1.0/bin"
    printf '#!/bin/sh\necho %s 1.0\n' "$name" \
      > "$HOME_DIR/data/xpkgs/xim-x-$name/1.0/bin/$name"
    chmod +x "$HOME_DIR/data/xpkgs/xim-x-$name/1.0/bin/$name"
    ln -sf ../../../bin/xlings "$GLOBAL_BIN/$name"
    [ -n "$versions" ] && versions="$versions,"
    versions="$versions\"$name\":{\"filename\":\"$name\",\"type\":\"program\",\"versions\":{\"1.0\":{\"path\":\"$HOME_DIR/data/xpkgs/xim-x-$name/1.0/bin\"}}}"
  done
  cat > "$HOME_DIR/.xlings.json" <<EOF
{"activeSubos":"default","mirror":"${XLINGS_TEST_MIRROR:-GLOBAL}","versions":{$versions}}
EOF
  printf '{"workspace":{"alpha":"1.0","beta":"1.0"}}\n' \
    > "$HOME_DIR/subos/default/.xlings.json"
}

# $1: optional named subos for the project (empty => anonymous)
build_project() {
  local subos_name="${1:-}"
  rm -rf "$PROJECT_DIR"
  mkdir -p "$PROJECT_DIR/.xlings"
  local state_dir
  if [ -n "$subos_name" ]; then
    printf '{"subos":"%s","workspace":{"alpha":"1.0"}}\n' "$subos_name" \
      > "$PROJECT_DIR/.xlings.json"
    state_dir="$PROJECT_DIR/.xlings/subos/$subos_name"
  else
    printf '{"mirror":"%s","workspace":{"alpha":"1.0"}}\n' \
      "${XLINGS_TEST_MIRROR:-GLOBAL}" > "$PROJECT_DIR/.xlings.json"
    state_dir="$PROJECT_DIR/.xlings/subos/_"
  fi
  mkdir -p "$state_dir/bin"
  printf '{"workspace":{"alpha":{"active":"1.0","installed":["1.0"]}}}\n' \
    > "$state_dir/.xlings.json"
  cat > "$PROJECT_DIR/.xlings/.xlings.json" <<EOF
{"versions":{"alpha":{"filename":"alpha","type":"program","versions":{"1.0":{"path":"$HOME_DIR/data/xpkgs/xim-x-alpha/1.0/bin"}}}},
 "workspace":{"alpha":{"active":"1.0","installed":["1.0"]}}}
EOF
}

global_entries() { ls "$GLOBAL_BIN" 2>/dev/null | sort | tr '\n' ' '; }

assert_entries() {
  local want="$1" why="$2" got
  got="$(global_entries)"
  [ "$got" = "$want" ] || fail "$why: global bin is [$got], expected [$want]"
  log "  ok: $why"
}

# ── 1. anonymous project (the fresh-install CI shape) ────────────────
build_home
build_project ""
assert_entries "alpha beta " "precondition: both global shims present"
( cd "$PROJECT_DIR" && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" \
    "$HOME_DIR/bin/xlings" use alpha 1.0 >/dev/null 2>&1 ) || true
assert_entries "alpha beta " "anonymous project scope kept the global entries"

# ── 2. named project subos that collides with a real subos in the home ──
# The nastier variant: the path exists and is readable, so "could not read it"
# never fires — only resolving the GLOBAL subos correctly saves this one.
build_home
mkdir -p "$HOME_DIR/subos/myenv/bin"
printf '{"workspace":{"alpha":"1.0"}}\n' > "$HOME_DIR/subos/myenv/.xlings.json"
build_project myenv
( cd "$PROJECT_DIR" && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" \
    "$HOME_DIR/bin/xlings" use alpha 1.0 >/dev/null 2>&1 ) || true
assert_entries "alpha beta " "named project subos did not rebuild from the wrong subos"

# ── 3. unreadable global workspace: refuse, do not derive "remove all" ──
build_home
printf '{ this is not json' > "$HOME_DIR/subos/default/.xlings.json"
build_project ""
OUT="$( cd "$PROJECT_DIR" && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" \
    "$HOME_DIR/bin/xlings" use alpha 1.0 2>&1 || true )"
assert_entries "alpha beta " "an unreadable global workspace did not prune the table"
assert_contains "$OUT" "not rebuilt" \
  "refusing to rebuild must say so — a repair that did not happen must not read as one that did"

# ── 4. the control: global scope still rebuilds normally ─────────────
# Without this the three rows above would also pass if the table had simply
# stopped being written at all.
build_home
rm -f "$GLOBAL_BIN/beta"
( cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" \
    "$HOME_DIR/bin/xlings" use alpha 1.0 >/dev/null 2>&1 ) || true
assert_entries "alpha beta " "global scope still restores a missing entry"

# ── 5. a subos with no workspace file yet is OBSERVED, and empty ─────
#
# The guard in row 3 has an over-strict failure mode, and it is not theoretical:
# the first cut of it treated "the file is not there" as "could not observe",
# which made a brand-new home refuse to write its table at all — the mirror
# image of the bug this test exists for, caught by project_shim_mirror_test.
#
# Three cases, not two:
#   the subos directory is absent          -> not observed (refuse)
#   the directory is there, no file yet    -> observed, empty (a fresh subos)
#   the file is there and unreadable       -> not observed (refuse)
build_home
rm -f "$HOME_DIR/subos/default/.xlings.json"
rm -f "$GLOBAL_BIN/alpha" "$GLOBAL_BIN/beta"
build_project ""
( cd "$PROJECT_DIR" && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" \
    "$HOME_DIR/bin/xlings" use alpha 1.0 >/dev/null 2>&1 ) || true
# The project's own command name must reach the global bin — that is what
# project_contributions() is for, and a refusing guard would silently skip it.
[ -e "$GLOBAL_BIN/alpha" ] || fail \
  "a subos with no workspace file yet was treated as unreadable: the project's shim never reached the global bin"
log "  ok: a fresh subos is observed-and-empty, not unreadable"

# ── 6. the same guard, in GLOBAL scope ───────────────────────────────
#
# No project anywhere. `Config::workspace()` then returns the very map
# `load_global_workspace_` filled, so its observation status is that map's --
# and a flat "the command just wrote it" would leave the common, non-project
# path rebuilding from an unreadable workspace. Same defect, other scope.
build_home
printf '{ this is not json' > "$HOME_DIR/subos/default/.xlings.json"
OUT="$( cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" \
    "$HOME_DIR/bin/xlings" use alpha 1.0 2>&1 || true )"
assert_entries "alpha beta " "global scope with an unreadable workspace did not prune the table"

log "PASS: project scope preserves global shims"
