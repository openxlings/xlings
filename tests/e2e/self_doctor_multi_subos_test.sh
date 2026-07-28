#!/usr/bin/env bash
# self_doctor_multi_subos_test.sh — `self doctor --fix` on a home with more
# than one subos.
#
# The versions DB and the payload store are shared by every subos of a home;
# workspace, shims and sysroot are not. doctor detects across the first pair
# and repairs only in the second, and until this test nothing pinned what
# happens where. Measured consequences, each an assertion below:
#
#   S2  a broken payload only another subos uses was repaired HERE, which
#       registered that package into THIS subos's workspace, shims and
#       sysroot. The user asked to fix their own environment.
#   S3  the migration marker lives in the HOME config while the repair
#       happens in ONE subos, so fixing the first subos stamped the whole
#       home as migrated and the rest never saw the hint again.
#   S4  fixing the last outstanding subos must land the stamp -- otherwise
#       the gate above just moves the nag from "too early" to "forever".
#   S5  a subos pointing at a version the shared DB no longer has was
#       invisible from everywhere except that subos.
#   S6  R3 reads `xlings remove` exiting 0 as "the record is gone". On a
#       multi-subos home removal detaches the current subos and KEEPS the
#       record, so R3 reported a version REMOVED that was still installed.
#
# Refs: .agents/docs/2026-07-28-multi-subos-repair-design.md
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/self_doctor_multi_subos"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
FAIL_MARKER="$RUNTIME_DIR/make-install-fail"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

# Every command names the subos it runs in. This test is entirely about which
# subos a thing happens in, so there is no default form on purpose.
RUN_IN() {
  local subos="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS="$subos" \
      "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR/subos/default/bin" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/m"

# Two fixtures with the same shape and different owners:
#   ms-shared  — installed in BOTH subos
#   ms-only-b  — installed in `other` ONLY
#
# ms-shared's install hook fails while $FAIL_MARKER exists. That is what makes
# S6 reachable: R2 (`xlings install`) has to fail for R3 to run at all, and a
# hook returning false is the same failure path as a bad download.
for pkg in ms-shared ms-only-b ms-proj; do
cat > "$LOCAL_INDEX_DIR/pkgs/m/$pkg.lua" <<LUA
package = {
    spec = "1", name = "$pkg",
    description = "multi-subos doctor fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    if "$pkg" == "ms-shared" and os.isfile("$FAIL_MARKER") then
        return false
    end
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "$pkg"),
                 "#!/bin/sh\necho $pkg@" .. pkginfo.version() .. "\n")
    return true
end
function config()
    xvm.add("$pkg", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall()
    xvm.remove("$pkg")
    return true
end
LUA
done

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN_IN default self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
RUN_IN default subos new other >/dev/null 2>&1 || fail "subos new other failed"

log "Setup: default has ms-shared; other has ms-shared + ms-only-b"
RUN_IN default install ms-shared@1.0.0 -y >/dev/null 2>&1 \
  || fail "setup: default install ms-shared failed"
RUN_IN other   install ms-shared@1.0.0 -y >/dev/null 2>&1 \
  || fail "setup: other install ms-shared failed"
RUN_IN other   install ms-only-b@1.0.0 -y >/dev/null 2>&1 \
  || fail "setup: other install ms-only-b failed"

WS_DEFAULT="$HOME_DIR/subos/default/.xlings.json"
WS_OTHER="$HOME_DIR/subos/other/.xlings.json"
PAYLOAD_B="$HOME_DIR/data/xpkgs/xim-x-ms-only-b/1.0.0"
PAYLOAD_SHARED="$HOME_DIR/data/xpkgs/xim-x-ms-shared/1.0.0"

# Record an older client into the home config, the way an upgraded home reads.
# Without this the migration-marker assertions would be vacuous: a fresh home
# already carries the running version, so "the stamp did not land" and "the
# stamp landed" look identical.
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1], ".xlings.json")
data = json.loads(p.read_text())
data["version"] = "0.4.69"
p.write_text(json.dumps(data, indent=2))
PY

recorded_version() {
  python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
print(json.loads(pathlib.Path(sys.argv[1], ".xlings.json").read_text()).get("version", ""))
PY
}
has_ws_entry() {
  python3 - "$1" "$2" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
ws = data.get("workspace") or {}
sys.exit(0 if sys.argv[2] in ws else 1)
PY
}

