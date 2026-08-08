#!/usr/bin/env bash
# E2E: `remove` must not report success and leave the payload behind (#511).
#
# The gate at src/core/xim/commands.cppm decided "is this installed?" from two
# workspace tables. A package can be absent from both while its payload sits in
# data/xpkgs/ -- `install` put it there. Answering "not installed" then exits 0
# having removed nothing and having skipped the recipe's uninstall() hook.
#
# The exit code is the least of it. `install` treats a present payload
# directory as already installed and skips the install AND its config() hook,
# so
#
#   install -> remove (reports success, payload stays) -> install
#
# leaves a package whose config() never runs again. That is the cascade behind
# the aarch64 failure in #509: an unconfigured gcc payload emitted binaries
# carrying the packaging machine's PT_INTERP, and the resulting error
# (posix_spawnp ... error 2) named the wrong file entirely.
#
# What is asserted, and why each one is here:
#   1. exit 0                  -- removal succeeds
#   2. uninstall() hook ran    -- exit 0 alone cannot tell "removed" from
#                                 "silently did nothing"; the fixture's hook
#                                 drops a marker file
#   3. payload directory gone  -- the invariant that actually matters
#
# NOT covered here: the cross-subos case the same gate was written for (0.4.19+
# -- package lives in ANOTHER subos, removal must still refuse). That behaviour
# is deliberately preserved and is exercised by remove_multi_version_test.sh.

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/remove_orphan_payload"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
HOME_DIR="$RUNTIME_DIR/home"
MARKER="$RUNTIME_DIR/uninstall-ran.marker"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

BIN="$(find_xlings_bin)"
log "client: $("$BIN" --version 2>&1 | head -1)"

cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/o"

# Registers no xvm version on purpose: that is what keeps it out of both
# workspace tables, which is the state under test.
cat > "$LOCAL_INDEX_DIR/pkgs/o/orphanpay.lua" <<LUA
package = {
    spec = "1",
    name = "orphanpay",
    description = "Local fixture for tests/e2e/remove_orphan_payload_test.sh",
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

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    io.writefile(path.join(dir, "bin", "orphanpay"), "#!/bin/sh\necho orphanpay\n")
    return true
end

function config()
    return true      -- registers nothing, deliberately
end

function uninstall()
    io.writefile("$MARKER", "ran")
    return true
end
LUA

mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/data/xim-index-repos"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$BIN" "$@" ) }

x self init >/dev/null 2>&1 || true

# ── install ─────────────────────────────────────────────────────────────
OUT="$(x install orphanpay@1.0.0 -y 2>&1)" \
  || { echo "$OUT" >&2; fail "install failed"; }

PAYLOAD="$(find "$HOME_DIR/data/xpkgs" -maxdepth 2 -type d -name '1.0.0' \
             -path '*orphanpay*' 2>/dev/null | head -1)"
[ -n "$PAYLOAD" ] && [ -d "$PAYLOAD" ] \
  || fail "install produced no payload directory -- the test cannot mean anything"
log "  ✓ installed, payload at ${PAYLOAD#"$HOME_DIR/"}"

# Precondition guard. If the fixture DID land in a workspace table, the branch
# under test is never reached and everything below passes vacuously.
IN_WS="$(python3 -c '
import json,sys
d = json.load(open(sys.argv[1]))
ws  = d.get("workspace", {}) or {}
wsi = d.get("workspace_installed", {}) or {}
print(int(bool(ws.get("orphanpay")) or bool(wsi.get("orphanpay"))))' \
  "$HOME_DIR/.xlings.json" 2>/dev/null || echo 0)"
[ "$IN_WS" = "0" ] \
  || fail "fixture landed in a workspace table; it must be in neither for this test"
log "  ✓ in neither workspace table (the precondition)"

# ── remove ──────────────────────────────────────────────────────────────
rm -f "$MARKER"
OUT="$(x remove orphanpay -y 2>&1)"; RC=$?
printf '%s\n' "$OUT" | sed 's/^/      | /'

[ "$RC" = "0" ] || fail "remove exited $RC"
log "  ✓ remove succeeds"

[ -f "$MARKER" ] \
  || fail "remove returned 0 but the recipe's uninstall() hook never ran"
log "  ✓ the recipe's uninstall() hook ran"

[ ! -d "$PAYLOAD" ] \
  || fail "remove returned 0 and LEFT the payload at $PAYLOAD -- the next install will skip config()"
log "  ✓ payload is gone"

log "E2E remove-orphan-payload: PASS"
