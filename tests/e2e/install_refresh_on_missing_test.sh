#!/usr/bin/env bash
# install_refresh_on_missing_test.sh — verifies the on-demand index refresh
# (C2, #366 UX): when `xlings install <target>` cannot resolve a target from
# the CURRENT (already-initialized) index, it refreshes the index once and
# retries before failing — so a freshly-published package/version installs
# without a manual `xlings update` first.
#
# Distinct from install_subindex_first_run_test.sh (C1): here the sub-index
# marker is PRESENT (not a first run), so C1 does not fire. The target is
# simply newer than the local index — only C2 can resolve it.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

HOME_DIR="$(runtime_home_dir refresh_on_missing_home)"
SUB_REPO="$ROOT_DIR/tests/fixtures/xim-pkgindex-testd2x-c2"

cleanup() {
  rm -rf "$HOME_DIR" "$SUB_REPO"
  rm -f "$FIXTURE_INDEX_DIR/xim-indexrepos.lua"
}
trap cleanup EXIT
cleanup

# ── 1. sub-index fixture with one initial package ──
mkdir -p "$SUB_REPO/pkgs/d"
cat > "$SUB_REPO/pkgs/d/d2testpkg.lua" <<'LUAEOF'
package = { name = "d2testpkg", description = "initial pkg", authors = "test", license = "MIT", repo = "https://example.com/d2testpkg" }
LUAEOF
cat > "$SUB_REPO/template.lua" <<'LUAEOF'

package.type = "courses"
package.xpm = { linux = { ["latest"] = { url = package.repo .. ".git" } } }
import("platform")
function installed() return false end
function install() return true end
function uninstall() return true end
LUAEOF
cat > "$SUB_REPO/pkgindex-build.lua" <<'LUAEOF'
package = { name = "pkgindex-update", xpm = { linux = { ["latest"] = {} } } }
local pd = path.join(os.scriptdir(), "pkgs")
local template = path.join(os.scriptdir(), "template.lua")
function installed() return false end
function install()
    local tc = io.readfile(template)
    for _, f in ipairs(os.files(path.join(pd, "**.lua"))) do
        if not f:endswith("pkgindex-update.lua") then io.writefile(f, io.readfile(f) .. tc) end
    end
    return true
end
function uninstall() return true end
LUAEOF
(cd "$SUB_REPO" && git init -q && git add -A && git commit -q -m "init")

cat > "$FIXTURE_INDEX_DIR/xim-indexrepos.lua" <<LUAEOF
xim_indexrepos = {
    ["testd2x"] = {
        ["GLOBAL"] = "file://$SUB_REPO",
    }
}
LUAEOF

# ── 2. HOME + full sync (marker + sub-index present; NOT a first run) ──
write_home_config "$HOME_DIR" "GLOBAL"
log "Priming: full xlings update..."
run_xlings "$HOME_DIR" "$ROOT_DIR" update >/dev/null 2>&1
MARKER="$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
grep -q "testd2x" "$MARKER" || fail "precondition: testd2x should be registered after update"

# ── 3. Publish a NEW package to the sub-repo AFTER the sync (local index stale) ──
log "Publishing newpkg to the sub-index (local index is now stale)..."
mkdir -p "$SUB_REPO/pkgs/n"
cat > "$SUB_REPO/pkgs/n/newpkg.lua" <<'LUAEOF'
package = { name = "newpkg", description = "freshly published", authors = "test", license = "MIT", repo = "https://example.com/newpkg" }
LUAEOF
(cd "$SUB_REPO" && git add -A && git commit -q -m "add newpkg")

# ── 4. install the fresh package WITHOUT `xlings update` — C2 must refresh ──
log "install testd2x:newpkg (not in stale local index; exercises C2)..."
OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" install testd2x:newpkg -y 2>&1)" || true
CLEAN="$(strip_ansi <<<"$OUT" | tr -d '\000')"
echo "$CLEAN" | grep -iE "refreshing|Packages to install|newpkg@latest|not found" | head

# C2's on-demand refresh must have fired...
assert_contains "$CLEAN" "not in current index; refreshing index" \
  "C2 on-demand refresh did not trigger for a missing target"
# ...and the package must resolve into a plan after the refresh.
if ! grep -qE "node\(s\) in plan|cloning testd2x:newpkg|newpkg@latest" <<<"$CLEAN"; then
  echo "$CLEAN"
  fail "testd2x:newpkg did not resolve after on-demand index refresh (C2)"
fi

log "PASS: on-demand index refresh resolves a freshly-published package (C2)"
