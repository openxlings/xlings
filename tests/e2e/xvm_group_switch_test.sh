#!/usr/bin/env bash
# E2E test: `xlings use` moves a whole toolchain release, or none of it.
#
# The bug this pins down: switching one member of a bound release used to
# leave the others behind. `gcc --version` reported the version the user
# asked for while `g++` stayed on the previous release, and the sysroot
# headers stayed on the previous release too -- so the failure surfaced much
# later, as an ABI or header mismatch during a build, with nothing to connect
# it back to the `use` that caused it.
#
# The fixture mirrors the real shape without needing a real toolchain: two
# programs bound into one release, plus headers, at two versions.
#
# Scenarios:
#   1. use <root> v1              → every member and the headers move to v1
#   2. use <member> v2            → entering from a bound member moves the
#                                   whole release, identically
#   3. remove v2                  → the surviving release comes back as a
#                                   whole, never mixed
#   4. use with a missing member  → refused, and nothing has changed

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/xvm_group_switch"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/t/toolchain-fixture.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR"

cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

# Two programs bound into one release, plus a version-stamped header. `tcxx`
# binds to `tcc`, so they form one group per version -- the same shape as
# gcc/g++ and their headers.
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "toolchain-fixture",
    description = "Local fixture for tests/e2e/xvm_group_switch_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local dir = pkginfo.install_dir()
    local version = pkginfo.version()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    os.mkdir(path.join(dir, "include"))
    -- Each version ships a header stamped with its own version, so the
    -- sysroot can be checked for which release is actually materialized.
    io.writefile(path.join(dir, "include", "tcver.h"),
                 "#define TC_VERSION \"" .. version .. "\"\n")
    -- Plain files: this test checks which release the workspace and the
    -- sysroot point at, not that the programs run. (os.runv is not in the
    -- libxpkg Lua sandbox, so there is no chmod here anyway.)
    for _, name in ipairs({"tcc", "tcxx"}) do
        io.writefile(path.join(dir, "bin", name),
                     "#!/bin/sh\necho " .. name .. " " .. version .. "\n")
    end
    return true
end

function config()
    -- xvm.setup is the shape real toolchain recipes use: a root node, the
    -- programs bound to it, and a header directory, all one release.
    xvm.setup("toolchain-fixture", {
        bindir     = "bin",
        includedir = "include",
        programs   = {"tcc", "tcxx"},
    })
    return true
end

function uninstall()
    xvm.teardown("toolchain-fixture", {
        includedir = "include",
        programs   = {"tcc", "tcxx"},
    })
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

SYSROOT_HEADER="$HOME_DIR/subos/default/usr/include/tcver.h"

active_version() {
  # Read the active version of $1 straight out of the subos workspace.
  python3 - "$HOME_DIR/subos/default/.xlings.json" "$1" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as fh:
        data = json.load(fh)
except (OSError, ValueError):
    print("")
    sys.exit(0)
entry = (data.get("workspace", {}) or {}).get(sys.argv[2]) or {}
print(entry.get("active", "") if isinstance(entry, dict) else str(entry))
PY
}

header_version() {
  [[ -f "$SYSROOT_HEADER" ]] || { echo ""; return; }
  sed -n 's/.*TC_VERSION "\([^"]*\)".*/\1/p' "$SYSROOT_HEADER"
}

expect_group_at() {
  local want="$1" ctx="$2" got_tcc got_tcxx got_hdr
  got_tcc="$(active_version tcc)"
  got_tcxx="$(active_version tcxx)"
  got_hdr="$(header_version)"
  [[ "$got_tcc" == "$want" ]] \
    || fail "$ctx: tcc is '$got_tcc', expected '$want'"
  # The whole point: a member left behind is the bug.
  [[ "$got_tcxx" == "$want" ]] \
    || fail "$ctx: tcxx is '$got_tcxx' while tcc is '$want' — release split"
  [[ "$got_hdr" == "$want" ]] \
    || fail "$ctx: sysroot header is '$got_hdr', expected '$want' — headers did not follow"
}

