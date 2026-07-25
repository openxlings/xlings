#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/index_same_name_namespace"
HOME_DIR="$RUNTIME_DIR/home"
SOURCE_INDEX_DIR="$ROOT_DIR/tests/fixtures/index-same-name"
INDEX_DIR="$RUNTIME_DIR/index"
DUPLICATE_SOURCE_DIR="$ROOT_DIR/tests/fixtures/index-duplicate"
DUPLICATE_INDEX_DIR="$RUNTIME_DIR/duplicate-index"
DUPLICATE_HOME_DIR="$RUNTIME_DIR/duplicate-home"

cleanup() {
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

mkdir -p "$INDEX_DIR"
cp -R "$SOURCE_INDEX_DIR/." "$INDEX_DIR/"
(cd "$INDEX_DIR" && git init -q && git add -A && git commit -q -m "init")

write_home_config "$HOME_DIR" "GLOBAL" "$INDEX_DIR"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

log "search exposes both canonical identities"
SEARCH_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" search demo 2>&1)"
assert_contains "$SEARCH_OUT" "alpha:demo" "search missing alpha:demo"
assert_contains "$SEARCH_OUT" "beta:demo" "search missing beta:demo"

INDEX_CACHE="$INDEX_DIR/.xlings-index-cache.json"
[[ -f "$INDEX_CACHE" ]] || fail "namespace index cache was not created"
grep -q '"version":2' "$INDEX_CACHE" || fail "namespace cache is not v2"
grep -q '"default_namespace":"xim"' "$INDEX_CACHE" \
  || fail "namespace cache missing default namespace context"
grep -q '"alpha:demo"' "$INDEX_CACHE" || fail "cache missing alpha:demo"
grep -q '"beta:demo"' "$INDEX_CACHE" || fail "cache missing beta:demo"
grep -q '"canonical_name":"alpha:demo"' "$INDEX_CACHE" \
  || fail "cache missing alpha canonical identity"

log "valid v2 cache is reused without rewrite"
CACHE_MTIME_BEFORE="$(stat -c %Y "$INDEX_CACHE" 2>/dev/null || stat -f %m "$INDEX_CACHE")"
sleep 1
run_xlings "$HOME_DIR" "$ROOT_DIR" search demo >/dev/null 2>&1
CACHE_MTIME_AFTER="$(stat -c %Y "$INDEX_CACHE" 2>/dev/null || stat -f %m "$INDEX_CACHE")"
[[ "$CACHE_MTIME_BEFORE" == "$CACHE_MTIME_AFTER" ]] \
  || fail "valid v2 cache was unexpectedly rewritten"

log "v1 cache is rejected and rebuilt as v2"
printf '{"version":1,"repo_head_hash":"stale","entries":{}}\n' > "$INDEX_CACHE"
run_xlings "$HOME_DIR" "$ROOT_DIR" search demo >/dev/null 2>&1
grep -q '"version":2' "$INDEX_CACHE" || fail "v1 cache was not rebuilt as v2"

log "corrupt identity cache fails closed to rebuild"
sed '0,/"namespace":"alpha"/s//"namespace":"corrupt"/' \
  "$INDEX_CACHE" > "$INDEX_CACHE.corrupt"
mv "$INDEX_CACHE.corrupt" "$INDEX_CACHE"
CORRUPT_RECOVERY="$(run_xlings "$HOME_DIR" "$ROOT_DIR" info alpha:demo 2>&1)"
assert_contains "$CORRUPT_RECOVERY" "Alpha namespace demo package" \
  "corrupt identity cache did not rebuild"
grep -q '"namespace":"alpha"' "$INDEX_CACHE" \
  || fail "rebuilt cache did not restore alpha identity"
if grep -q '"namespace":"corrupt"' "$INDEX_CACHE"; then
  fail "corrupt identity survived cache validation"
fi

log "explicit identities resolve independently"
ALPHA_INFO="$(run_xlings "$HOME_DIR" "$ROOT_DIR" info alpha:demo 2>&1)"
BETA_INFO="$(run_xlings "$HOME_DIR" "$ROOT_DIR" info beta:demo 2>&1)"
assert_contains "$ALPHA_INFO" "Alpha namespace demo package" \
  "alpha:demo resolved to the wrong descriptor"
assert_contains "$BETA_INFO" "Beta namespace demo package" \
  "beta:demo resolved to the wrong descriptor"

log "bare name fails with stable ambiguity candidates"
set +e
BARE_INFO="$(run_xlings "$HOME_DIR" "$ROOT_DIR" info demo 2>&1)"
BARE_RC=$?
set -e
[[ "$BARE_RC" -ne 0 ]] || fail "bare demo unexpectedly resolved"
assert_contains "$BARE_INFO" "package 'demo' is ambiguous" \
  "bare demo did not report ambiguity"
assert_contains "$BARE_INFO" "1. alpha:demo@1.0.0" \
  "alpha:demo is not the first stable candidate"
assert_contains "$BARE_INFO" "2. beta:demo@1.0.0" \
  "beta:demo is not the second stable candidate"

log "explicit dependency preserves canonical identity"
EXPLICIT_PLAN="$(run_xlings "$HOME_DIR" "$ROOT_DIR" interface plan_install \
  --args '{"targets":["alpha:explicit-consumer"]}' 2>&1)"
assert_contains "$EXPLICIT_PLAN" '"alpha:demo@1.0.0"' \
  "explicit dependency did not resolve alpha:demo"
assert_contains "$EXPLICIT_PLAN" '"alpha:explicit-consumer@1.0.0"' \
  "explicit consumer missing from plan"

log "bare dependency does not inherit the declaring namespace"
set +e
BARE_PLAN="$(run_xlings "$HOME_DIR" "$ROOT_DIR" interface plan_install \
  --args '{"targets":["alpha:bare-consumer"]}' 2>&1)"
BARE_PLAN_RC=$?
set -e
[[ "$BARE_PLAN_RC" -ne 0 ]] \
  || fail "bare dependency unexpectedly inherited namespace alpha"
assert_contains "$BARE_PLAN" "package 'demo' is ambiguous" \
  "bare dependency did not preserve catalog ambiguity"
assert_contains "$BARE_PLAN" "alpha:demo@1.0.0" \
  "bare dependency error missing alpha candidate"
assert_contains "$BARE_PLAN" "beta:demo@1.0.0" \
  "bare dependency error missing beta candidate"

log "duplicate effective identity fails with both descriptor paths"
mkdir -p "$DUPLICATE_INDEX_DIR"
cp -R "$DUPLICATE_SOURCE_DIR/." "$DUPLICATE_INDEX_DIR/"
(cd "$DUPLICATE_INDEX_DIR" && git init -q && git add -A && git commit -q -m "init")
write_home_config "$DUPLICATE_HOME_DIR" "GLOBAL" "$DUPLICATE_INDEX_DIR"
mkdir -p "$DUPLICATE_HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$DUPLICATE_HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
set +e
DUPLICATE_OUT="$(run_xlings "$DUPLICATE_HOME_DIR" "$ROOT_DIR" search demo 2>&1)"
DUPLICATE_RC=$?
set -e
[[ "$DUPLICATE_RC" -ne 0 ]] || fail "duplicate xim:demo unexpectedly built"
assert_contains "$DUPLICATE_OUT" "duplicate package identity 'xim:demo'" \
  "duplicate identity error missing canonical identity"
assert_contains "$DUPLICATE_OUT" "implicit.demo.lua" \
  "duplicate identity error missing implicit descriptor path"
assert_contains "$DUPLICATE_OUT" "explicit.demo.lua" \
  "duplicate identity error missing explicit descriptor path"

log "same HEAD with a different default namespace invalidates cache"
ln -s "$INDEX_DIR" "$HOME_DIR/data/other"
write_home_config "$HOME_DIR" "GLOBAL" "$INDEX_DIR" "other"
run_xlings "$HOME_DIR" "$ROOT_DIR" search demo >/dev/null 2>&1
grep -q '"default_namespace":"other"' "$INDEX_CACHE" \
  || fail "cache context did not follow the changed default namespace"

log "PASS: same-name namespace identity e2e"
