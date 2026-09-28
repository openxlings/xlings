#!/usr/bin/env bash
# E2E: an install runs a package's config() once per scope and revision, and
# `--reconfig` runs it again (openxlings/xlings#632 §1).
#
# `install` of a package already installed used to re-run config() for every
# node of its closure: 89 nodes and ~85 s for one desktop app, nothing
# downloaded, nothing changed, nothing printed. A payload in the store is a HOME
# fact; having been configured is a fact about ONE scope, so it is recorded in
# that scope (`configured` in the subos file) and checked against the recipe's
# revision and the scope's installed[].
#
#   C1  a fresh install configures, records the revision, and says so per node;
#       a later node's hook can run an earlier node's command by name through
#       the routing table (it is rebuilt per node, not once per plan)
#   C2  installing it again configures nothing, says how to, exits 0, and the
#       interface still reports already_present
#   C3  `--reconfig` configures the whole closure again
#   C4  a second subos configures its own copy (the record is per scope)
#   C5  a revision bump reinstalls in one subos AND reconfigures the other on
#       its next install -- the payload is shared, the record is not
#   C6  remove, then install: configures again (the record left with it)
#   C7  a subos file written before the record configures once, then records
#   C8  a privileged environment declaration is announced when it is new or
#       changed, not every time config() records it again (#632 §4)
#
# Offline: hook-only fixtures.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/install_configured_record"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
COUNTS="$RUNTIME_DIR/config-runs.log"