log "Installing both releases"
RUN install toolchain-fixture@1.0.0 -y > "$RUNTIME_DIR/install1.log" 2>&1 \
  || { cat "$RUNTIME_DIR/install1.log"; fail "install 1.0.0 failed"; }
RUN install toolchain-fixture@2.0.0 -y >/dev/null 2>&1 \
  || fail "install 2.0.0 failed"

# ── Scenario 1: switch from the root ──────────────────────────────
log "S1: use tcc 1.0.0 → whole release on 1.0.0"
RUN use tcc 1.0.0 >/dev/null 2>&1 || fail "S1: use failed"
expect_group_at "1.0.0" "S1"

# ── Scenario 2: switch by entering from a bound member ────────────
log "S2: use tcxx 2.0.0 → whole release on 2.0.0"
RUN use tcxx 2.0.0 >/dev/null 2>&1 || fail "S2: use failed"
expect_group_at "2.0.0" "S2"

# Entering from either member must land in the same place.
log "S3: use tcc 1.0.0 again → identical to entering from tcxx"
RUN use tcc 1.0.0 >/dev/null 2>&1 || fail "S3: use failed"
expect_group_at "1.0.0" "S3"

# ── Scenario 4: removal falls back as a whole ─────────────────────
log "S4: switch to 2.0.0, remove it → fall back to 1.0.0 coherently"
RUN use tcc 2.0.0 >/dev/null 2>&1 || fail "S4: use failed"
expect_group_at "2.0.0" "S4 (before remove)"
RUN remove toolchain-fixture@2.0.0 -y >/dev/null 2>&1 \
  || fail "S4: remove failed"

TCC_AFTER="$(active_version tcc)"
TCXX_AFTER="$(active_version tcxx)"
# Either the whole surviving release came back, or nothing did. A mix is the
# failure this release exists to prevent.
if [[ -n "$TCC_AFTER" || -n "$TCXX_AFTER" ]]; then
  [[ "$TCC_AFTER" == "$TCXX_AFTER" ]] \
    || fail "S4: fell back to a split release — tcc='$TCC_AFTER' tcxx='$TCXX_AFTER'"
  [[ "$TCC_AFTER" == "1.0.0" ]] \
    || fail "S4: fell back to '$TCC_AFTER', expected the surviving 1.0.0"
  log "S4: fell back to 1.0.0 as a whole"
else
  log "S4: whole release deactivated (acceptable — never mixed)"
fi

# ── Scenario 5: a broken release is refused, changing nothing ─────
log "S5: break a member's payload record → use is refused, nothing changes"
RUN use tcc 1.0.0 >/dev/null 2>&1 || fail "S5: setup use failed"
expect_group_at "1.0.0" "S5 (before)"

WS_BEFORE="$(cat "$HOME_DIR/subos/default/.xlings.json")"
HDR_BEFORE="$(header_version)"

# Drop tcxx@1.0.0 from the version database, leaving the release's manifest
# still naming it. This is the dangling state a stale edge used to produce.
python3 - "$HOME_DIR/.xlings.json" <<'PY'
import json, sys
path = sys.argv[1]
with open(path) as fh:
    data = json.load(fh)
versions = data.get("versions", {})
if "tcxx" in versions:
    versions["tcxx"].get("versions", {}).pop("1.0.0", None)
with open(path, "w") as fh:
    json.dump(data, fh, indent=2)
PY

if RUN use tcc 1.0.0 >/dev/null 2>&1; then
  fail "S5: use succeeded despite a missing member"
fi
[[ "$(cat "$HOME_DIR/subos/default/.xlings.json")" == "$WS_BEFORE" ]] \
  || fail "S5: refused but the workspace changed anyway"
[[ "$(header_version)" == "$HDR_BEFORE" ]] \
  || fail "S5: refused but the sysroot headers changed anyway"
log "S5: refused with the workspace and sysroot untouched"

log "PASS: xvm group switch"
