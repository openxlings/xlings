#!/usr/bin/env bash
# E2E-106: one broken package (or subos) must not change what other
# commands report for the rest of the home.
#
# The requirement, made concrete: a home can accumulate several
# independent kinds of damage --
#
#   1. a subos directory whose .xlings.json is not valid JSON at all
#      (load_subos_snapshots() used to `catch (...) {}` this and say
#      nothing -- see FindingKind::SubosUnreadable)
#   2. a versions-DB entry for a package that was never really installed,
#      pointing at a payload directory that does not exist
#   3. a local overlay recipe added via `config --add-xpkg` whose file was
#      then deleted out from under the overlay's own bookkeeping
#   4. a REAL installed package's own xvm record edited to point at a
#      bin directory that does not exist -- breaking that one package
#
# None of this is fiction: it is what a hand-edited, multi-year home looks
# like, one incident at a time. The blast radius of every one of these must
# be its own entry, its own package, its own subos -- never the rest of the
# home. This fixture builds all four AT ONCE and proves it by comparing a
# snapshot of an untouched package's payload + shim table taken before the
# damage against one taken after every command below has run against the
# wounded home.
#
# NOTE ON FIXTURE NAMES: a parallel task in this round (Task 5) is expected
# to add `pkgs/p/plain.lua` and `pkgs/h/hookfail.lua` to the shared
# tests/fixtures/xim-pkgindex checkout. It was not present in this worktree
# at the time this test was written, so this test defines its own
# equivalents -- `isoplain` and `isohookfail` -- directly in its PRIVATE copy
# of the fixture index (the same pattern doctor_fix_convergence_test.sh and
# self_doctor_test.sh already use for their own fixtures), under different
# names so a later merge of Task 5's recipes cannot collide with this file.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/broken_home_isolation"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

# ABSOLUTE -- commands below run from /tmp, where a relative binary path
# dies with `env: No such file or directory`.
XLINGS_BIN="$(cd "$(dirname "$(find_xlings_bin)")" && pwd)/$(basename "$(find_xlings_bin)")"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
      "$XLINGS_BIN" "$@" </dev/null )
}

RUN_SHIM() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
      "$HOME_DIR/subos/default/bin/$1" "${@:2}" )
}

# No `terminate called`, no `Segmentation`, no `Aborted` -- and an exit code
# a wrapping script can act on (0 clean, 1 issues found, 2 a refusal) rather
# than a raw signal (134 SIGABRT, 139 SIGSEGV).
assert_survives() {
  local label="$1" rc="$2" out="$3"
  case "$rc" in
    0|1|2) ;;
    *) fail "$label: exit code $rc is not in {0,1,2} -- looks like a crash:
$out" ;;
  esac
  echo "$out" | grep -qE "terminate called|Segmentation|Aborted" \
    && fail "$label: crash signature in output:
$out"
  return 0
}

mkdir -p "$HOME_DIR"

# Private, mutable copy of the shared fixture index.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/i"