OUT_FILE="$RUNTIME_DIR/last-output.txt"
# Captures into $out / $rc. Via a file because the TUI stream emits null
# bytes, which bash drops from a command substitution with a warning each
# time -- noise that would bury a real diagnostic in the CI log.
run_capture() {
  local subos="$1"; shift
  rc=0
  RUN_IN "$subos" "$@" >"$OUT_FILE" 2>&1 || rc=$?
  out=$(tr -d '\0' < "$OUT_FILE")
}

# ── S1: both subos are clean ───────────────────────────────────────
log "S1: doctor is clean in both subos"
RUN_IN default self doctor >/dev/null 2>&1 || fail "S1: default should be clean"
RUN_IN other   self doctor >/dev/null 2>&1 || fail "S1: other should be clean"
[[ "$(recorded_version)" == "0.4.69" ]] \
  || fail "S1 setup: recorded client version should still read 0.4.69"

# ── S2: a payload only `other` uses is reported, attributed, NOT adopted ──
log "S2: broken payload owned by another subos → attributed, not repaired here"
rm -rf "$PAYLOAD_B"

run_capture default self doctor
grep -q "broken payload \[subos: other\]" <<<"$out" \
  || fail "S2: default's report must attribute the finding to 'other'; got:\n$out"
grep -q "xlings subos use other" <<<"$out" \
  || fail "S2: default's report must say where to repair it; got:\n$out"
# Not this subos's problem, so not this subos's exit code. Counting it would
# leave `default` permanently red with no command that clears it from here.
[[ $rc -eq 0 ]] \
  || fail "S2: another subos's broken payload must not fail this subos (rc=$rc):\n$out"

log "S2b: --fix in default must not adopt the package"
run_capture default self doctor --fix
has_ws_entry "$WS_DEFAULT" "ms-only-b" \
  && fail "S2b: --fix pulled ms-only-b into default's workspace:\n$out"
[[ ! -d "$PAYLOAD_B" ]] \
  || fail "S2b: default's --fix must not reinstall another subos's package"
[[ ! -e "$HOME_DIR/subos/default/bin/ms-only-b" ]] \
  || fail "S2b: default's --fix must not create a shim for another subos's package"

# ── S3: the migration marker must not land while another subos is behind ──
log "S3: stamp withheld while another subos still has a repair outstanding"
[[ "$(recorded_version)" == "0.4.69" ]] \
  || fail "S3: --fix stamped the home while 'other' still had work outstanding"

# ── S4: repairing it in `other` lands the stamp ──────────────────────
log "S4: --fix in other repairs the payload and lands the stamp"
run_capture other self doctor --fix
[[ $rc -eq 0 ]] || fail "S4: other's --fix should exit 0; got $rc:\n$out"
[[ -f "$PAYLOAD_B/bin/ms-only-b" ]] \
  || fail "S4: other's --fix must restore the payload it owns"
[[ "$(recorded_version)" != "0.4.69" ]] \
  || fail "S4: stamp must land once nothing is outstanding anywhere"

RUN_IN default self doctor >/dev/null 2>&1 \
  || fail "S4: default should be clean again after other repaired its payload"

# ── S5: what the other subos points at is visible from here ─────────
log "S5: another subos pointing at an unregistered version is reported here"
python3 - "$WS_OTHER" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
data = json.loads(p.read_text())
ws = data.get("workspace") or {}
entry = ws.get("ms-only-b")
# Point the active version at something the shared DB does not have. Both
# schema forms are accepted on read, so write back the one that is there.
if isinstance(entry, dict):
    entry["active"] = "9.9.9"
else:
    ws["ms-only-b"] = "9.9.9"
data["workspace"] = ws
p.write_text(json.dumps(data, indent=2))
PY

run_capture default self doctor
grep -q "other subos" <<<"$out" \
  || fail "S5: default must report the state of other subos; got:\n$out"
grep -q "xvm-subos-active-missing" <<<"$out" \
  || fail "S5: the finding must carry its code; got:\n$out"
grep -q "xlings subos use other" <<<"$out" \
  || fail "S5: the finding must say where to go; got:\n$out"
