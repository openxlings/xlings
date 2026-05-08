#!/usr/bin/env bash
# nested_xlings_home_test.sh — regression for the project-discovery
# boundary fix shipped in 0.4.20.
#
# Background: a package like mcpp ships its own xlings home as part of
# its payload, with the inner XLINGS_HOME pointing at
# `<outer-home>/data/xpkgs/<repo>-x-mcpp/<ver>/registry`. Pre-fix, the
# inner xlings's load_project_config_() walk would skip its own home
# (homeNorm match), continue walking upward through the package layers,
# and ultimately mis-load the OUTER `<outer-home>/.xlings.json` as a
# project root. That polluted projectDir_ with the outer home and
# routed every install/shim/workspace path into a phantom
# `<outer-home>/.xlings/{subos,data,...}` tree disjoint from the
# inner home's actual layout.
#
# Fix: any directory containing `.xlings.json` AND a sibling `subos/`
# directory is an xlings home (regardless of whose) — terminate the
# project walk there, never load it as a project. Project layouts
# nest subos under `.xlings/subos/`, not bare `subos/`, so this
# signature has no false positives in real projects.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/nested_xlings_home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

OUTER_HOME="$RUNTIME_DIR/outer-home"
INNER_HOME="$OUTER_HOME/data/xpkgs/xim-x-mcpp/0.0.1/registry"

# ── Outer home: typical xlings home layout ───────────────────────────
mkdir -p "$OUTER_HOME/subos/default/bin"
cp "$XLINGS_BIN" "$OUTER_HOME/xlings"
cat > "$OUTER_HOME/.xlings.json" <<JSON
{ "activeSubos": "default", "mirror": "GLOBAL" }
JSON

# ── Inner home, packaged under outer's xpkgs (mimics mcpp/registry) ──
mkdir -p "$INNER_HOME/subos/default/bin"
mkdir -p "$INNER_HOME/data/runtimedir"
cat > "$INNER_HOME/.xlings.json" <<JSON
{ "activeSubos": "default", "mirror": "GLOBAL" }
JSON

run_config() {
  local home="$1"; local cwd="$2"
  ( cd "$cwd" && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$home" \
      "$XLINGS_BIN" config 2>&1 | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' )
}

# Detect project mode by checking output for project-anonymous subos
# path (.xlings/subos/) or the explicit XLINGS_DATA_PROJECT line.
is_project_mode() {
  echo "$1" | grep -qE "(\.xlings/subos/|XLINGS_DATA_PROJECT)"
}

# ── Scenario 1: inner xlings inside its own runtimedir ───────────────
log "S1: inner xlings (cwd=runtimedir) must NOT cross into outer home"
out="$(run_config "$INNER_HOME" "$INNER_HOME/data/runtimedir")"
if is_project_mode "$out"; then
  fail "S1: inner xlings entered project mode (likely mistook outer home as project)
  output:
$out"
fi
echo "$out" | grep -F "$INNER_HOME" >/dev/null \
  || fail "S1: inner xlings did not report inner home"
log "  ✓ inner xlings stayed in its own home, no project leak"

# ── Scenario 2: regular project still detected (no false negative) ───
log "S2: regular user project (NOT inside any xlings home) still loads"
PROJECT_DIR="$RUNTIME_DIR/myproj"
mkdir -p "$PROJECT_DIR/subdir"
cat > "$PROJECT_DIR/.xlings.json" <<JSON
{ "workspace": { "node": "22.0.0" } }
JSON
out="$(run_config "$OUTER_HOME" "$PROJECT_DIR/subdir")"
is_project_mode "$out" \
  || fail "S2: legitimate project should activate project mode
  output:
$out"
log "  ✓ legitimate project mode still detected"

# ── Scenario 3: bare cwd /tmp + XLINGS_HOME=inner — no project anywhere
log "S3: cwd outside any project, XLINGS_HOME=inner — no project leak"
out="$(run_config "$INNER_HOME" "/tmp")"
if is_project_mode "$out"; then
  fail "S3: should not find any project from /tmp
  output:
$out"
fi
log "  ✓ no project mode from /tmp"

# ── Scenario 4: env var XLINGS_PROJECT_DIR pointing at an xlings home
#                must be rejected (same boundary check applies)
log "S4: XLINGS_PROJECT_DIR pointing at an xlings home is rejected"
out="$(
  cd /tmp && \
  env -i HOME="$HOME" PATH=/usr/bin:/bin \
    XLINGS_HOME="$INNER_HOME" \
    XLINGS_PROJECT_DIR="$OUTER_HOME" \
    "$XLINGS_BIN" config 2>&1 | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
)"
if is_project_mode "$out"; then
  fail "S4: XLINGS_PROJECT_DIR pointing at an xlings home was accepted as project
  output:
$out"
fi
log "  ✓ env-var route honors the same boundary"

log "PASS: nested xlings home — project discovery boundary holds"
