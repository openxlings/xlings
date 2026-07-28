#!/usr/bin/env bash
# subos_install_remove_isolation_test.sh — regression for the 0.4.19
# subos-blind workspace pollution: pre-fix `xlings remove pkg` in subos
# A would auto-set A.workspace[pkg] to whatever highest version EXISTED
# IN ANY SUBOS'S global versions DB, including ones A had never opted
# into.
#
# Concrete scenario that surfaced the bug:
#
#   subos default has rm-fixture@1.0.0
#   subos tmp     has rm-fixture@2.0.0
#   $ XLINGS_ACTIVE_SUBOS=tmp xlings remove rm-fixture
#
#   Pre-fix:  tmp.workspace[rm-fixture] = "1.0.0"  ← wrong! tmp never had 1.0.0
#             tmp/bin/rm-fixture        = stale shim
#   Post-fix: tmp.workspace             = {} (empty)
#             tmp/bin/rm-fixture        = removed
#
# Root cause was in installer.cppm uninstall()'s xvm-ops loop, where the
# auto-switch fallback after removing the active version picked from the
# global versions DB instead of the current subos's installed[] set.
#
# Asserts the post-fix invariants on a clean isolated XLINGS_HOME.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_install_remove_isolation"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/r/rm-fixture.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN_IN() {
  local subos="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS="$subos" \
      "$XLINGS_BIN" "$@" )
}
RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1", name = "rm-fixture",
    description = "isolation-test fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {}, ["3.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {}, ["3.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {}, ["3.0.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local d = pkginfo.install_dir()
    os.tryrm(d); os.mkdir(d)
    io.writefile(path.join(d, "VERSION"), pkginfo.version())
    return true
end
function config()
    xvm.add("rm-fixture", { bindir = pkginfo.install_dir() })
    return true
end
function uninstall()
    xvm.remove("rm-fixture")
    return true
end
LUA

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN self init >/dev/null 2>&1
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ── Setup: default has 1.0.0, tmp has 2.0.0 (different versions per subos)
log "Setup: default installs 1.0.0; tmp installs 2.0.0 (different versions)"
RUN subos new tmp >/dev/null
RUN_IN default install rm-fixture@1.0.0 -y >/dev/null 2>&1 || fail "default install failed"
RUN_IN tmp     install rm-fixture@2.0.0 -y >/dev/null 2>&1 || fail "tmp install failed"

read_active() {
  python3 - "$1" "$2" <<'PY'
import json, sys, pathlib
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
entry = (data.get("workspace") or {}).get(sys.argv[2])
if isinstance(entry, dict): print(entry.get("active", "<none>"))
elif isinstance(entry, str): print(entry)
else: print("<none>")
PY
}

# Sanity-check pre-state
[[ "$(read_active "$HOME_DIR/subos/default/.xlings.json" rm-fixture)" == "1.0.0" ]] \
  || fail "setup: default active should be 1.0.0"
[[ "$(read_active "$HOME_DIR/subos/tmp/.xlings.json" rm-fixture)" == "2.0.0" ]] \
  || fail "setup: tmp active should be 2.0.0"

# ── Action: remove in tmp
log "Action: 'xlings remove rm-fixture -y' in tmp"
RUN_IN tmp remove rm-fixture -y >/dev/null 2>&1 || fail "remove failed"

# ── Critical assertions: tmp must be clean, default untouched
TMP_ACTIVE="$(read_active "$HOME_DIR/subos/tmp/.xlings.json" rm-fixture)"
DEF_ACTIVE="$(read_active "$HOME_DIR/subos/default/.xlings.json" rm-fixture)"

[[ "$TMP_ACTIVE" == "<none>" ]] \
  || fail "REGRESSION: tmp.workspace[rm-fixture] should be empty, got '$TMP_ACTIVE' (likely leaked from default's installed[])"
log "  ✓ tmp workspace is clean (no leak from default)"

[[ "$DEF_ACTIVE" == "1.0.0" ]] \
  || fail "default.workspace[rm-fixture] should still be 1.0.0, got '$DEF_ACTIVE'"
log "  ✓ default workspace untouched"

[[ ! -e "$HOME_DIR/subos/tmp/bin/rm-fixture" ]] \
  || fail "REGRESSION: tmp/bin/rm-fixture shim still present after remove (subos installed[] is empty)"
log "  ✓ tmp shim removed (per-subos installed[] is empty)"

[[ -e "$HOME_DIR/subos/default/bin/rm-fixture" ]] \
  || fail "default/bin/rm-fixture shim was removed (default still has 1.0.0)"
log "  ✓ default shim preserved"

# Payload accounting: 2.0.0 should be physically gone, 1.0.0 retained
[[ ! -d "$HOME_DIR/data/xpkgs/xim-x-rm-fixture/2.0.0" ]] \
  || fail "rm-fixture@2.0.0 payload should be physically removed (no subos uses it)"
[[ -d "$HOME_DIR/data/xpkgs/xim-x-rm-fixture/1.0.0" ]] \
  || fail "rm-fixture@1.0.0 payload should be retained (default still uses it)"
log "  ✓ payload GC: 2.0.0 deleted, 1.0.0 preserved"

# Global versions DB: 2.0.0 dropped, 1.0.0 retained
DB_VERS="$(python3 -c "
import json
d = json.load(open('$HOME_DIR/.xlings.json'))
v = d.get('versions',{}).get('rm-fixture',{}).get('versions',{})
print(','.join(sorted(v.keys())))
")"
[[ "$DB_VERS" == "1.0.0" ]] \
  || fail "global versions DB should only have 1.0.0, got: '$DB_VERS'"
log "  ✓ global versions DB: only 1.0.0 remains"

# ── S2: removing a version ANOTHER subos still uses is a detach, and says so
#
# The case above has each subos on a different version, so the removal is a
# real removal. When two subos share the SAME version, `remove` deliberately
# keeps the registration and the payload -- deleting them would break the
# other subos. That is correct. Reporting it as "removed" was not: the user
# was told the package was gone while the version DB and data/xpkgs/ still
# held it in full, and the reinstall they tried next told them to uninstall
# first -- a loop with no exit (openxlings/xlings#443, #422).
#
# Falsifiable by construction: before the fix this printed the same
# "✓ rm-fixture@1.0.0 removed" as the real removal above, so the grep below
# is what fails on a regression.
log "S2: remove a version another subos still uses"
RUN_IN tmp install rm-fixture@1.0.0 -y >/dev/null 2>&1 \
  || fail "S2 setup: tmp install of the shared version failed"

S2_OUT="$(RUN_IN tmp remove rm-fixture -y 2>&1)" || fail "S2: remove failed"
# ftxui colours and pads; drop the escapes before matching.
S2_TEXT="$(printf '%s' "$S2_OUT" | sed 's/\x1b\[[0-9;]*m//g')"

grep -q "detached" <<<"$S2_TEXT" \
  || fail "S2: remove reported no detach; output was: $S2_TEXT"
log "  ✓ reported as a detach, not a removal"

grep -q "payload kept" <<<"$S2_TEXT" \
  || fail "S2: the retained payload was not mentioned; output was: $S2_TEXT"
log "  ✓ said the payload was kept"

# The state the message now matches. These held BEFORE the fix too -- they are
# what made the old "removed" wording false, so they belong in the assertion.
[[ -d "$HOME_DIR/data/xpkgs/xim-x-rm-fixture/1.0.0" ]] \
  || fail "S2: payload should be retained (default still uses 1.0.0)"
S2_DB="$(python3 -c "
import json
d = json.load(open('$HOME_DIR/.xlings.json'))
v = d.get('versions',{}).get('rm-fixture',{}).get('versions',{})
print(','.join(sorted(v.keys())))
")"
[[ "$S2_DB" == "1.0.0" ]] \
  || fail "S2: version DB should still carry 1.0.0, got: '$S2_DB'"
[[ "$(read_active "$HOME_DIR/subos/default/.xlings.json" rm-fixture)" == "1.0.0" ]] \
  || fail "S2: default should still be on 1.0.0"
log "  ✓ payload, version DB and the other subos are intact"

# And the current subos really was detached -- otherwise "detached" would be
# just as false as "removed" was.
[[ "$(read_active "$HOME_DIR/subos/tmp/.xlings.json" rm-fixture)" == "<none>" ]] \
  || fail "S2: tmp should have been detached from rm-fixture"
log "  ✓ tmp detached"

log "PASS: subos_install_remove_isolation"
