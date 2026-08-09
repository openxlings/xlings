#!/usr/bin/env bash
# E2E: a SubOS manifest is the authority for its one active core runtime.
# Multiple payload versions may coexist in the global store, but `use` may
# not silently move this SubOS to a runtime different from its declaration.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$(runtime_home_dir subos_runtime_authority)"
HOME_DIR="$RUNTIME_DIR/home"
SUBOS_FILE="$HOME_DIR/subos/default/.xlings.json"
HOME_FILE="$HOME_DIR/.xlings.json"
SNAPSHOT_DIR="$RUNTIME_DIR/snapshot"
PAYLOAD39="$HOME_DIR/data/xpkgs/xim-x-glibc/2.39"
PAYLOAD44="$HOME_DIR/data/xpkgs/xim-x-glibc/2.44"
VIEWER1="$HOME_DIR/data/xpkgs/xim-x-viewer/1.0.0"
VIEWER2="$HOME_DIR/data/xpkgs/xim-x-viewer/2.0.0"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$HOME_DIR/subos/default/bin" "$PAYLOAD39" "$PAYLOAD44" \
         "$VIEWER1" "$VIEWER2" "$SNAPSHOT_DIR"

XLINGS_BIN="$(find_xlings_bin)"
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"

cat > "$HOME_FILE" <<JSON
{
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "versions": {
    "glibc": {
      "type": "group",
      "versions": {
        "2.39": {"kind": "group", "path": "$PAYLOAD39"},
        "2.44": {"kind": "group", "path": "$PAYLOAD44"}
      }
    },
    "viewer": {
      "type": "group",
      "versions": {
        "1.0.0": {"kind": "group", "path": "$VIEWER1"},
        "2.0.0": {"kind": "group", "path": "$VIEWER2"}
      }
    },
    "legacy-edge-owner": {
      "type": "group",
      "versions": {
        "1.0.0": {"kind": "group", "path": "$VIEWER1"}
      },
      "bindings": {
        "missing-peer": {"1.0.0": "9.9.9"}
      }
    }
  }
}
JSON

cat > "$SUBOS_FILE" <<JSON
{
  "subos_info": {
    "schema_version": 1,
    "runtime": "glibc@2.44",
    "envs": {},
    "created_at": "2026-08-09T00:00:00Z",
    "created_by": "runtime-authority-e2e"
  },
  "workspace": {
    "glibc": {"active": "2.39", "installed": ["2.39", "2.44"]},
    "viewer": {"active": "1.0.0", "installed": ["1.0.0", "2.0.0"]},
    "legacy-edge-owner": {"active": "1.0.0", "installed": ["1.0.0"]}
  }
}
JSON

