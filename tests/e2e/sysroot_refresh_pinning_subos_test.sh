#!/usr/bin/env bash
# E2E test: a registration rewrite refreshes the sysroot of every subos that
# pins the rewritten version, not only the subos the command ran in (#586).
#
# The installer's effect-placement loop only ever wrote into the CURRENT
# subos. A registration rewrite (the recipe's install()/config() hooks
# re-run for a target@version that was already registered, and write
# DIFFERENT source paths for it) changed what a symlink in the sysroot
# should point at. Only the subos the command ran in got the update; every
# other subos pinning that exact version kept a symlink pointing at
# whatever the OLD registration named, and once that location is gone
# (this fixture's install() wipes its own install dir before writing the
# new layout -- a real reinstall-from-a-cleared-payload does the same),
# the link is left dangling. Measured on a home in long use:
# freetype's links were valid in 4 subos and dangling in 3.
#
# The fixture makes the rewrite deterministic: install() picks one of two
# sub-layouts ("gen1"/"gen2") based on a marker file, and always wipes the
# WHOLE install dir first -- so the layout current before a registration
# rewrite is physically gone afterward, not merely unreferenced.
#
# Getting a SECOND subos to genuinely pin the version (not just install it
# independently) needs a workaround: once a payload is already installed
# anywhere, `xlings install` of the same target@version takes a shortcut
# straight to activation (`xvm cmd_use`) instead of re-running the recipe --
# and that activation refuses because the target was never in THIS subos's
# own installed[] set, so plain sequential `install` calls in two subos
# never both end up pinning the payload through the CLI alone. This is a
# separate, pre-existing gap (not #586), so the test works around it by
# seeding `other`'s installed[] directly, the same way other e2e fixtures
# seed state a code path won't produce for them, then calling `xlings use`
# to materialize+activate for real. See task-7-report.md for the write-up.
#
# For the same reason, plain `remove` + `install` (the shape #586 was
# literally filed against) does not by itself retrigger the recipe once
# another subos genuinely pins the version: `remove` correctly detaches
# only the current subos and keeps the shared payload ("payload kept --
# still used by other"), and the payload still being on disk means the
# next `install` takes that same broken shortcut instead of re-running
# config(). The test clears the shared payload directory after `remove` to
# force the reinstall through -- a stand-in for the payload having gone
# missing or been GC'd, which is a realistic way a real registration
# rewrite gets triggered.
#
# Scenarios:
#   1. install in `default`; bootstrap `other` to genuinely pin the same
#      version too; `bystander` gets nothing. Both installed subos read
#      back gen1.
#   2. flip the marker; in `default` only: `remove` (detach-only, since
#      `other` still pins it) + clear the shared payload + `install` again
#      -> a registration rewrite to gen2.
#   3. `other`'s sysroot must have followed the rewrite to gen2, with no
#      dangling symlink left behind.
#   4. `bystander`, which never had the package, must still have none --
#      the refresh must not push it into a subos that never asked for it.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/sysroot_refresh_pinning_subos"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/l/libfixture.lua"
MARKER_FILE="$RUNTIME_DIR/relocate-marker"
PAYLOAD_DIR="$HOME_DIR/data/xpkgs/xim-x-libfixture/1.0.0"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

# Every command names the subos it runs in, same convention as
# self_doctor_multi_subos_test.sh -- this test is entirely about which
# subos a thing lands in.
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
mkdir -p "$(dirname "$FIXTURE_PKG")"

