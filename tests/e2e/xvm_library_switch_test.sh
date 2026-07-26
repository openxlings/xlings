#!/usr/bin/env bash
# E2E test: libraries follow the release, and removal needs no teardown hook.
#
# Two things that were broken in different ways, exercised through the real
# CLI on a real package.
#
# 1. A library was a first-class entry everywhere except materialization on
#    switch. `plan_use_switch` decided library work by reading
#    `VData::libdir`, and nothing in the tree writes that field, so the
#    planner emitted nothing and `xlings use` was a silent no-op for
#    libraries. Install two versions of a package that ships a library and a
#    header, and the sysroot ends up with the library at whichever version
#    was installed last and the header at whichever was installed first --
#    compile against one, link against the other, and nothing says so.
#
# 2. Every recipe hand-writes an `uninstall()` that mirrors its `config()`
#    call for call. 0.4.70 made removal provider-scoped: with no `xvm.remove`
#    ops at all, the installer scans the database for entries whose
#    `bindingGroup.provider` matches and takes the whole release out. That
#    was read out of the code and never tested. This fixture's `uninstall()`
#    does nothing, so if the mechanism does not work the entries survive.
#
# Scenarios:
#   1. install v1        → library and header both materialize at v1
#   2. install v2        → v1 stays active; the sysroot must not move
#   3. use v2            → library AND header both move to v2
#   4. use v1            → both move back
#   5. remove (no teardown hook) → every entry of the release deregisters

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/xvm_library_switch"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/l/libfixture.lua"

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

# One program, one library, one header -- the shape of openssl, which is the
# package the real failure was found on. The library keeps the same soname
# across versions, so switching replaces it rather than creating a second
# one; that is the case that matters, because it is the one where a stale
# link is invisible.
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "libfixture",
    description = "Local fixture for tests/e2e/xvm_library_switch_test.sh",
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

local SONAME = "libfixture.so.1"

function install()
    local dir = pkginfo.install_dir()
    local version = pkginfo.version()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    os.mkdir(path.join(dir, "lib"))
    os.mkdir(path.join(dir, "include"))
    -- Every artifact is stamped with its version so the sysroot can be
    -- checked for which release is actually materialized.
    io.writefile(path.join(dir, "bin", "fixture-tool"),
                 "#!/bin/sh\necho fixture-tool " .. version .. "\n")
    io.writefile(path.join(dir, "lib", SONAME),
                 "SOVERSION " .. version .. "\n")
    io.writefile(path.join(dir, "include", "fixture.h"),
                 "#define FIXTURE_VERSION \"" .. version .. "\"\n")
    return true
end

function config()
    local dir = pkginfo.install_dir()
    local binding = "libfixture@" .. pkginfo.version()

    -- Root of the release. A library-only package would register nothing
    -- else here; this one also has a program.
    xvm.add("libfixture")
    xvm.add("fixture-tool", {
        bindir = path.join(dir, "bin"),
        binding = binding,
    })
    -- The library, exactly as a real recipe registers one.
    xvm.add(SONAME, {
        type = "lib",
        bindir = path.join(dir, "lib"),
        filename = SONAME,
        alias = SONAME,
        binding = binding,
    })
    -- Headers through the op xlings owns, so `use` can move them.
    table.insert(_XVM_OPS, {
        op = "headers",
        includedir = path.join(dir, "include"),
    })
    return true
end

-- Deliberately empty. Removal is provider-scoped since 0.4.70: the installer
-- finds every entry belonging to this provider and takes the release out.
-- If that does not work, scenario 5 fails.
function uninstall()
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

SYSROOT_LIB="$HOME_DIR/subos/default/lib/libfixture.so.1"
SYSROOT_HDR="$HOME_DIR/subos/default/usr/include/fixture.h"

lib_version() {
  [[ -e "$SYSROOT_LIB" ]] || { echo ""; return; }
  sed -n 's/^SOVERSION \(.*\)$/\1/p' "$SYSROOT_LIB"
}

header_version() {
  [[ -e "$SYSROOT_HDR" ]] || { echo ""; return; }
  sed -n 's/.*FIXTURE_VERSION "\([^"]*\)".*/\1/p' "$SYSROOT_HDR"
}

active_version() {
  python3 - "$HOME_DIR/subos/default/.xlings.json" "$1" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as fh:
        data = json.load(fh)
except (OSError, ValueError):
    print(""); sys.exit(0)
entry = (data.get("workspace", {}) or {}).get(sys.argv[2]) or {}
print(entry.get("active", "") if isinstance(entry, dict) else str(entry))
PY
}

