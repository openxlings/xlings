#!/usr/bin/env bash
# E2E test for `xlings update <pkg>` (specific package upgrade).
#
# Scenarios:
#   1. update non-installed pkg     → warn, exit 0
#   2. update active=1.0.0          → upgrade to latest (2.0.0); old version
#                                     remains installed (multi-version semantics)
#   3. update again                 → "already the latest"; no install attempt
#   4. bare `xlings update`         → only refreshes the index, exit 0
#   5. update active=1.0.0 while 2.0.0 is already in the store
#                                   → "is in the store", then
#                                     "active: 1.0.0 -> 2.0.0"; never "upgraded"
#   6. bare `xlings update` with a stale index cache
#                                   → the index's pkgindex-build.lua runs ONCE

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/update_package"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/u/upgrade-fixture.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR"

# Private copy of the shared fixture index — neutralise the sub-index-repos
# fetch so the test stays offline.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

# Fixture: two versions, no URL (no downloads), simple install hook.
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "upgrade-fixture",
    description = "Local fixture for tests/e2e/update_package_test.sh",
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
    xvm.add("upgrade-fixture", { bindir = pkginfo.install_dir() })
    return true
end

function uninstall()
    xvm.remove("upgrade-fixture")
    return true
end
LUA

# Seed XLINGS_HOME
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

STORE_DIR="$HOME_DIR/data/xpkgs/xim-x-upgrade-fixture"

# ── Scenario 1: update on non-installed pkg → warn, exit 0 ─────────
log "Scenario 1: update upgrade-fixture (not installed) → warn"
OUT_S1="$(RUN update upgrade-fixture -y 2>&1)" || fail "S1: command exited non-zero"
echo "$OUT_S1" | grep -q "is not installed" \
  || fail "S1: expected 'is not installed' warning; got:\n$OUT_S1"

# Confirm nothing was installed
[[ ! -d "$STORE_DIR" ]] || fail "S1: store dir should not exist before install"

# ── Scenario 2: install 1.0.0 first, then update → upgrade to 2.0.0 ─
log "Install upgrade-fixture@1.0.0 (precondition for S2)"
RUN install "upgrade-fixture@1.0.0" -y >/dev/null 2>&1 \
  || fail "install upgrade-fixture@1.0.0 failed"
[[ -f "$STORE_DIR/1.0.0/VERSION" ]] || fail "S2 setup: 1.0.0 marker missing"

python3 - "$HOME_DIR" 1.0.0 <<'PY' || fail "S2 setup: workspace active version wrong"
import json, sys, pathlib
home, want = sys.argv[1:]
ws = json.loads(pathlib.Path(home, "subos/default/.xlings.json").read_text())
entry = (ws.get("workspace") or {}).get("upgrade-fixture")
got = entry.get("active") if isinstance(entry, dict) else entry  # 0.4.19 schema
assert got == want, f"expected active={want}, got {got!r}"
PY

log "Scenario 2: update upgrade-fixture (1.0.0 → 2.0.0)"
OUT_S2="$(RUN update upgrade-fixture -y 2>&1)" || fail "S2: command exited non-zero"
echo "$OUT_S2" | grep -q "upgraded" \
  || fail "S2: expected 'upgraded' summary; got:\n$OUT_S2"

[[ -f "$STORE_DIR/1.0.0/VERSION" ]] \
  || fail "S2: 1.0.0 payload should be retained (multi-version)"
[[ -f "$STORE_DIR/2.0.0/VERSION" ]] \
  || fail "S2: 2.0.0 payload should be installed"

python3 - "$HOME_DIR" 2.0.0 <<'PY' || fail "S2 DB state wrong"
import json, sys, pathlib
home, want = sys.argv[1:]
ws = json.loads(pathlib.Path(home, "subos/default/.xlings.json").read_text())
entry = (ws.get("workspace") or {}).get("upgrade-fixture")
got = entry.get("active") if isinstance(entry, dict) else entry  # 0.4.19 schema
assert got == want, f"S2: active should switch to {want}, got {got!r}"
PY

# ── Scenario 3: update again → already-latest no-op ─────────────────
log "Scenario 3: update upgrade-fixture (already at 2.0.0) → no-op"
OUT_S3="$(RUN update upgrade-fixture -y 2>&1)" || fail "S3: command exited non-zero"
echo "$OUT_S3" | grep -q "already the latest" \
  || fail "S3: expected 'already the latest'; got:\n$OUT_S3"

# ── Scenario 4: bare `update` refreshes index, exits 0 ──────────────
log "Scenario 4: bare update (no target) refreshes index"
OUT_S4="$(RUN update 2>&1)" || fail "S4: command exited non-zero"
echo "$OUT_S4" | grep -q "index updated" \
  || fail "S4: expected 'index updated'; got:\n$OUT_S4"

# ── Scenario 5: the target is already in the store ──────────────────
# 2.0.0 stays in the store after S2; switching back to 1.0.0 makes `update`
# move the active version without installing anything. The two lines it prints
# say that. Before this was worded, the output was "is already installed"
# followed by "upgraded ...: 1.0.0 -> 2.0.0" -- both true, and read together a
# contradiction.
log "Scenario 5: update upgrade-fixture while 2.0.0 is in the store"
RUN use upgrade-fixture 1.0.0 >/dev/null 2>&1 || fail "S5 setup: use 1.0.0 failed"
OUT_S5="$(RUN update upgrade-fixture -y 2>&1)" || fail "S5: command exited non-zero"
echo "$OUT_S5" | grep -qF "xim:upgrade-fixture@2.0.0 is in the store" \
  || fail "S5: expected 'xim:upgrade-fixture@2.0.0 is in the store'; got:\n$OUT_S5"
echo "$OUT_S5" | grep -qF "active: 1.0.0 -> 2.0.0" \
  || fail "S5: expected 'active: 1.0.0 -> 2.0.0'; got:\n$OUT_S5"
if echo "$OUT_S5" | grep -qE "is already installed|upgraded "; then
  fail "S5: a store hit must not read as 'already installed' or 'upgraded'; got:\n$OUT_S5"
fi

# ── Scenario 6: one rebuild per update ─────────────────────────────
# The index's pkgindex-build.lua runs on every catalog build and records each
# run in a file. With the cache deleted, the catalog's implicit first build and
# the forced rebuild `update` performs were two builds; `update` now performs
# only the forced one.
log "Scenario 6: bare update runs the index build script once"
RUNS_FILE="$RUNTIME_DIR/pkgindex-build-runs.log"
rm -f "$RUNS_FILE"
cat > "$LOCAL_INDEX_DIR/pkgindex-build.lua" <<LUA
package = { name = "pkgindex-update", namespace = "fixture" }
function installed() return false end
function install()
    local f = io.open("$RUNS_FILE", "a")
    if f then f:write("run\\n") f:close() end
    print("[1/1] fixture::upgrade-fixture")
    return true
end
function uninstall() return true end
LUA
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
OUT_S6="$(RUN update 2>&1)" || fail "S6: command exited non-zero:\n$OUT_S6"
[[ -f "$RUNS_FILE" ]] || fail "S6: the index build script never ran; got:\n$OUT_S6"
runs=$(grep -c '^run$' "$RUNS_FILE")
[[ "$runs" -eq 1 ]] \
  || fail "S6: the index build script ran $runs times for one update; got:\n$OUT_S6"

log "PASS: update <pkg> scenarios 1-6"
