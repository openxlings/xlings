#!/usr/bin/env bash
# interface_multi_repo_error_visibility_test.sh — regression test for
# issue #374: `interface install_packages` / `plan_install` silently
# exits 1 (no error event on the NDJSON wire) when a project configures
# >=2 index_repos and one of them is degenerate (no pkgs/).
#
# Two independent architectural defects combine to produce the bug:
#
#   Root defect 1 (observability): the xim command layer reports failures
#   via the global log::error(), which is fully suppressed in interface
#   mode (platform::set_tui_mode(true)). The NDJSON stream carries a
#   first-class ErrorEvent kind, but cmd_install never emits it — so a
#   non-zero exit produces {"exitCode":1,"kind":"result"} with an empty
#   event stream. The consumer (mcpp) cannot tell what failed.
#
#   Root defect 2 (multi-repo robustness): PackageCatalog::rebuild() and
#   sync_all_repos() fail-fast on the first bad repo, so one degenerate
#   index_repo collapses the whole catalog — even the healthy repos.
#
# Assertions (design doc §6):
#   A1 (P1 best-effort skip): 2 project repos, 2nd has no pkgs/, target
#      resolves in the HEALTHY repo → exit 0, install_plan emitted, AND a
#      wire event naming the skipped bad repo (not silently ignored).
#   A2 (P0 invariant + structured error): 2 project repos, target absent
#      everywhere → NON-ZERO exit AND at least one {"kind":"error"} event
#      on the wire (never a bare result envelope).
#
# Refs: .agents/docs/2026-07-19-issue374-multi-repo-silent-exit-design.md

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/interface_multi_repo_error_visibility"
HOME_DIR="$RUNTIME_DIR/home"
APP_DIR="$RUNTIME_DIR/app"          # the "project" dir (has .xlings.json, NOT an xlings home)
GOOD_DIR="$RUNTIME_DIR/proj_good"   # a valid project index (has pkgs/)
BAD_DIR="$RUNTIME_DIR/proj_bad"     # degenerate: dir exists but no pkgs/

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

mkdir -p "$HOME_DIR/subos/default/bin" "$APP_DIR" "$GOOD_DIR/pkgs/t" "$BAD_DIR"

# ── a VALID project index with one package "toolx" (script type, no
#    download — plan_install only resolves, never installs) ───────────
cat > "$GOOD_DIR/pkgs/t/toolx.lua" <<'LUA'
package = {
    spec = "1",
    name = "toolx",
    description = "Test fixture: healthy project-repo package (issue #374 coverage)",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}
function install() return true end
LUA

# ── the DEGENERATE project index: exists, but NO pkgs/ (models a
#    default-namespace redirect / empty local-dev repo) ───────────────
printf 'placeholder — deliberately no pkgs/ subdir\n' > "$BAD_DIR/README.md"

# ── global home: xim namespace -> local fixture (offline), sub-indexes
#    neutralised so nothing reaches the network ────────────────────────
cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "subos": {"default": {"dir": ""}},
  "index_repos": [
    {"name": "xim", "url": "$FIXTURE_INDEX_DIR"}
  ]
}
JSON

RUN_HOME() {
  ( cd /tmp && env -i HOME="$HOME" USER="${USER:-ci}" SHELL="${SHELL:-/bin/sh}" \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" XLINGS_INDEX_SOURCE=git \
      "$XLINGS_BIN" "$@" )
}
RUN_PROJ() {
  ( cd "$APP_DIR" && env -i HOME="$HOME" USER="${USER:-ci}" SHELL="${SHELL:-/bin/sh}" \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" XLINGS_INDEX_SOURCE=git \
      "$XLINGS_BIN" "$@" )
}

RUN_HOME self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
printf 'xim_indexrepos = {}\n' > "$FIXTURE_INDEX_DIR/xim-indexrepos.lua" 2>/dev/null || true

# project manifest: TWO index_repos, the 2nd (projbad) is degenerate
cat > "$APP_DIR/.xlings.json" <<JSON
{
  "index_repos": [
    {"name": "projgood", "url": "$GOOD_DIR"},
    {"name": "projbad",  "url": "$BAD_DIR"}
  ]
}
JSON

# ════════════════════════════════════════════════════════════════════
# A1: healthy target resolves despite a broken sibling repo (P1),
#     and the skipped repo is surfaced on the wire (not silent).
# ════════════════════════════════════════════════════════════════════
log "A1: plan_install toolx with 2 project repos (2nd degenerate) → exit 0 + skip surfaced"

set +e
out_a1="$(RUN_PROJ interface plan_install --args '{"targets":["toolx"]}' 2>/dev/null)"
rc_a1=$?
set -e
echo "$out_a1" | sed 's/^/    /'

[[ "$rc_a1" -eq 0 ]] \
  || fail "A1: expected exit 0 (healthy repo should still resolve), got $rc_a1
$out_a1"
echo "$out_a1" | grep -q '"dataKind":"install_plan"' \
  || fail "A1: no install_plan event — toolx did not resolve from the healthy repo
$out_a1"
echo "$out_a1" | grep -q 'projbad' \
  || fail "A1: the skipped bad repo 'projbad' is NOT surfaced on the wire (silent skip)
$out_a1"
log "  ✓ A1: exit 0, toolx planned, projbad skip surfaced on wire"

# ════════════════════════════════════════════════════════════════════
# A2: genuinely-missing target → non-zero exit MUST carry a structured
#     error event on the wire (the core issue #374 invariant).
# ════════════════════════════════════════════════════════════════════
log "A2: plan_install <missing> with 2 project repos → non-zero + {\"kind\":\"error\"} on wire"

set +e
out_a2="$(RUN_PROJ interface plan_install --args '{"targets":["nonexistent-pkg-zzz"]}' 2>/dev/null)"
rc_a2=$?
set -e
echo "$out_a2" | sed 's/^/    /'

[[ "$rc_a2" -ne 0 ]] \
  || fail "A2: expected non-zero exit for a missing package, got $rc_a2
$out_a2"
echo "$out_a2" | grep -q '"kind":"error"' \
  || fail "A2: non-zero exit produced NO {\"kind\":\"error\"} event — this is the issue #374 silent failure
$out_a2"
log "  ✓ A2: non-zero exit carries a structured error event"

# Also exercise the destructive install_packages capability (the exact
# entry point mcpp uses) on the missing target — same invariant.
log "A2b: install_packages <missing> → non-zero + structured error"
set +e
out_a2b="$(RUN_PROJ interface install_packages \
            --args '{"targets":["nonexistent-pkg-zzz"],"yes":true}' 2>/dev/null)"
rc_a2b=$?
set -e
[[ "$rc_a2b" -ne 0 ]] \
  || fail "A2b: install_packages missing target exit was $rc_a2b, expected non-zero
$out_a2b"
echo "$out_a2b" | grep -q '"kind":"error"' \
  || fail "A2b: install_packages non-zero exit produced NO structured error event
$out_a2b"
log "  ✓ A2b: install_packages non-zero exit carries a structured error event"

log "PASS: issue #374 multi-repo error visibility covered"