expect_sysroot_at() {
  local want="$1" ctx="$2" got_lib got_hdr
  got_lib="$(lib_version)"
  got_hdr="$(header_version)"
  [[ "$got_lib" == "$want" ]] \
    || fail "$ctx: sysroot library is '$got_lib', expected '$want' — the library did not follow the release"
  [[ "$got_hdr" == "$want" ]] \
    || fail "$ctx: sysroot header is '$got_hdr', expected '$want'"
  # The pair is the whole point: a library at one version beside a header at
  # another is the state that compiles and then fails at run time.
  [[ "$got_lib" == "$got_hdr" ]] \
    || fail "$ctx: library '$got_lib' and header '$got_hdr' are from different releases"
}

# ── Scenario 1 ────────────────────────────────────────────────────
log "S1: install 1.0.0 → library and header materialize at 1.0.0"
RUN install libfixture@1.0.0 -y > "$RUNTIME_DIR/install1.log" 2>&1 \
  || { cat "$RUNTIME_DIR/install1.log"; fail "install 1.0.0 failed"; }
expect_sysroot_at "1.0.0" "S1"

# ── Scenario 2 ────────────────────────────────────────────────────
log "S2: install 2.0.0 → 1.0.0 stays active, sysroot must not move"
RUN install libfixture@2.0.0 -y >/dev/null 2>&1 || fail "install 2.0.0 failed"
[[ "$(active_version fixture-tool)" == "1.0.0" ]] \
  || fail "S2: installing a second version stole the active pointer"
expect_sysroot_at "1.0.0" "S2"

# ── Scenario 3 ────────────────────────────────────────────────────
log "S3: use 2.0.0 → library AND header both move"
RUN use fixture-tool 2.0.0 >/dev/null 2>&1 || fail "S3: use failed"
expect_sysroot_at "2.0.0" "S3"

# ── Scenario 4 ────────────────────────────────────────────────────
log "S4: use 1.0.0 → both move back"
RUN use fixture-tool 1.0.0 >/dev/null 2>&1 || fail "S4: use failed"
expect_sysroot_at "1.0.0" "S4"

# Entering from the library resolves the same release as entering from the
# program.
log "S4b: entering from the library switches the whole release"
RUN use libfixture.so.1 2.0.0 >/dev/null 2>&1 || fail "S4b: use failed"
expect_sysroot_at "2.0.0" "S4b"
[[ "$(active_version fixture-tool)" == "2.0.0" ]] \
  || fail "S4b: entering from the library left the program behind"

# ── Scenario 5 ────────────────────────────────────────────────────
log "S5: remove with an empty uninstall() → the whole release deregisters"
RUN remove libfixture@2.0.0 -y > "$RUNTIME_DIR/remove.log" 2>&1 \
  || { cat "$RUNTIME_DIR/remove.log"; fail "S5: remove failed"; }

still_registered() {
  python3 - "$HOME_DIR/.xlings.json" "$1" "$2" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as fh:
        data = json.load(fh)
except (OSError, ValueError):
    print("no"); sys.exit(0)
versions = (data.get("versions", {}) or {}).get(sys.argv[2], {}) or {}
print("yes" if sys.argv[3] in (versions.get("versions", {}) or {}) else "no")
PY
}

for target in fixture-tool libfixture.so.1 libfixture; do
  [[ "$(still_registered "$HOME_DIR/.xlings.json" "$target" "2.0.0")" == "no" ]] \
    || fail "S5: '$target@2.0.0' survived removal — provider-scoped teardown did not cover it"
done
log "S5: every entry of the release was deregistered without a teardown hook"

# The surviving release must come back as a whole, never mixed.
LIB_AFTER="$(lib_version)"
HDR_AFTER="$(header_version)"
if [[ -n "$LIB_AFTER" || -n "$HDR_AFTER" ]]; then
  [[ "$LIB_AFTER" == "$HDR_AFTER" ]] \
    || fail "S5: fell back to a split release — library='$LIB_AFTER' header='$HDR_AFTER'"
  log "S5: fell back to $LIB_AFTER as a whole"
else
  log "S5: whole release deactivated (acceptable — never mixed)"
fi

log "PASS: xvm library switch + provider-scoped removal"
