#!/usr/bin/env bash
# E2E regression: uninstall must use the recipe the payload kept
# (install_dir/.xpkg/pkgs/<dir>/<name>.lua, with its index libs; .xpkg.lua
# before 2026.11 -- design 2026-11-27 §20.1) instead of the current package
# index when present.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/xpkg_snapshot_remove"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/s/snapshot-fixture.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR"

# Private copy of the shared fixture index and no nested index fetches, so the
# test stays offline and can mutate its package definition safely.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "snapshot-fixture",
    description = "Local fixture for tests/e2e/xpkg_snapshot_remove_test.sh",
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
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(dir)
    io.writefile(path.join(dir, "VERSION"), pkginfo.version())
    return true
end

function config()
    xvm.add("snapshot-fixture", { bindir = pkginfo.install_dir() })
    return true
end

function uninstall()
    io.writefile(path.join(pkginfo.install_dir(), "..", "SNAPSHOT_UNINSTALL_USED"), pkginfo.version())
    xvm.remove("snapshot-fixture")
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

STORE_DIR="$HOME_DIR/data/xpkgs/xim-x-snapshot-fixture"
INSTALL_DIR="$STORE_DIR/1.0.0"
snapshot_file() { ls "$INSTALL_DIR"/.xpkg/pkgs/*/*.lua 2>/dev/null | head -1; }
SNAPSHOT_MARKER="$STORE_DIR/SNAPSHOT_UNINSTALL_USED"
CURRENT_MARKER="$STORE_DIR/CURRENT_INDEX_UNINSTALL_USED"

log "Install snapshot-fixture@1.0.0"
RUN install snapshot-fixture@1.0.0 -y >/dev/null 2>&1 \
  || fail "install snapshot-fixture@1.0.0 failed"

[[ -f "$INSTALL_DIR/VERSION" ]] || fail "installed payload marker missing"
SNAPSHOT_FILE="$(snapshot_file)"
[[ -n "$SNAPSHOT_FILE" && -f "$SNAPSHOT_FILE" ]] || fail "expected the kept recipe under $INSTALL_DIR/.xpkg/pkgs"
[[ ! -e "$INSTALL_DIR/.xpkg.lua" ]] || fail "the previous layout's .xpkg.lua is still written"
grep -q "SNAPSHOT_UNINSTALL_USED" "$SNAPSHOT_FILE" \
  || fail "snapshot should contain the original uninstall hook"

cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "snapshot-fixture",
    description = "Mutated fixture used to verify uninstall does not load the current index",
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
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(dir)
    io.writefile(path.join(dir, "VERSION"), pkginfo.version())
    return true
end

function config()
    xvm.add("snapshot-fixture", { bindir = pkginfo.install_dir() })
    return true
end

function uninstall()
    io.writefile(path.join(pkginfo.install_dir(), "..", "CURRENT_INDEX_UNINSTALL_USED"), pkginfo.version())
    xvm.remove("snapshot-fixture")
    return true
end
LUA

log "Remove snapshot-fixture after mutating the current index"
RUN remove snapshot-fixture -y >/dev/null 2>&1 \
  || fail "remove snapshot-fixture failed"

[[ -f "$SNAPSHOT_MARKER" ]] \
  || fail "remove should have used the snapshotted uninstall hook"
[[ ! -f "$CURRENT_MARKER" ]] \
  || fail "remove used the mutated current-index uninstall hook"

log "PASS: remove prefers the recipe the payload kept over the current index"