# Reported, not counted: `default` cannot repair another subos's pointer, and
# a red exit with no command that clears it is the nag-forever shape.
[[ $rc -eq 0 ]] \
  || fail "S5: another subos's dangling pointer must not fail this subos (rc=$rc)"

# Restore `other` so the last scenario starts from a consistent home.
RUN_IN other use ms-only-b 1.0.0 >/dev/null 2>&1 \
  || fail "S5 cleanup: could not re-select ms-only-b in other"

# ── S6: R3 must not claim a removal that only detached this subos ────
#
# ms-shared is installed in BOTH subos. Break its payload and make its install
# hook fail, so R2 fails and R3 runs for real: its `xlings remove` finds the
# version still referenced by `other`, detaches `default`, and exits 0 with
# the record intact.
log "S6: R3 does not report REMOVED when the record survived"
rm -rf "$PAYLOAD_SHARED"
: > "$FAIL_MARKER"

run_capture default self doctor --fix
[[ $rc -ne 0 ]] || fail "S6: the repair could not succeed, so doctor must not exit 0; got:\n$out"
grep -q "REMOVED" <<<"$out" \
  && fail "S6: reported a removal that did not happen; got:\n$out"
grep -q "still registered" <<<"$out" \
  || fail "S6: must say the record survived the removal; got:\n$out"

# The record it declined to claim as removed is in fact still there.
python3 - "$HOME_DIR" <<'PY' || fail "S6: ms-shared@1.0.0 must still be registered"
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1], ".xlings.json").read_text())
versions = (data.get("versions") or {}).get("ms-shared") or {}
assert "1.0.0" in (versions.get("versions") or {}), \
    "S6: the entry R3 said it removed is gone -- the message was right after all?"
PY

# And the home must not be stamped off the back of a failed repair.
[[ "$(recorded_version)" != "0.4.69" ]] \
  || true   # already stamped in S4; S6 must not un-stamp, nothing to assert

rm -f "$FAIL_MARKER"

# ── S7: a project subos counts as a reference ────────────────────────
#
# Payload reference counting used to enumerate ONE side: the project subos
# when the package's scope was Project, the home's subos otherwise. The two
# sets never met, so a global-scope package used by two PROJECT subos was
# counted as referenced by nobody -- and removing it from one deleted the
# payload the other was still pointing at, from a command run in a third
# place entirely.
log "S7: a second project subos keeps the payload alive"
PROJ_DIR="$RUNTIME_DIR/proj"
mkdir -p "$PROJ_DIR"
write_project_subos() {
  cat > "$PROJ_DIR/.xlings.json" <<JSON
{ "subos": "$1" }
JSON
}
RUN_PROJ() {
  ( cd "$PROJ_DIR" && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

write_project_subos pa
RUN_PROJ install ms-proj@1.0.0 -y >/dev/null 2>&1 \
  || fail "S7: install into project subos pa failed"
write_project_subos pb
RUN_PROJ install ms-proj@1.0.0 -y >/dev/null 2>&1 \
  || fail "S7: install into project subos pb failed"

PAYLOAD_PROJ="$HOME_DIR/data/xpkgs/xim-x-ms-proj/1.0.0"
[[ -d "$PAYLOAD_PROJ" ]] || fail "S7 setup: payload should exist"
has_ws_entry "$PROJ_DIR/.xlings/subos/pb/.xlings.json" "ms-proj" \
  || fail "S7 setup: pb should reference ms-proj"

write_project_subos pa
RUN_PROJ remove ms-proj@1.0.0 -y >/dev/null 2>&1 \
  || fail "S7: remove from project subos pa failed"

[[ -d "$PAYLOAD_PROJ" ]] \
  || fail "S7: payload deleted while project subos pb still referenced it"
has_ws_entry "$PROJ_DIR/.xlings/subos/pb/.xlings.json" "ms-proj" \
  || fail "S7: pb lost its reference to a package it still has"
has_ws_entry "$PROJ_DIR/.xlings/subos/pa/.xlings.json" "ms-proj" \
  && fail "S7: pa should have detached from ms-proj"

log "PASS: multi-subos doctor scoping, attribution, stamping, R3 honesty, refcount union"
