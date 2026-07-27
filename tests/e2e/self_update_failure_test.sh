#!/usr/bin/env bash
# E2E: `xlings self update` must fail loudly when the upgrade does not happen.
#
# Regression. cmd_update used to run `xlings install xlings@latest -y`, and on
# a non-zero exit log a *warning* and `return 0`. A failed upgrade was then
# indistinguishable from a successful one: the install error scrolled past
# under the progress bar, the command exited 0, and the user carried on with
# the old binary believing they were current.
#
# Seen in the field on 2026-07-27: a 0.4.69 client resolving xlings@2026.7.27.3
# against a CN mirror that had not been topped up yet got HTTP 404, and
# `self update` still reported success.
#
# The fixture makes the install hook fail outright, which is the same shape as
# a 404 from cmd_update's point of view: `xlings install` exits non-zero.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/self_update_failure"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/x/xlings.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

# `self update` shells out to `xlings ...` three times rather than calling
# cmd_install/cmd_use directly (see core/xself/update.cppm on why), so the
# bootstrap has to be reachable on PATH for the subprocesses to resolve.
RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH="$HOME_DIR/bin:/usr/bin:/bin" \
      XLINGS_HOME="$HOME_DIR" "$HOME_DIR/bin/xlings" "$@" )
}

mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/bin"

cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

# A published xlings@9.9.9 whose install hook fails. No URL, so nothing is
# downloaded and the failure is deterministic and offline.
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "xlings",
    description = "Local fixture for tests/e2e/self_update_failure_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["latest"] = { ref = "9.9.9" }, ["9.9.9"] = {} },
        macosx  = { ["latest"] = { ref = "9.9.9" }, ["9.9.9"] = {} },
        windows = { ["latest"] = { ref = "9.9.9" }, ["9.9.9"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    -- Stands in for "the download 404'd": install exits non-zero.
    return false
end

function config()
    xvm.add("xlings", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("xlings")
    return true
end
LUA

cp "$XLINGS_BIN" "$HOME_DIR/bin/xlings"
chmod +x "$HOME_DIR/bin/xlings"
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

# Premise: the install this exercises really does fail on its own.
rc=0
RUN install xlings@latest -y >/dev/null 2>&1 || rc=$?
[[ $rc -ne 0 ]] \
  || fail "premise broken: fixture install should fail, but exited 0"

# ── S1: self update propagates the failure ─────────────────────────
log "S1: self update with a failing install → non-zero exit"
rc=0
out=$(RUN self update 2>&1) || rc=$?
[[ $rc -ne 0 ]] \
  || fail "S1: self update must NOT report success when the upgrade failed (got 0)"

# ── S2: and says so in words the user can act on ───────────────────
log "S2: self update explains that the version did not change"
printf '%s\n' "$out" | grep -qi "still on the current version" \
  || fail "S2: output should state the version did not change; got:\n$out"
printf '%s\n' "$out" | grep -q "xlings install xlings@latest" \
  || fail "S2: output should point at the command that shows why; got:\n$out"

log "PASS: self update failure propagation (S1-S2)"