run_x() {
  ( cd /tmp && env -i HOME="$HOME_DIR" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

active_version() {
  python3 - "$SUBOS_FILE" "$1" <<'PY'
import json, pathlib, sys
workspace = json.loads(pathlib.Path(sys.argv[1]).read_text()).get("workspace", {})
print((workspace.get(sys.argv[2]) or {}).get("active", ""))
PY
}

# Quick doctor must compare the declaration to THIS SubOS's active XVM state;
# finding some matching version elsewhere in the global store is insufficient.
doctor_rc=0
DOCTOR_OUT="$(run_x self doctor 2>&1)" || doctor_rc=$?
[[ "$doctor_rc" -ne 0 ]] \
  || fail "doctor accepted declared glibc@2.44 with active glibc@2.39"
DOCTOR_CLEAN="$(printf '%s\n' "$DOCTOR_OUT" | strip_ansi)"
grep -Fq 'glibc@2.44' <<<"$DOCTOR_CLEAN" \
  || fail "doctor did not name declared glibc@2.44:\n$DOCTOR_CLEAN"
grep -Fq 'glibc@2.39' <<<"$DOCTOR_CLEAN" \
  || fail "doctor did not name active glibc@2.39:\n$DOCTOR_CLEAN"
log "  ✓ quick doctor names declared and active runtime"

# A refusal is a strict zero-write path. The planted dangling legacy edge is
# important: current `use` self-heals it before its old decision boundary, so
# comparing only the workspace would miss an earlier versions/config write.
cp "$HOME_FILE" "$SNAPSHOT_DIR/home.json"
cp "$SUBOS_FILE" "$SNAPSHOT_DIR/subos.json"

use_rc=0
USE_OUT="$(run_x use glibc@2.39 2>&1)" || use_rc=$?
[[ "$use_rc" -ne 0 ]] \
  || fail "ordinary use silently activated a runtime that contradicts the manifest"
USE_CLEAN="$(printf '%s\n' "$USE_OUT" | strip_ansi)"
grep -Eqi 'migrat' <<<"$USE_CLEAN" \
  || fail "runtime refusal had no explicit migration hint:\n$USE_CLEAN"
grep -Fq 'glibc@2.44' <<<"$USE_CLEAN" \
  || fail "runtime refusal did not name the declared runtime:\n$USE_CLEAN"
grep -Fq 'glibc@2.39' <<<"$USE_CLEAN" \
  || fail "runtime refusal did not name the requested runtime:\n$USE_CLEAN"

cmp -s "$SNAPSHOT_DIR/home.json" "$HOME_FILE" \
  || fail "refused use rewrote the home versions/config file (dangling edge was pruned)"
cmp -s "$SNAPSHOT_DIR/subos.json" "$SUBOS_FILE" \
  || fail "refused use rewrote the SubOS workspace/manifest file"
[[ -d "$PAYLOAD39" && -d "$PAYLOAD44" ]] \
  || fail "refused use deleted one of the globally coexisting payloads"
[[ "$(active_version glibc)" == "2.39" ]] \
  || fail "refused use changed the active runtime"
log "  ✓ mismatching use refused before every config/workspace write"

# Runtime authority must not be bypassed by an unrelated manifest defect.
# Doctor owns env-schema repair; `use` can still read the valid runtime field
# and must enforce it while the rest of the block is malformed.
python3 - "$SUBOS_FILE" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
data = json.loads(p.read_text())
data["subos_info"]["envs"] = 42
p.write_text(json.dumps(data, indent=2))
PY
cp "$SUBOS_FILE" "$SNAPSHOT_DIR/subos-malformed.json"
malformed_rc=0
MALFORMED_OUT="$(run_x use glibc@2.39 2>&1)" || malformed_rc=$?
[[ "$malformed_rc" -ne 0 ]] \
  || fail "malformed envs bypassed the still-valid runtime authority"
grep -Eqi 'migrat' <<<"$(printf '%s\n' "$MALFORMED_OUT" | strip_ansi)" \
  || fail "malformed-block refusal lost the runtime migration hint"
cmp -s "$SNAPSHOT_DIR/subos-malformed.json" "$SUBOS_FILE" \
  || fail "malformed-block runtime refusal rewrote the manifest"
cmp -s "$SNAPSHOT_DIR/home.json" "$HOME_FILE" \
  || fail "malformed-block runtime refusal rewrote the versions/config file"
cp "$SNAPSHOT_DIR/subos.json" "$SUBOS_FILE"
log "  ✓ unrelated manifest defects cannot disable runtime authority"

# The declared exact runtime remains a normal activation, and unrelated
# package families keep their existing multi-version switching semantics.
run_x use glibc@2.44 >/dev/null 2>&1 \
  || fail "exact declared runtime glibc@2.44 was refused"
[[ "$(active_version glibc)" == "2.44" ]] \
  || fail "exact declared runtime did not become active"

run_x use viewer@2.0.0 >/dev/null 2>&1 \
  || fail "runtime guard leaked into an unrelated package family"
[[ "$(active_version viewer)" == "2.0.0" ]] \
  || fail "unrelated package family did not switch"
[[ "$(active_version glibc)" == "2.44" ]] \
  || fail "unrelated switch disturbed the core runtime"
[[ -d "$PAYLOAD39" && -d "$PAYLOAD44" ]] \
  || fail "successful activation deleted a globally retained runtime payload"

log "PASS: one declared core runtime per SubOS, many payloads globally"
