#!/usr/bin/env bash
# install_subindex_first_run_test.sh — regression test for #366:
# "sub-index repos (scode/awesome/d2x) never synced on first run".
#
# Root cause: get_catalog() only ran sync_all_repos() in the rebuild-FAILURE
# fallback, but the main index rebuilds fine without sub-indexes present
# (repo_specs_ skips a sub-repo whose pkgs/ dir doesn't exist and rebuild
# still succeeds). So on a fresh machine `install <ns>:<pkg>` failed with
# "package '<ns>:<pkg>' not found" until the user ran `xlings update`.
#
# Fix (C1, commands.cppm get_catalog): when the sub-index marker JSON
# (xim-indexrepos.json) is missing, force a one-time sync before giving up.
#
# Repro: sync once so the MAIN index lands in HOME/data, then delete the
# sub-index marker + synced dir — that is exactly the #366 state (main
# index present & rebuildable, sub-index absent). Then `install <ns>:<pkg>`
# WITHOUT running `xlings update` must auto-sync and resolve the package.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

HOME_DIR="$(runtime_home_dir subindex_first_run_home)"
SUB_REPO="$ROOT_DIR/tests/fixtures/xim-pkgindex-testd2x-firstrun"

cleanup() {
  rm -rf "$HOME_DIR" "$SUB_REPO"
  rm -f "$FIXTURE_INDEX_DIR/xim-indexrepos.lua"
}
trap cleanup EXIT
cleanup  # start fresh

# ── 1. sub-index fixture (pkg + template + build script) ──
mkdir -p "$SUB_REPO/pkgs/d"
cat > "$SUB_REPO/pkgs/d/d2testpkg.lua" <<'LUAEOF'
package = {
    name = "d2testpkg",
    description = "Test d2x package for first-run sync",
    authors = "test",
    license = "MIT",
    repo = "https://example.com/d2testpkg",
}
LUAEOF
cat > "$SUB_REPO/template.lua" <<'LUAEOF'

package.type = "courses"
package.xpm = {
    linux = { ["latest"] = { url = package.repo .. ".git" } },
    macosx = { ["latest"] = { url = package.repo .. ".git" } },
    windows = { ["latest"] = { url = package.repo .. ".git" } },
}

import("platform")

function installed() return false end
function install() return true end
function uninstall() return true end
LUAEOF
cat > "$SUB_REPO/pkgindex-build.lua" <<'LUAEOF'
package = {
    name = "pkgindex-update",
    description = "Test pkgindex build script",
    xpm = { linux = { ["latest"] = {} } },
}
local projectdir = os.scriptdir()
local pkgsdir = path.join(projectdir, "pkgs")
local template = path.join(projectdir, "template.lua")
function installed() return false end
function install()
    local files = os.files(path.join(pkgsdir, "**.lua"))
    local template_content = io.readfile(template)
    for _, file in ipairs(files) do
        if not file:endswith("pkgindex-update.lua") then
            io.writefile(file, io.readfile(file) .. template_content)
        end
    end
    return true
end
function uninstall() return true end
LUAEOF
(cd "$SUB_REPO" && git init -q && git add -A && git commit -q -m "init")

# ── 2. declare the sub-index in the main index ──
cat > "$FIXTURE_INDEX_DIR/xim-indexrepos.lua" <<LUAEOF
xim_indexrepos = {
    ["testd2x"] = {
        ["GLOBAL"] = "file://$SUB_REPO",
    }
}
LUAEOF

# ── 3. HOME + one sync so the MAIN index lands in HOME/data ──
write_home_config "$HOME_DIR" "GLOBAL"
log "Priming: xlings update (syncs main + sub, writes marker)..."
run_xlings "$HOME_DIR" "$ROOT_DIR" update >/dev/null 2>&1

MARKER="$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
[[ -f "$MARKER" ]] || fail "precondition: marker JSON should exist after update"
# Guard against a vacuous test: the multi-line xim-indexrepos.lua must have been
# parsed and 'testd2x' registered by the priming update (a one-line table would
# silently fail the line-based parser and make the whole test meaningless).
grep -q "testd2x" "$MARKER" || fail "fixture bug: sub-index 'testd2x' not registered by priming update (marker: $(cat "$MARKER"))"

# ── 4. Reproduce the #366 fresh-sub-index state ──
#     main index present (rebuildable) + sub-index marker/dir gone.
log "Simulating #366 state: remove sub-index marker + synced dir..."
rm -f "$MARKER"
rm -rf "$HOME_DIR/data/xim-index-repos/xim-pkgindex-testd2x-firstrun"
[[ -f "$MARKER" ]] && fail "marker should be gone before the install"

# ── 5. install WITHOUT `xlings update` — C1 must auto-sync the sub-index ──
log "install testd2x:d2testpkg with NO prior update (exercises C1)..."
OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" install testd2x:d2testpkg -y 2>&1)" || true
CLEAN="$(strip_ansi <<<"$OUT" | tr -d '\000')"
echo "$CLEAN" | tail -25

# #366 symptom: the resolver returns "not found" and install aborts.
if grep -qE "package 'testd2x:d2testpkg[^']*' not found" <<<"$CLEAN"; then
  fail "#366 regression: sub-index package 'not found' on first run — auto-sync did not resolve it"
fi
# Strong positive signal: the package was resolved into an install plan and
# entered the download stage (only reachable AFTER successful resolution).
# The download itself fails (example.com is unreachable) — that's expected and
# irrelevant; resolution is what #366 is about.
if ! grep -qE "node\(s\) in plan|cloning testd2x:d2testpkg|testd2x:d2testpkg@latest" <<<"$CLEAN"; then
  echo "$CLEAN"
  fail "install did not resolve testd2x:d2testpkg into a plan after first-run auto-sync"
fi
[[ -f "$MARKER" ]] || fail "auto-sync did not re-create the sub-index marker JSON"
grep -q "testd2x" "$MARKER" || fail "auto-sync did not re-register 'testd2x' in the marker"

log "PASS: #366 first-run sub-index auto-sync"
