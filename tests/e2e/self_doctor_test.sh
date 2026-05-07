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

# ── S7: broken payload (active version) → doctor reports broken + hint ─
log "S7: rm xpkgs payload while active → doctor reports broken + actionable hint"
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
echo "$out" | grep -q "xlings install doctor-fixture@1.0.0" \
  || fail "S7: output should include the remediation command; got:\n$out"

# ── S8: --fix MUST NOT touch broken-payload state (doctor never modifies
#       payload metadata). Workspace + shim + DB entry must be unchanged
#       compared to S7. The hint guides users to manual remove+install.
log "S8: doctor --fix on broken payload → does NOT deregister; only re-prints hint"
rc=0
out=$(RUN self doctor --fix 2>&1) || rc=$?
[[ $rc -ne 0 ]] || fail "S8: --fix should still exit non-zero when broken remains (got 0)"
echo "$out" | grep -q "broken payload" \
  || fail "S8: --fix output should still report broken payload"
echo "$out" | grep -q "xlings install doctor-fixture@1.0.0" \
  || fail "S8: --fix output should still print remediation command"

# Confirm doctor preserved metadata (workspace + DB entry + shim file).
python3 - "$HOME_DIR" <<'PY' || fail "S8: --fix must NOT modify versions DB"
import json, sys, pathlib
home = sys.argv[1]
data = json.loads(pathlib.Path(home, ".xlings.json").read_text())
assert "doctor-fixture" in (data.get("versions") or {}), \
    "S8: --fix must NOT remove doctor-fixture from versions DB"
ws = json.loads(pathlib.Path(home, "subos/default/.xlings.json").read_text())
entry = (ws.get("workspace") or {}).get("doctor-fixture")
# 0.4.19+: workspace value is {active, installed[]} dict; tolerate both forms.
active = entry.get("active") if isinstance(entry, dict) else entry
assert active == "1.0.0", \
    f"S8: --fix must NOT clear workspace pointer; got active={active!r}"
PY
[[ -e "$SHIM" ]] || fail "S8: --fix must NOT remove shim file (only hints user to install)"

# ── S8c: user follows the hint (`xlings install <pkg>@<ver>`)
#         → installer detects payload missing, re-runs install hook,
#         → next doctor reports OK
#         (validates the installer-side trust-but-verify on the xvm-DB
#          shortcut: a DB-registered version with no payload on disk
#          must NOT be treated as "already installed".)
log "S8c: user follows hint (xlings install) → installer self-heals → doctor OK"
RUN install doctor-fixture@1.0.0 -y >/dev/null 2>&1 \
  || fail "S8c: install (the hinted command) should succeed"
[[ -d "$PAYLOAD_DIR/bin" ]] \
  || fail "S8c: payload bin/ must be recreated by install hook"
[[ -f "$PAYLOAD_DIR/bin/doctor-fixture" ]] \
  || fail "S8c: payload binary must be recreated by install hook"
RUN self doctor >/dev/null 2>&1 \
  || fail "S8c: doctor should report OK after install self-heal"

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
    -- Real binary that the alias points to: alias-real
    io.writefile(path.join(bindir, "alias-real"),
                 "#!/bin/sh\necho alias-real\n")
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

# ── S10: alias's underlying binary missing → warning ───────────────
log "S10: rm alias target → doctor reports warning (not error)"
ALIAS_PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-alias-fixture/1.0.0/bin/alias-real"
rm -f "$ALIAS_PAYLOAD"
# Reset rc — it leaks across scenarios via the `cmd || rc=$?` pattern, so a
# successful exit here would otherwise still see the previous run's value.
rc=0
out=$(RUN self doctor 2>&1) || rc=$?
echo "$out" | grep -q "alias unresolved" \
  || fail "S10: should report 'alias unresolved' warning; got:\n$out"
# Warning-level → exit 0 (no errors; could be intentional system command)
[[ $rc -eq 0 ]] || fail "S10: alias warning alone should exit 0; got $rc"
# --fix MUST NOT touch warning-level findings
RUN self doctor --fix >/dev/null 2>&1
python3 - "$HOME_DIR" <<'PY' || fail "S10: --fix should not touch alias warning"
import json, sys, pathlib
home = sys.argv[1]
data = json.loads(pathlib.Path(home, ".xlings.json").read_text())
vers = (data.get("versions") or {}).get("alias-fixture", {}).get("versions", {})
assert "1.0.0" in vers, "S10: alias-fixture@1.0.0 should remain registered (warning is non-actionable)"
PY

log "PASS: self doctor scenarios 1-10 (S8 split into S8 fix-noop + alias S9/S10)"
