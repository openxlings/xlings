#!/usr/bin/env bash
# E2E: `xlings self doctor` verifies the workspace ↔ shim file invariant
# and `--fix` repairs detected inconsistencies.
#
# The invariant: for every program N in the workspace with non-empty
# version, a shim file at <binDir>/<N> must exist; conversely, every
# program-typed shim file under binDir must have a workspace entry.
#
# Scenarios:
#   1. Clean state                              → exit 0, "OK"
#   2. Corrupt: delete a shim manually          → doctor reports missing
#   3. --fix recreates the missing shim         → exit 0 after fix
#   4. Corrupt: drop a stray shim under binDir
#      (not present in versions DB)             → ignored (not ours)
#   5. Corrupt: registered program shim with no
#      workspace entry                          → doctor reports orphan
#   6. --fix removes the orphan                 → exit 0 after fix
#   7. Corrupt: delete the payload dir           → doctor reports broken
#   8. --fix --dry-run                           → prints plan, changes nothing
#   8b. --fix                                    → repairs, verified by re-detect
#   8c. second --fix                             → nothing left to do

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/self_doctor"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/d/doctor-fixture.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR"

# Private copy of the shared fixture index, neutralise sub-index repos.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

# Fixture: a single-version program named "doctor-fixture" that drops
# a printable script at <bindir>/doctor-fixture so a shim can be created.
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "doctor-fixture",
    description = "Local fixture for tests/e2e/self_doctor_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "doctor-fixture"),
                 "#!/bin/sh\necho doctor-fixture@" .. pkginfo.version() .. "\n")
    return true
end