cleanup() {
  chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN_IN() {
  local subos="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS="$subos" \
      XLINGS_NON_INTERACTIVE=1 \
      "$XLINGS_BIN" "$@" )
}

# write_recipes <parent revision>
#   cfgdep     registers a command, `cfgdep-tool`, and counts its config runs
#   cfgparent  depends on cfgdep, runs `cfgdep-tool` BY NAME in its install
#              hook, and counts its config runs
write_recipes() {
  local rev="$1"
  mkdir -p "$LOCAL_INDEX_DIR/pkgs/c"
  cat > "$LOCAL_INDEX_DIR/pkgs/c/cfgdep.lua" <<LUA
package = {
    spec = "1", name = "cfgdep", description = "configured-record fixture (dep)",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "aarch64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux  = { ["1.0"] = {} },
        macosx = { ["1.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.mkdir(bindir)
    local tool = path.join(bindir, "cfgdep-tool")
    io.writefile(tool, "#!/bin/sh\\necho cfgdep-tool-ran\\n")
    os.exec("chmod +x " .. tool)
    return true
end
function config()
    local f = io.open("$COUNTS", "a"); f:write("cfgdep\\n"); f:close()
    xvm.add("cfgdep-tool", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall() xvm.remove("cfgdep-tool") return true end
LUA
  cat > "$LOCAL_INDEX_DIR/pkgs/c/cfgparent.lua" <<LUA
package = {
    spec = "1", name = "cfgparent", description = "configured-record fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "aarch64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux  = { deps = { "cfgdep" }, ["1.0"] = { revision = $rev } },
        macosx = { deps = { "cfgdep" }, ["1.0"] = { revision = $rev } },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.mkdir(bindir)
    -- The dep was installed earlier in THIS plan; its command must already be
    -- routable by name.
    local out = os.iorun("cfgdep-tool")
    io.writefile(path.join(bindir, "cfgparent"), "cfgparent-r$rev " .. (out or "") .. "\\n")
    return true
end
function config()
    local f = io.open("$COUNTS", "a"); f:write("cfgparent\\n"); f:close()
    xvm.add("cfgparent", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall() xvm.remove("cfgparent") return true end
LUA
  rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
}

runs_of() { grep -cx "$1" "$COUNTS" 2>/dev/null || true; }
expect_runs() {  # <package> <count> <scenario>
  local got; got="$(runs_of "$1")"
  [[ "$got" == "$2" ]] || fail "$3: $1 configured $got time(s), expected $2"
}
record_of() {  # <subos> <key> -> the recorded revision, or "absent"
  python3 - "$HOME_DIR/subos/$1/.xlings.json" "$2" <<'PY'
import json, sys
try:
    doc = json.load(open(sys.argv[1]))
except (OSError, ValueError):
    print("absent"); sys.exit(0)
print(doc.get("configured", {}).get(sys.argv[2], "absent"))
PY
}

# A package whose config declares a privileged variable (C8). Written before
# the first sync, like the others; C8 only changes its value.
write_env_recipe() {  # <value>
  cat > "$LOCAL_INDEX_DIR/pkgs/c/cfgenv.lua" <<LUA
package = {
    spec = "1", name = "cfgenv", description = "privileged env fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "aarch64"}, status = "stable", categories = {"test-fixture"},
    xpm = { linux = { ["1.0"] = {} }, macosx = { ["1.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
import("xim.libxpkg.subos")
function install()
    os.mkdir(path.join(pkginfo.install_dir(), "lib"))
    io.writefile(path.join(pkginfo.install_dir(), "lib", "marker"), "x\n")
    return true
end
function config()
    subos.env({ var = "LIBGL_DRIVERS_PATH", op = "prepend", value = "$1" })
    return true
end
function uninstall() return true end
LUA
  rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
}
mkdir -p "$HOME_DIR/subos/default/bin"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
write_recipes 0
write_env_recipe '${subosdir}/usr/lib/dri'

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON
RUN_IN default self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ── C1 ───────────────────────────────────────────────────────────────
log "C1: a fresh install configures each node once and records it"
OUT="$(RUN_IN default install cfgparent@1.0 -y 2>&1)" \
  || fail "C1: install failed:
$OUT"
expect_runs cfgdep 1 C1
expect_runs cfgparent 1 C1
[[ "$(record_of default xim:cfgparent@1.0)" == "0" ]] \
  || fail "C1: no record of cfgparent's configuration: $(record_of default xim:cfgparent@1.0)"
[[ "$(record_of default xim:cfgdep@1.0)" == "0" ]] \
  || fail "C1: no record of cfgdep's configuration"
grep -q "installed xim:cfgparent@1.0" <<<"$OUT" \
  || fail "C1: no per-node line for cfgparent:
$OUT"
grep -q "cfgdep-tool-ran" "$HOME_DIR/data/xpkgs/xim-x-cfgparent/1.0/bin/cfgparent" \
  || fail "C1: cfgparent's install hook could not run cfgdep-tool by name"
log "  ok"

# ── C2 ───────────────────────────────────────────────────────────────
log "C2: installing it again configures nothing"
OUT="$(RUN_IN default install cfgparent@1.0 -y 2>&1)" \
  || fail "C2: a no-op install failed:
$OUT"
expect_runs cfgdep 1 C2
expect_runs cfgparent 1 C2
grep -q "is already installed" <<<"$OUT" || fail "C2: no 'already installed':
$OUT"
grep -q "xlings install xim:cfgparent@1.0 --reconfig" <<<"$OUT" \
  || fail "C2: the way to configure again is not printed:
$OUT"
if grep -qE "\[[0-9]+/[0-9]+\] (installed|configured)" <<<"$OUT"; then
  fail "C2: a no-op install narrated per-node work:
$OUT"
fi
T="$(RUN_IN default interface install_packages \
      --args '{"targets":["cfgparent@1.0"],"yes":true}' 2>/dev/null)" \
  || fail "C2: interface install failed"
python3 - "$T" <<'PY' || fail "C2: the interface report is not already_present"
import json, sys
for line in sys.argv[1].splitlines():
    if not line.startswith("{"): continue
    e = json.loads(line)
    if e.get("dataKind") == "install_targets":
        status = e["payload"]["targets"][0]["status"]
        sys.exit(0 if status == "already_present" else "status " + status)
sys.exit("no install_targets")
PY
expect_runs cfgparent 1 "C2 (interface)"
log "  ok"

# ── C3 ───────────────────────────────────────────────────────────────
log "C3: --reconfig configures the closure again"
RUN_IN default install cfgparent@1.0 -y --reconfig >/dev/null 2>&1 \
  || fail "C3: --reconfig install failed"
expect_runs cfgdep 2 C3
expect_runs cfgparent 2 C3
log "  ok"

# ── C4 ───────────────────────────────────────────────────────────────
log "C4: a second subos configures its own copy"
RUN_IN default subos new other >/dev/null 2>&1 || fail "C4: subos new other failed"
OUT="$(RUN_IN other install cfgparent@1.0 -y 2>&1)" \
  || fail "C4: install in other failed:
$OUT"
expect_runs cfgdep 3 C4
expect_runs cfgparent 3 C4
[[ "$(record_of other xim:cfgparent@1.0)" == "0" ]] || fail "C4: no record in other"
grep -q "configured xim:cfgparent@1.0" <<<"$OUT" \
  || fail "C4: mapping into a subos is not narrated as configured:
$OUT"
RUN_IN other install cfgparent@1.0 -y >/dev/null 2>&1 || fail "C4: second install in other failed"
expect_runs cfgparent 3 "C4 (again)"
log "  ok"

# ── C5 ───────────────────────────────────────────────────────────────
log "C5: a revision bump reaches the other subos on its next install"
write_recipes 1
RUN_IN default install cfgparent@1.0 -y >/dev/null 2>&1 || fail "C5: reinstall in default failed"
expect_runs cfgparent 4 "C5 (default)"
[[ "$(record_of default xim:cfgparent@1.0)" == "1" ]] || fail "C5: default's record not at revision 1"
expect_runs cfgdep 3 "C5 (dep untouched)"
RUN_IN other install cfgparent@1.0 -y >/dev/null 2>&1 || fail "C5: install in other failed"
expect_runs cfgparent 5 "C5 (other)"
[[ "$(record_of other xim:cfgparent@1.0)" == "1" ]] || fail "C5: other's record not at revision 1"
log "  ok"

# ── C6 ───────────────────────────────────────────────────────────────
log "C6: remove takes the record with it"
RUN_IN other remove cfgparent -y >/dev/null 2>&1 || fail "C6: remove in other failed"
[[ "$(record_of other xim:cfgparent@1.0)" == "absent" ]] \
  || fail "C6: the record survived the removal: $(record_of other xim:cfgparent@1.0)"
RUN_IN other install cfgparent@1.0 -y >/dev/null 2>&1 || fail "C6: reinstall in other failed"
expect_runs cfgparent 6 C6
log "  ok"

# ── C7 ───────────────────────────────────────────────────────────────
log "C7: a subos file from before the record configures once, then records"
python3 - "$HOME_DIR/subos/default/.xlings.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
doc.pop("configured", None)
open(sys.argv[1], "w").write(json.dumps(doc, indent=2))
PY
RUN_IN default install cfgparent@1.0 -y >/dev/null 2>&1 || fail "C7: install failed"
expect_runs cfgparent 7 C7
[[ "$(record_of default xim:cfgparent@1.0)" == "1" ]] || fail "C7: no record written"
RUN_IN default install cfgparent@1.0 -y >/dev/null 2>&1 || fail "C7: second install failed"
expect_runs cfgparent 7 "C7 (again)"
log "  ok"

# ── C8 ───────────────────────────────────────────────────────────────
warnings_in() { grep -c "declares LIBGL_DRIVERS_PATH" <<<"$1" || true; }
log "C8: a privileged env declaration is announced when new or changed only"
OUT="$(RUN_IN default install cfgenv@1.0 -y 2>&1)" || fail "C8: install failed:
$OUT"
[[ "$(warnings_in "$OUT")" == "1" ]] || fail "C8: the first declaration was not announced once:
$OUT"
OUT="$(RUN_IN default install cfgenv@1.0 -y --reconfig 2>&1)" || fail "C8: reconfig failed"
[[ "$(warnings_in "$OUT")" == "0" ]] || fail "C8: an identical re-record was announced again:
$OUT"
write_env_recipe '${subosdir}/usr/lib/dri2'
OUT="$(RUN_IN default install cfgenv@1.0 -y --reconfig 2>&1)" || fail "C8: reconfig failed"
[[ "$(warnings_in "$OUT")" == "1" ]] || fail "C8: a changed declaration was not announced:
$OUT"
log "  ok"

log "PASS: install_configured_record"
