#!/usr/bin/env bash
# E2E test: verify that the index cache mechanism works correctly.
# 1. After 'xlings update', cache files are created in repo dirs
# 2. Subsequent 'xlings search' loads from cache (no Lua rebuild)
# 3. Deleting cache triggers rebuild on next command
# 4. Changing git HEAD invalidates cache
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

HOME_DIR="$(runtime_home_dir index_cache_home)"

cleanup() {
  rm -rf "$HOME_DIR"
  rm -f "$FIXTURE_INDEX_DIR/xim-indexrepos.lua"
  rm -rf "$ROOT_DIR/tests/fixtures/xim-pkgindex-cachetest"
}
trap cleanup EXIT
cleanup  # start fresh

# ── 1. Create a sub-index fixture ──
SUB_INDEX_DIR="$ROOT_DIR/tests/fixtures/xim-pkgindex-cachetest"
mkdir -p "$SUB_INDEX_DIR/pkgs/c"
cat > "$SUB_INDEX_DIR/pkgs/c/cachepkg.lua" <<'LUAEOF'
package = {
    name = "cachepkg",
    description = "Test package for cache verification",
    authors = "test",
    license = "MIT",
    repo = "https://example.com/cachepkg",
}
LUAEOF
(cd "$SUB_INDEX_DIR" && git init -q && git add -A && git commit -q -m "init")

# Point main fixture to sub-index
cat > "$FIXTURE_INDEX_DIR/xim-indexrepos.lua" <<LUAEOF
xim_indexrepos = {
    ["cachetest"] = {
        ["GLOBAL"] = "file://$SUB_INDEX_DIR",
    }
}
LUAEOF

# ── 2. Set up XLINGS_HOME and run update ──
write_home_config "$HOME_DIR" "GLOBAL"

log "Running xlings update (should create cache files)..."
run_xlings "$HOME_DIR" "$ROOT_DIR" update

# ── 3. Verify cache files exist ──
MAIN_CACHE="$FIXTURE_INDEX_DIR/.xlings-index-cache.json"
[[ -f "$MAIN_CACHE" ]] || fail "main repo cache file not created: $MAIN_CACHE"
log "PASS: main repo cache file exists"

SUB_REPO_DIR="$HOME_DIR/data/xim-index-repos/xim-pkgindex-cachetest"
SUB_CACHE="$SUB_REPO_DIR/.xlings-index-cache.json"
[[ -f "$SUB_CACHE" ]] || fail "sub-repo cache file not created: $SUB_CACHE"
log "PASS: sub-repo cache file exists"

# ── 4. Verify cache contains valid JSON with expected fields ──
grep -q '"repo_head_hash"' "$MAIN_CACHE" || fail "cache missing repo_head_hash field"
grep -q '"entries"' "$MAIN_CACHE" || fail "cache missing entries field"
grep -q '"version":2' "$MAIN_CACHE" || fail "cache is not format v2"
grep -q '"default_namespace":"xim"' "$MAIN_CACHE" \
  || fail "cache missing main repo namespace context"
grep -q '"identity"' "$MAIN_CACHE" || fail "cache missing identity metadata"
grep -q '"canonical_name"' "$MAIN_CACHE" || fail "cache missing canonical names"
grep -q '"entry_key"' "$MAIN_CACHE" || fail "cache missing entry keys"
log "PASS: cache file has expected structure"

# ── 5. Record cache mtime, run search, verify cache was NOT rewritten ──
MAIN_CACHE_MTIME_BEFORE="$(stat -c %Y "$MAIN_CACHE" 2>/dev/null || stat -f %m "$MAIN_CACHE")"
sleep 1  # ensure mtime granularity

log "Running xlings search gcc (should use cache)..."
SEARCH_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" search gcc 2>&1)"
assert_contains "$SEARCH_OUT" "gcc" "search did not find gcc"

MAIN_CACHE_MTIME_AFTER="$(stat -c %Y "$MAIN_CACHE" 2>/dev/null || stat -f %m "$MAIN_CACHE")"
[[ "$MAIN_CACHE_MTIME_BEFORE" == "$MAIN_CACHE_MTIME_AFTER" ]] \
  || fail "cache was rewritten during search (expected cache hit)"
log "PASS: search used cache (mtime unchanged)"

# ── 6. Verify sub-index package visible via cache ──
log "Running xlings search cachepkg..."
SEARCH_SUB_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" search cachepkg 2>&1)"
echo "$SEARCH_SUB_OUT"
assert_contains "$SEARCH_SUB_OUT" "cachepkg" "search did not find cachepkg from sub-index"
log "PASS: sub-index package found via cache"

# ── 7. Delete cache, verify rebuild on next command ──
rm -f "$MAIN_CACHE"
log "Running xlings search gcc after cache delete..."
SEARCH_OUT2="$(run_xlings "$HOME_DIR" "$ROOT_DIR" search gcc 2>&1)"
assert_contains "$SEARCH_OUT2" "gcc" "search did not find gcc after cache delete"
[[ -f "$MAIN_CACHE" ]] || fail "cache was not rebuilt after deletion"
log "PASS: cache rebuilt after deletion"

# ── 8. Modify sub-index git HEAD (new commit), verify cache invalidation ──
SUB_CACHE_HASH_BEFORE="$(grep -o '"repo_head_hash":"[^"]*"' "$SUB_CACHE" || true)"

cat > "$SUB_INDEX_DIR/pkgs/c/cachepkg2.lua" <<'LUAEOF'
package = {
    name = "cachepkg2",
    description = "Second test package for cache invalidation",
    authors = "test",
    license = "MIT",
    repo = "https://example.com/cachepkg2",
}
LUAEOF
(cd "$SUB_INDEX_DIR" && git add -A && git commit -q -m "add cachepkg2")

# Pull the change into the synced sub-repo
(cd "$SUB_REPO_DIR" && git pull -q origin HEAD 2>/dev/null || true)

log "Running xlings search cachepkg2 (should trigger cache rebuild)..."
SEARCH_NEW_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" search cachepkg2 2>&1)"
echo "$SEARCH_NEW_OUT"
assert_contains "$SEARCH_NEW_OUT" "cachepkg2" "search did not find cachepkg2 after sub-index update"

SUB_CACHE_HASH_AFTER="$(grep -o '"repo_head_hash":"[^"]*"' "$SUB_CACHE" || true)"
[[ "$SUB_CACHE_HASH_BEFORE" != "$SUB_CACHE_HASH_AFTER" ]] \
  || fail "sub-repo cache hash was not updated after new commit"
log "PASS: cache invalidated after git HEAD change"

log "PASS: index cache e2e tests"