emit_good_pkg() {
  local name="$1"
  cat > "$LOCAL_INDEX_DIR/pkgs/i/$name.lua" <<LUA
package = {
    spec = "1",
    name = "$name",
    description = "Local fixture for tests/e2e/broken_home_isolation_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    programs = {"$name"},
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
    io.writefile(path.join(bindir, "$name"),
                 "#!/bin/sh\necho $name@" .. pkginfo.version() .. "\n")
    if os.host() ~= "windows" then
        os.exec("chmod +x " .. path.join(bindir, "$name"))
    end
    return true
end

function config()
    xvm.add("$name", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("$name")
    return true
end
LUA
}

emit_good_pkg isoplain
emit_good_pkg isohookfail
# Mirror the real hookfail fixture's defining trait (uninstall() fails) even
# though this test never uninstalls it -- if it ever does, the failure is
# the point, not a surprise.
python3 - "$LOCAL_INDEX_DIR/pkgs/i/isohookfail.lua" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
text = p.read_text()
text = text.replace(
    'function uninstall()\n    xvm.remove("isohookfail")\n    return true\nend',
    'function uninstall()\n    xvm.remove("isohookfail")\n    error("boom")\n    return true\nend',
)
p.write_text(text)
PY

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

log "Installing the two good packages"
RUN install isoplain@1.0.0 -y >/dev/null 2>&1 || fail "setup: install isoplain failed"
RUN install isohookfail@1.0.0 -y >/dev/null 2>&1 || fail "setup: install isohookfail failed"
RUN use isoplain 1.0.0 >/dev/null 2>&1 || fail "setup: use isoplain failed"
RUN use isohookfail 1.0.0 >/dev/null 2>&1 || fail "setup: use isohookfail failed"

PLAIN_SHIM="$HOME_DIR/subos/default/bin/isoplain"
PLAIN_PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-isoplain/1.0.0"
[[ -e "$PLAIN_SHIM" ]] || fail "setup: isoplain shim should exist"
[[ -d "$PLAIN_PAYLOAD" ]] || fail "setup: isoplain payload dir should exist"

snapshot() {
  local tree_hash bin_listing
  tree_hash="$(find "$PLAIN_PAYLOAD" -type f -exec sha256sum {} + 2>/dev/null \
                 | sort | sha256sum | awk '{print $1}')"
  bin_listing="$(ls "$HOME_DIR/subos/default/bin" 2>/dev/null | sort | sha256sum | awk '{print $1}')"
  printf '%s:%s\n' "$tree_hash" "$bin_listing"
}

snap_before="$(snapshot)"
[[ -n "$snap_before" ]] || fail "setup: snapshot should not be empty"

# ── Inject the four kinds of damage ─────────────────────────────────

log "Injection 1: a subos whose .xlings.json is not valid JSON"
mkdir -p "$HOME_DIR/subos/broken"
printf '{garbage' > "$HOME_DIR/subos/broken/.xlings.json"

log "Injection 2: a DB entry for a package that was never installed"
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
state = home / ".xlings.json"
d = json.loads(state.read_text())
d.setdefault("versions", {})["ghost"] = {
    "filename": "ghost",
    "type": "program",
    "versions": {
        "1.0": {
            "kind": "program",
            "path": str(home / "data" / "xpkgs" / "xim-x-ghost" / "1.0" / "bin"),
        }
    },
}
state.write_text(json.dumps(d, indent=2))
PY

log "Injection 3: a local overlay recipe whose file is deleted after add-xpkg"
mkdir -p "$RUNTIME_DIR/local-src"
emit_overlay_src() {
  local name="$1"
  cat > "$RUNTIME_DIR/local-src/$name.lua" <<LUA
package = {
    spec = "1",
    name = "$name",
    description = "Local-only overlay fixture, never installed, for tests/e2e/broken_home_isolation_test.sh",
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
LUA
}
emit_overlay_src isolocal
# A SECOND, untouched overlay entry -- proves the blast radius of the
# deleted-file entry below is itself just one entry, not the whole overlay
# listing.
emit_overlay_src isolocalhealthy

addout="$(RUN config --add-xpkg "$RUNTIME_DIR/local-src/isolocal.lua" 2>&1)" \
  || fail "setup: config --add-xpkg isolocal failed:
$addout"
OVERLAY_FILE="$HOME_DIR/data/xim-pkgindex-local/pkgs/i/isolocal.lua"
[[ -f "$OVERLAY_FILE" ]] || fail "setup: overlay file should exist at $OVERLAY_FILE:
$addout"

addout2="$(RUN config --add-xpkg "$RUNTIME_DIR/local-src/isolocalhealthy.lua" 2>&1)" \
  || fail "setup: config --add-xpkg isolocalhealthy failed:
$addout2"

rm -f "$OVERLAY_FILE"
[[ ! -e "$OVERLAY_FILE" ]] || fail "setup: overlay file should be gone"

log "Injection 4: isohookfail's own xvm record points at a bin dir that does not exist"
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
state = home / ".xlings.json"
d = json.loads(state.read_text())
entry = d["versions"]["isohookfail"]["versions"]["1.0.0"]
entry["path"] = str(home / "data" / "xpkgs" / "xim-x-isohookfail" / "1.0.0" / "does-not-exist")
state.write_text(json.dumps(d, indent=2))
PY

# ── The rest of the home must not notice ────────────────────────────

log "list"
rc=0; out="$(RUN list 2>&1)" || rc=$?
assert_survives "list" "$rc" "$out"

log "install isoplain -y (again, on the wounded home)"
rc=0; out="$(RUN install isoplain@1.0.0 -y 2>&1)" || rc=$?
assert_survives "install isoplain" "$rc" "$out"

log "use isoplain 1.0.0 (again)"
rc=0; out="$(RUN use isoplain 1.0.0 2>&1)" || rc=$?
assert_survives "use isoplain" "$rc" "$out"

log "run isoplain's own shim"
rc=0; out="$(RUN_SHIM isoplain --version 2>&1)" || rc=$?
assert_survives "isoplain shim" "$rc" "$out"
echo "$out" | grep -q "isoplain@1.0.0" \
  || fail "isoplain shim: expected it to still run its program; got:
$out"

log "self doctor"
rc=0; doctor_out="$(RUN self doctor 2>&1)" || rc=$?
assert_survives "self doctor" "$rc" "$doctor_out"
echo "$doctor_out" | strip_ansi | grep -q "broken" \
  || fail "self doctor: expected the unreadable subos name ('broken') in the output; got:
$doctor_out"

log "self doctor --deep"
rc=0; deep_out="$(RUN self doctor --deep 2>&1)" || rc=$?
assert_survives "self doctor --deep" "$rc" "$deep_out"

# Bonus, directly exercising cmd_list --all's new note (same injected
# damage): a subos that could not be read must be SAID, not just omitted.
log "list --all (the note for the unreadable subos)"
rc=0; list_all_out="$(RUN list --all 2>&1)" || rc=$?
assert_survives "list --all" "$rc" "$list_all_out"
echo "$list_all_out" | strip_ansi | grep -q "could not be read" \
  || fail "list --all: expected a note that a subos could not be read; got:
$list_all_out"
echo "$list_all_out" | strip_ansi | grep -q "broken" \
  || fail "list --all: expected the unreadable subos's name ('broken') in the note; got:
$list_all_out"

# This is what actually exercises injection 3: `.overlay.json` still has a
# TRACKED entry for isolocal (added by config --add-xpkg above), but the
# recipe FILE it points at is gone. Nothing else in this test ever reads
# the local overlay's provenance -- the catalog's normal package scan globs
# pkgs/*/*.lua directly and simply never sees a file that isn't there, so
# without this call injection 3 would be pure setup, asserted on by
# nothing. `config --list-xpkg` is the one command that reads
# `.overlay.json` against disk (`load_with_files` + `status_of` per entry)
# and is exactly where a tracked-but-deleted file could crash the whole
# listing instead of just that one row.
log "config --list-xpkg (the deleted overlay file must not crash the listing)"
rc=0; listxpkg_out="$(RUN config --list-xpkg 2>&1)" || rc=$?
assert_survives "config --list-xpkg" "$rc" "$listxpkg_out"
# Not asserting what it says about the deleted entry itself (whatever
# status label that renders as is an implementation detail) -- only that
# the healthy, untouched sibling entry still shows up, i.e. the deleted
# file took down its own row and nothing else.
echo "$listxpkg_out" | strip_ansi | grep -q "isolocalhealthy" \
  || fail "config --list-xpkg: expected the untouched sibling overlay entry
('isolocalhealthy') to still be listed even though isolocal's file is gone; got:
$listxpkg_out"

snap_after="$(snapshot)"
[[ "$snap_after" == "$snap_before" ]] \
  || fail "isoplain's payload tree and this subos's shim table changed just
from a broken subos and a broken sibling package existing in the same home:
  before: $snap_before
  after:  $snap_after"

log "all scenarios passed -- one broken package did not change the others"
