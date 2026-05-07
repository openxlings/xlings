#!/usr/bin/env bash
# E2E test: `xlings install` must be idempotent in the current sub-OS.
#
# Bug being fixed: `install` was silently upgrading to the catalog's
# highest declared version whenever the user typed a bare name (or a
# version prefix) and any version was already active in the sub-OS.
# `install` should be a no-op in that case; users wanting an upgrade
# should use `xlings update <pkg>`.
#
# Scenarios:
#   1. install bare name, fresh subos     → installs latest declared
#                                           (baseline behavior, unchanged)
#   2. install bare name, 1.0.0 active    → "all packages already installed"
#                                           (NO upgrade to 2.0.0)
#   3. install <name>@1, 1.0.0 active     → "all packages already installed"
#                                           (active matches the prefix)
#   4. install <name>@2, 1.0.0 active     → installs 2.0.0
#                                           (active doesn't match prefix)

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/install_idempotent"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/i/idempotent-fixture.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR"

# Private copy of the shared fixture index — neutralise sub-index repos
# so the test stays offline.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

# Two declared versions; install hook writes a marker file so we can
# verify on disk which versions are present.
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "idempotent-fixture",
    description = "Local fixture for tests/e2e/install_idempotent_test.sh",
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
    os.tryrm(dir)
    os.mkdir(dir)
    io.writefile(path.join(dir, "VERSION"), pkginfo.version())
    return true
end

function config()
    xvm.add("idempotent-fixture", { bindir = pkginfo.install_dir() })
    return true
end

function uninstall()
    xvm.remove("idempotent-fixture")
    return true
end
LUA

# Seed XLINGS_HOME pointing at the private (neutralised) index
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

STORE_DIR="$HOME_DIR/data/xpkgs/xim-x-idempotent-fixture"

# ── Scenario 1: bare name, fresh subos → installs latest declared ─
log "Scenario 1: install idempotent-fixture (fresh subos) → install 2.0.0"
RUN install idempotent-fixture -y >/dev/null 2>&1 \
  || fail "S1: install failed"
[[ -f "$STORE_DIR/2.0.0/VERSION" ]] \
  || fail "S1: latest version 2.0.0 should have been installed"
[[ ! -d "$STORE_DIR/1.0.0" ]] \
  || fail "S1: 1.0.0 should NOT have been installed"

# Reset to install only 1.0.0 so subsequent scenarios start with that as active
log "Reset: remove 2.0.0, install 1.0.0 explicitly"
RUN remove idempotent-fixture -y >/dev/null 2>&1 || fail "reset: remove failed"
RUN install idempotent-fixture@1.0.0 -y >/dev/null 2>&1 \
  || fail "reset: install 1.0.0 failed"
[[ -f "$STORE_DIR/1.0.0/VERSION" ]] || fail "reset: 1.0.0 should be on disk"
[[ ! -d "$STORE_DIR/2.0.0" ]] || fail "reset: 2.0.0 should NOT be on disk"

python3 - "$HOME_DIR" 1.0.0 <<'PY' || fail "reset: workspace active version wrong"
import json, sys, pathlib
home, want = sys.argv[1:]
ws = json.loads(pathlib.Path(home, "subos/default/.xlings.json").read_text())
entry = (ws.get("workspace") or {}).get("idempotent-fixture")
got = entry.get("active") if isinstance(entry, dict) else entry  # 0.4.19 schema
assert got == want, f"expected active={want}, got {got!r}"
PY

# ── Scenario 2: bare name with 1.0.0 active → no upgrade ───────────
log "Scenario 2: install idempotent-fixture (1.0.0 active) → no upgrade"
OUT_S2="$(RUN install idempotent-fixture -y 2>&1)" || fail "S2: install exited non-zero"
echo "$OUT_S2" | grep -q "already installed" \
  || fail "S2: expected 'already installed'; got:\n$OUT_S2"
[[ ! -d "$STORE_DIR/2.0.0" ]] \
  || fail "S2: 2.0.0 must NOT be installed (this is the bug being fixed)"

# ── Scenario 3: name@<prefix> matches active → no upgrade ──────────
log "Scenario 3: install idempotent-fixture@1 (1.0.0 active) → no upgrade"
OUT_S3="$(RUN install idempotent-fixture@1 -y 2>&1)" || fail "S3: install exited non-zero"
echo "$OUT_S3" | grep -q "already installed" \
  || fail "S3: expected 'already installed'; got:\n$OUT_S3"
[[ ! -d "$STORE_DIR/2.0.0" ]] \
  || fail "S3: 2.0.0 must NOT be installed (prefix match against active)"

# ── Scenario 4: name@<prefix> doesn't match active → install latest 2.x ─
log "Scenario 4: install idempotent-fixture@2 (1.0.0 active) → install 2.0.0"
RUN install idempotent-fixture@2 -y >/dev/null 2>&1 \
  || fail "S4: install failed"
[[ -f "$STORE_DIR/2.0.0/VERSION" ]] \
  || fail "S4: 2.0.0 should have been installed"
[[ -f "$STORE_DIR/1.0.0/VERSION" ]] \
  || fail "S4: 1.0.0 should still be retained"

log "PASS: install idempotency scenarios 1, 2, 3, 4"