# A program + a library + a header, one version. install() always wipes its
# own install dir, then re-lays it out under "gen1" or "gen2" depending on
# whether $MARKER_FILE exists -- both the payload's physical location and
# the DB's registered path move together, at the SAME package version.
# `libfixture` itself carries the bindir (not a separate bindir-less "root"
# name): a bindir-less root fails `xvm cmd_use`'s activation silently,
# which is the shortcut S1 uses to seed `other` below.
cat > "$FIXTURE_PKG" <<LUA
package = {
    spec = "1",
    name = "libfixture",
    description = "Local fixture for tests/e2e/sysroot_refresh_pinning_subos_test.sh",
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

local SONAME = "libfixture.so.1"

local function gen()
    return os.isfile("$MARKER_FILE") and "gen2" or "gen1"
end

function install()
    local dir = pkginfo.install_dir()
    local version = pkginfo.version()
    local g = gen()
    -- Wipes whatever layout a PREVIOUS install left behind, gen1 included --
    -- a real registration rewrite leaves the old location gone, not merely
    -- unreferenced.
    os.tryrm(dir)
    os.mkdir(path.join(dir, g, "bin"))
    os.mkdir(path.join(dir, g, "lib"))
    os.mkdir(path.join(dir, g, "include"))
    io.writefile(path.join(dir, g, "bin", "libfixture"),
                 "#!/bin/sh\necho libfixture " .. version .. " " .. g .. "\n")
    io.writefile(path.join(dir, g, "lib", SONAME),
                 "GEN " .. g .. "\n")
    io.writefile(path.join(dir, g, "include", "fixture.h"),
                 "#define FIXTURE_GEN \"" .. g .. "\"\n")
    return true
end

function config()
    local dir = pkginfo.install_dir()
    local g = gen()
    local binding = "libfixture@" .. pkginfo.version()

    -- The top-level target: real materialization (a bindir), no binding
    -- field of its own -- a node cannot bind to itself.
    xvm.add("libfixture", {
        bindir = path.join(dir, g, "bin"),
    })
    xvm.add(SONAME, {
        type = "lib",
        bindir = path.join(dir, g, "lib"),
        filename = SONAME,
        alias = SONAME,
        binding = binding,
    })
    table.insert(_XVM_OPS, {
        op = "headers",
        includedir = path.join(dir, g, "include"),
    })
    return true
end

-- Deliberately empty, same convention as xvm_library_switch_test.sh: the
-- installer's provider-scoped teardown does the deregistration.
function uninstall()
    return true
end
LUA

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
RUN_IN default self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
RUN_IN default subos new other >/dev/null 2>&1 || fail "subos new other failed"
RUN_IN default subos new bystander >/dev/null 2>&1 || fail "subos new bystander failed"

LIB_DEFAULT="$HOME_DIR/subos/default/lib/libfixture.so.1"
LIB_OTHER="$HOME_DIR/subos/other/lib/libfixture.so.1"
LIB_BYSTANDER_DIR="$HOME_DIR/subos/bystander/lib"
HDR_OTHER="$HOME_DIR/subos/other/usr/include/fixture.h"
OTHER_WS="$HOME_DIR/subos/other/.xlings.json"

lib_gen() {
  [[ -e "$1" ]] || { echo ""; return; }
  sed -n 's/^GEN \(.*\)$/\1/p' "$1"
}

# ── Scenario 1: default installs for real; other is made to genuinely pin
#    the same version too (see the file header for why this needs a
#    workaround instead of a second plain `install`); bystander gets nothing.
log "S1: install libfixture@1.0.0 in default (gen1)"
RUN_IN default install libfixture@1.0.0 -u -y \
  > "$RUNTIME_DIR/install-default-1.log" 2>&1 \
  || { cat "$RUNTIME_DIR/install-default-1.log"; fail "S1: install in default failed"; }

log "S1: seed other's installed[] and materialize with 'xlings use' (genuine pin, gen1)"
python3 - "$OTHER_WS" <<'PY'
import json, sys
p = sys.argv[1]
with open(p) as fh:
    data = json.load(fh)
ws = data.setdefault("workspace", {})
ws["libfixture"] = {"installed": ["1.0.0"]}
ws["libfixture.so.1"] = {"installed": ["1.0.0"]}
with open(p, "w") as fh:
    json.dump(data, fh, indent=2)
PY
RUN_IN other use libfixture 1.0.0 \
  > "$RUNTIME_DIR/use-other-1.log" 2>&1 \
  || { cat "$RUNTIME_DIR/use-other-1.log"; fail "S1: use in other failed"; }

[[ "$(lib_gen "$LIB_DEFAULT")" == "gen1" ]] \
  || fail "S1: default's sysroot library is not gen1"
[[ "$(lib_gen "$LIB_OTHER")" == "gen1" ]] \
  || fail "S1: other's sysroot library is not gen1"
grep -q '"libfixture"' "$OTHER_WS" \
  || fail "S1: other does not genuinely pin libfixture -- setup did not take"
[[ ! -e "$LIB_BYSTANDER_DIR/libfixture.so.1" ]] \
  || fail "S1: bystander must not have libfixture's library before ever installing it"

# ── Scenario 2: a registration rewrite in `default` only ─────────────
log "S2: flip the marker; remove (detach-only, other still pins it) + clear payload + reinstall"
touch "$MARKER_FILE"
RUN_IN default remove libfixture -y \
  > "$RUNTIME_DIR/remove-default.log" 2>&1 \
  || { cat "$RUNTIME_DIR/remove-default.log"; fail "S2: remove in default failed"; }
grep -qi 'still used by other' "$RUNTIME_DIR/remove-default.log" \
  || fail "S2: remove did not detach-only (other's pin was not recognized) -- got:\n$(cat "$RUNTIME_DIR/remove-default.log")"
# The shared payload is untouched by a detach-only remove; clear it so the
# next install cannot take the "already installed" shortcut and must
# re-run the recipe -- see the file header.
rm -rf "$PAYLOAD_DIR"
RUN_IN default install libfixture@1.0.0 -u -y \
  > "$RUNTIME_DIR/install-default-2.log" 2>&1 \
  || { cat "$RUNTIME_DIR/install-default-2.log"; fail "S2: reinstall in default failed"; }

[[ "$(lib_gen "$LIB_DEFAULT")" == "gen2" ]] \
  || fail "S2: default's own sysroot did not follow the registration rewrite to gen2"

# ── Scenario 3: `other` must have been refreshed too, no dangling link ──
log "S3: other's sysroot must follow the rewrite -- no dangling symlink"
DANGLING="$(find "$HOME_DIR/subos/other/lib" -xtype l 2>/dev/null || true)"
[[ -z "$DANGLING" ]] \
  || fail "S3: other/lib has dangling symlink(s): $DANGLING"
[[ -e "$LIB_OTHER" ]] \
  || fail "S3: other's libfixture.so.1 link does not resolve"
[[ "$(lib_gen "$LIB_OTHER")" == "gen2" ]] \
  || fail "S3: other's sysroot library is '$(lib_gen "$LIB_OTHER")', expected 'gen2' -- the registration rewrite did not reach it"

# The header must have moved the same way -- same gate, same lambda.
[[ -e "$HDR_OTHER" ]] && grep -q 'FIXTURE_GEN "gen2"' "$HDR_OTHER" \
  || fail "S3: other's header did not follow the registration rewrite to gen2"

# ── Scenario 4: bystander gets nothing pushed to it ───────────────────
log "S4: bystander (never installed) must still have no libfixture link"
[[ ! -e "$LIB_BYSTANDER_DIR/libfixture.so.1" ]] \
  || fail "S4: the cross-subos refresh pushed libfixture into bystander, which never had it"

# ── Scenario 5: a corrupted pinning subos must not block the others,
#    must not crash the command that is refreshing them, and must now be
#    named in a warn instead of silently disappearing from the scan.
#
# Controller ruling (Item A, 2026.9.12): `load_subos_snapshots` still SKIPS
# a subos whose `.xlings.json` fails to parse from the returned snapshot
# list -- a hand-corrupted config must not take down an unrelated `remove`
# -- but it now also reports that name through its `unreadable` out-param,
# and `find_subos_pinning_version` forwards it. Both consumers of that
# predicate were widened to treat "could not tell" as "assume it still
# uses this": `is_version_referenced_anywhere_` (installer.cpp) now makes
# `remove` detach-only instead of a full removal whenever ANY subos it
# scans is unreadable, and this cross-subos sysroot-refresh loop now warns
# by name about every subos the scan could not even open -- reachable here
# for exactly the pre-corrupted-file case this comment used to say could
# not reach it.
log "S5: a corrupted pinning subos's file must not block or crash the others' refresh"
printf '{garbage' > "$OTHER_WS"
rm -f "$MARKER_FILE"
rm -rf "$PAYLOAD_DIR"
RUN_IN default remove libfixture -y \
  > "$RUNTIME_DIR/remove-default-2.log" 2>&1 \
  || { cat "$RUNTIME_DIR/remove-default-2.log"; fail "S5: remove in default failed"; }
grep -q 'doctor --subos other' "$RUNTIME_DIR/remove-default-2.log" \
  || fail "S5: remove did not name 'other' as an unreadable/possibly-pinning subos -- got:\n$(cat "$RUNTIME_DIR/remove-default-2.log")"
RUN_IN default install libfixture@1.0.0 -u -y \
  > "$RUNTIME_DIR/install-default-3.log" 2>&1 \
  || { cat "$RUNTIME_DIR/install-default-3.log"; fail "S5: reinstall in default failed"; }

grep -q 'doctor --subos other' "$RUNTIME_DIR/install-default-3.log" \
  || fail "S5: the sysroot-refresh warn did not name 'other' as unreadable -- got:\n$(cat "$RUNTIME_DIR/install-default-3.log")"
[[ "$(lib_gen "$LIB_DEFAULT")" == "gen1" ]] \
  || fail "S5: default's own sysroot did not follow this second registration rewrite -- a corrupted OTHER subos must not block it"
[[ ! -e "$LIB_BYSTANDER_DIR/libfixture.so.1" ]] \
  || fail "S5: bystander must still have nothing after a rewrite that ran alongside a corrupted subos"

log "PASS: a registration rewrite refreshes every subos pinning the version, not the ones that never asked, and a broken subos cannot block or widen that"