function config()
    xvm.add("doctor-fixture", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("doctor-fixture")
    return true
end
LUA

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF

log "Initializing sandbox XLINGS_HOME at $HOME_DIR"
RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# Install the fixture so we have a real workspace + shim to mess with.
RUN install doctor-fixture@1.0.0 -y >/dev/null 2>&1 \
  || fail "setup: install failed"

SHIM="$HOME_DIR/subos/default/bin/doctor-fixture"
[[ -e "$SHIM" ]] || fail "setup: shim should exist after install"

# ── S1: clean state → exit 0 ────────────────────────────────────────
log "S1: doctor on clean state → exit 0"
RUN self doctor >/dev/null 2>&1 || fail "S1: doctor should report OK on clean state"

# ── S2: delete shim manually → doctor reports missing ──────────────
log "S2: delete shim, doctor (no --fix) → non-zero, reports missing"
rm -f "$SHIM"
[[ ! -e "$SHIM" ]] || fail "S2 setup: shim should be gone"
out=$(RUN self doctor 2>&1) || rc=$?; rc=${rc:-0}
[[ $rc -ne 0 ]] || fail "S2: doctor should exit non-zero when shim missing (got 0)"
echo "$out" | grep -q "missing shim" \
  || fail "S2: output should mention 'missing shim'; got:\n$out"

# ── S3: --fix recreates the missing shim ───────────────────────────
log "S3: doctor --fix recreates missing shim → exit 0"
RUN self doctor --fix >/dev/null 2>&1 || fail "S3: doctor --fix should succeed"
[[ -e "$SHIM" ]] || fail "S3: shim should be recreated by --fix"

# Re-run doctor without --fix to confirm clean state restored
RUN self doctor >/dev/null 2>&1 || fail "S3: post-fix doctor should be clean"

# ── S4: stray file under binDir not in versions DB → ignored ───────
log "S4: stray file under binDir not in versions DB → ignored"
STRAY="$HOME_DIR/subos/default/bin/some-random-tool"
echo '#!/bin/sh' > "$STRAY"
chmod +x "$STRAY"
RUN self doctor >/dev/null 2>&1 \
  || fail "S4: doctor should ignore files not registered in versions DB"
[[ -e "$STRAY" ]] || fail "S4: doctor should NOT touch unregistered files"
rm -f "$STRAY"

# ── S5: orphan registered shim (workspace entry cleared manually) ──
log "S5: orphan shim → doctor reports orphan"
# Manipulate the workspace JSON to remove only the doctor-fixture entry,
# leaving the shim file in place.
python3 - "$HOME_DIR" <<'PY'
import json, sys, pathlib
home = sys.argv[1]
ws_path = pathlib.Path(home, "subos/default/.xlings.json")
data = json.loads(ws_path.read_text())
ws = data.get("workspace") or {}
ws.pop("doctor-fixture", None)
data["workspace"] = ws
ws_path.write_text(json.dumps(data))
PY

[[ -e "$SHIM" ]] || fail "S5 setup: shim should still exist"
out=$(RUN self doctor 2>&1) || rc=$?; rc=${rc:-0}
[[ $rc -ne 0 ]] || fail "S5: doctor should exit non-zero on orphan (got 0)"
echo "$out" | grep -q "orphan shim" \
  || fail "S5: output should mention 'orphan shim'; got:\n$out"

# ── S6: --fix removes orphan shim ──────────────────────────────────
log "S6: doctor --fix removes orphan shim → exit 0"
RUN self doctor --fix >/dev/null 2>&1 || fail "S6: doctor --fix should succeed"
[[ ! -e "$SHIM" ]] || fail "S6: --fix should remove orphan shim"

# Reset state for payload-layer scenarios: re-install + ensure shim+workspace.
RUN install doctor-fixture@1.0.0 -y >/dev/null 2>&1 \
  || fail "reset: re-install before S7 failed"
RUN use doctor-fixture 1.0.0 >/dev/null 2>&1 || fail "reset: use failed"
[[ -e "$SHIM" ]] || fail "reset: shim should exist after re-install"

# ── S7: quick keeps the finding; deep resolves the package remedy ─────
log "S7: rm active payload → quick reports it without deep remedy resolution"
PAYLOAD_DIR="$HOME_DIR/data/xpkgs/xim-x-doctor-fixture/1.0.0"
[[ -d "$PAYLOAD_DIR" ]] || fail "S7 setup: payload dir should exist"
rm -rf "$PAYLOAD_DIR"

rc=0
out=$(RUN self doctor 2>&1) || rc=$?
[[ $rc -ne 0 ]] || fail "S7: doctor should exit non-zero on broken payload (got 0)"
echo "$out" | grep -q "broken payload" \
  || fail "S7: output should mention 'broken payload'; got:\n$out"
echo "$out" | grep -q "active" \
  || fail "S7: output should mark active version with [active] tag"
echo "$out" | grep -qE "xlings install (xim:)?doctor-fixture@1\.0\.0" \
  && fail "S7: quick doctor must not resolve package remedies; got:\n$out"

rc=0
out=$(RUN self doctor --deep 2>&1) || rc=$?
[[ $rc -ne 0 ]] || fail "S7: deep doctor should still report the broken payload"
# The remedy names the PACKAGE, with its namespace, because that is what
# `xlings install` takes -- a finding names an xvm target and those are not the
# same thing (`nm@20.1.7` is a program llvm registers, not a package).
echo "$out" | grep -qE "xlings install (xim:)?doctor-fixture@1\.0\.0" \
  || fail "S7: output should include the remediation command; got:\n$out"

# ── S8: --fix --dry-run previews the repair and changes NOTHING ──────
#
# `--fix` used to refuse to repair broken payloads and only re-print the
# remediation command. That refusal did not survive contact with an upgraded
# home: 0.4.69 records a headers-only package as one program-typed entry with
# no executable in it, so the new client reports every such package as a
# broken payload -- 56 findings on a five-package home, each with a printed
# command that WAS the correct cure. Handing the user 56 commands to paste is
# not a diagnosis.
#
# The promise that replaced "never touches the network" is this flag, so it is
# pinned first: the plan is printed, and the state is byte-for-byte untouched.
log "S8: doctor --fix --dry-run → prints the plan, repairs nothing"
rc=0
out=$(RUN self doctor --fix --dry-run 2>&1) || rc=$?
[[ $rc -ne 0 ]] || fail "S8: --dry-run should still exit non-zero (nothing repaired)"
echo "$out" | grep -q "would run" \
  || fail "S8: --dry-run should print the planned command; got:\n$out"
[[ ! -d "$PAYLOAD_DIR" ]] \
  || fail "S8: --dry-run must NOT recreate the payload"

python3 - "$HOME_DIR" <<'PY' || fail "S8: --dry-run must NOT modify versions DB"
import json, sys, pathlib
home = sys.argv[1]
data = json.loads(pathlib.Path(home, ".xlings.json").read_text())
assert "doctor-fixture" in (data.get("versions") or {}), \
    "S8: --dry-run must NOT remove doctor-fixture from versions DB"
ws = json.loads(pathlib.Path(home, "subos/default/.xlings.json").read_text())
entry = (ws.get("workspace") or {}).get("doctor-fixture")
active = entry.get("active") if isinstance(entry, dict) else entry
assert active == "1.0.0", \
    f"S8: --dry-run must NOT clear workspace pointer; got active={active!r}"
PY
[[ -e "$SHIM" ]] || fail "S8: --dry-run must NOT remove the shim file"

# ── S8b: --fix repairs it, and the repair is VERIFIED, not claimed ────
#
# The rung that does the work here is re-register: `xlings install
# <pkg>@<ver>`, which the installer turns into a real install because the
# payload is gone. What makes this a test rather than a smoke check is that
# doctor re-detects from a reloaded state file afterwards -- a rung reporting
# success while the finding survives is the failure shape this codebase keeps
# producing, and here it would turn a still-broken home into `status OK`.
log "S8b: doctor --fix → repairs the broken payload → exit 0"
rc=0
out=$(RUN self doctor --fix 2>&1) || rc=$?
[[ $rc -eq 0 ]] || fail "S8b: --fix should repair the payload and exit 0; got $rc:\n$out"
[[ -d "$PAYLOAD_DIR/bin" ]] \
  || fail "S8b: --fix must recreate the payload bin/"
[[ -f "$PAYLOAD_DIR/bin/doctor-fixture" ]] \
  || fail "S8b: --fix must recreate the payload binary"
[[ -e "$SHIM" ]] || fail "S8b: --fix must leave the shim in place"

RUN self doctor >/dev/null 2>&1 \
  || fail "S8b: doctor should report OK after --fix repaired the payload"

# ── S8c: a second --fix finds nothing ────────────────────────────────
#
# A ladder that repairs the same thing on every run is looping, not
# converging, and one pass cannot tell the difference.
log "S8c: second doctor --fix → nothing left to repair"
rc=0
out=$(RUN self doctor --fix 2>&1) || rc=$?
[[ $rc -eq 0 ]] || fail "S8c: second --fix should exit 0; got $rc:\n$out"
echo "$out" | grep -q "repaired\|would run" \
  && fail "S8c: second --fix should have had nothing to do; got:\n$out"

# ── S8d: payload present, OTHER executables present, ours missing ────
#
# The discriminator behind the "release anchor" classification. A package that
# ships no executable at all is not a broken program -- it is a library-only
# package that xvm typed as a program, and reinstalling it can never change
# that. But a payload that HAS executables and is merely missing the one we
# name is genuinely broken, and must not be swept into the same bucket.
#
# Without this case the anchor rule is unfalsifiable: S7 deletes the whole
# payload directory, which is caught by an earlier check and never reaches it.
log "S8d: payload with other executables but ours missing → still broken"
rm -f "$PAYLOAD_DIR/bin/doctor-fixture"
printf '#!/bin/sh\necho sibling\n' > "$PAYLOAD_DIR/bin/some-other-tool"
chmod +x "$PAYLOAD_DIR/bin/some-other-tool"

rc=0
out=$(RUN self doctor 2>&1) || rc=$?
[[ $rc -ne 0 ]] \
  || fail "S8d: a missing binary next to other executables must stay an error"
echo "$out" | grep -q "broken payload" \
  || fail "S8d: should still report 'broken payload', not a release anchor; got:\n$out"
echo "$out" | grep -q "release anchor  doctor-fixture" \
  && fail "S8d: must NOT reclassify a real breakage as a release anchor"

# put it back for the scenarios that follow
RUN install doctor-fixture@1.0.0 -y >/dev/null 2>&1 \
  || fail "S8d cleanup: reinstall should succeed"
rm -f "$PAYLOAD_DIR/bin/some-other-tool"
RUN self doctor >/dev/null 2>&1 || fail "S8d cleanup: doctor should be clean again"

# ── Setup for alias-mode scenarios: a fixture with vdata.alias set ──
# Inject a new fixture file. The catalog cache was warm from the earlier
# scenarios and won't pick this up automatically — invalidate so `install`
# rebuilds the index from disk and sees alias-fixture.lua.
ALIAS_PKG="$LOCAL_INDEX_DIR/pkgs/d/alias-fixture.lua"
mkdir -p "$(dirname "$ALIAS_PKG")"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
rm -f "$HOME_DIR/data/xim-pkgindex/.xlings-index-cache.json" 2>/dev/null || true
cat > "$ALIAS_PKG" <<'LUA'
package = {
    spec = "1",
    name = "alias-fixture",
    description = "fixture for alias-mode doctor checks",
    type = "package",
    archs = {"x86_64"},
    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    -- Real binary that the alias points to: alias-real.
    --
    -- It has to be EXECUTABLE, not merely present. doctor exempts a payload
    -- that ships no executable at all ("this package has no program of its
    -- own"), so a fixture whose only file is unreadable as a program takes
    -- that exemption and every alias-mode assertion below passes for the
    -- wrong reason -- verified by mutation: without the chmod, deleting the
    -- alias branch under test left S11 green.
    io.writefile(path.join(bindir, "alias-real"),
                 "#!/bin/sh\necho alias-real\n")
    if os.host() == "windows" then
        io.writefile(path.join(bindir, "alias-real.bat"), "@echo alias-real\n")
    else
        os.exec("chmod +x " .. path.join(bindir, "alias-real"))
    end
    return true
end

function config()
    -- alias mode: register alias-fixture with alias = "alias-real"
    xvm.add("alias-fixture", {
        bindir = path.join(pkginfo.install_dir(), "bin"),
        alias  = "alias-real",
    })
    return true
end

function uninstall()
    xvm.remove("alias-fixture")
    return true
end
LUA

log "S9: alias resolves in payload → doctor OK"
RUN install alias-fixture@1.0.0 -y >/dev/null 2>&1 \
  || fail "S9 setup: install alias-fixture failed"
RUN self doctor >/dev/null 2>&1 \
  || fail "S9: alias resolves correctly, doctor should be OK"

# ── S10: the three tiers below the payload ─────────────────────────
#
# The runtime resolves an alias through four places, in order: the payload,
# the payload's bin, the subos bin dir, and the inherited PATH. doctor used to
# ask only about the first two and call everything past them one warning, so a
# package whose aliases name a sibling package's command — thirty of mcpp's
# short commands do — was reported broken on every run while working
# perfectly, and a genuinely unresolvable alias was one indistinguishable line
# among them.
#
# RUN() supplies `PATH=/usr/bin:/bin`, so what the host can satisfy is fixed
# and these three cases are deterministic.
ALIAS_PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-alias-fixture/1.0.0/bin/alias-real"
SUBOS_BIN="$HOME_DIR/subos/default/bin"

log "S10a: alias target only in the subos bin dir → silent, exit 0"
rm -f "$ALIAS_PAYLOAD"
printf '#!/bin/sh\necho sibling\n' > "$SUBOS_BIN/alias-real"
chmod +x "$SUBOS_BIN/alias-real"
rc=0
out=$(RUN self doctor 2>&1) || rc=$?
echo "$out" | grep -q "alias unresolved" \
  && fail "S10a: a sibling command in the subos bin dir resolves at runtime; got:\n$out"
[[ $rc -eq 0 ]] || fail "S10a: nothing is wrong, expected exit 0; got $rc"
rm -f "$SUBOS_BIN/alias-real"

log "S10b: alias target only on the host PATH → notice, exit 0"
# `sh` is on /bin in every environment RUN() constructs.
python3 - "$HOME_DIR" <<'PY' || fail "S10b setup: could not repoint the alias"
import json, pathlib, sys
p = pathlib.Path(sys.argv[1], ".xlings.json")
data = json.loads(p.read_text())
data["versions"]["alias-fixture"]["versions"]["1.0.0"]["alias"] = ["sh"]
p.write_text(json.dumps(data, indent=2))
PY
rc=0
out=$(RUN self doctor 2>&1) || rc=$?
echo "$out" | grep -q "alias unresolved" \
  && fail "S10b: a host command satisfies the alias; that is a notice, not a defect; got:\n$out"
[[ $rc -eq 0 ]] || fail "S10b: a host-satisfied alias must not set the exit code; got $rc"
RUN self doctor --all 2>&1 | grep -q "host alias" \
  || fail "S10b: --all should name the alias the host is satisfying"

log "S10c: alias target nowhere at all → error, exit 1"
python3 - "$HOME_DIR" <<'PY' || fail "S10c setup: could not repoint the alias"
import json, pathlib, sys
p = pathlib.Path(sys.argv[1], ".xlings.json")
data = json.loads(p.read_text())
data["versions"]["alias-fixture"]["versions"]["1.0.0"]["alias"] = ["alias-real"]
p.write_text(json.dumps(data, indent=2))
PY
rc=0
out=$(RUN self doctor 2>&1) || rc=$?
echo "$out" | grep -q "alias unresolved" \
  || fail "S10c: an alias with nothing to exec must be reported; got:\n$out"
echo "$out" | grep -q "resolves to nothing" \
  || fail "S10c: the finding should say what was searched; got:\n$out"
# The behaviour change this scenario exists to pin: it used to be a warning
# that exited 0, which is how it stayed invisible among the false ones.
[[ $rc -eq 1 ]] || fail "S10c: an unresolvable alias is an error; expected exit 1, got $rc"

# ── S11: repairing an alias-mode entry is reported as repaired ─────
#
# The re-detect that decides whether a repair worked used to ask only
# `resolve_executable(<target>)` -- check 3's NON-alias branch. An alias-mode
# entry has no executable of its own by construction, so every one of them came
# back `✗ repair failed` after a repair that had in fact worked, and doctor
# exited 1 on a home with nothing left to fix.
#
# Not a fixture-only shape: measured against the real index, `patchelf`
# registers `elfpatch` with `alias = "patchelf"`, so deleting that payload and
# running `--fix` restored it and then reported the restore as a failure.
log "S11: --fix on an alias-mode entry → healed, not 'repair failed'"
ALIAS_PAYLOAD_DIR="$HOME_DIR/data/xpkgs/xim-x-alias-fixture/1.0.0"
rm -rf "$ALIAS_PAYLOAD_DIR"
rc=0
out=$(RUN self doctor 2>&1) || rc=$?
[[ $rc -ne 0 ]] || fail "S11 setup: a missing payload directory must be an error"

rc=0
out=$(RUN self doctor --fix 2>&1) || rc=$?
[[ -f "$ALIAS_PAYLOAD_DIR/bin/alias-real" ]] \
  || fail "S11: --fix should have restored the payload; got:\n$out"
echo "$out" | grep -q "repair failed" \
  && fail "S11: the repair worked — reporting it as failed is the bug:\n$out"
[[ $rc -eq 0 ]] \
  || fail "S11: --fix repaired everything, so doctor must exit 0; got $rc:\n$out"

# ── S12: a long report prints in full ──────────────────────────────
# Regression: the panel renderer asked ftxui for Dimension::Fit(doc), whose
# extend_beyond_screen parameter defaults to false -- the fitted height gets
# clamped to the terminal's, and with stdout on a pipe ftxui reports a
# default 80x24. Every row past 24 was dropped with no marker. Findings
# print first and the totals last, so what got cut was exactly the summary
# telling the user how bad it is.
log "S12: report longer than a screen prints past row 24, summary included"
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
home = sys.argv[1]
p = pathlib.Path(home, ".xlings.json")
data = json.loads(p.read_text())
versions = data.setdefault("versions", {})
# 30 registered versions whose payload directory does not exist -> 30
# `broken payload` findings, comfortably past the old 24-row ceiling.
entry = versions.setdefault("bulk-fixture", {"type": "program", "versions": {}})
for i in range(30):
    entry["versions"][f"1.0.{i}"] = {
        "path": f"{home}/data/xpkgs/xim-x-bulk-fixture/1.0.{i}/bin",
    }
p.write_text(json.dumps(data))
PY

rc=0
out=$(RUN self doctor 2>&1) || rc=$?
lines=$(printf '%s\n' "$out" | wc -l)
[[ $lines -gt 24 ]] \
  || fail "S12: report was clamped to a screen — got $lines lines, expected > 24"
printf '%s\n' "$out" | grep -q "broken payloads" \
  || fail "S12: the summary line must survive a long report; got $lines lines:\n$out"

log "PASS: self doctor scenarios 1-12 (alias repair reported honestly, long report not clamped)"
