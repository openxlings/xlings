#!/usr/bin/env bash
# E2E test: `xlings subos new <name> --from <spec>` fork semantics.
#
# Validates:
#   1. Local fork: `subos new fork --from <local-subos>` copies content
#   2. Modifying fork doesn't affect base (independent inodes/COW)
#   3. pkg-spec fork: `subos new x --from subos:py-demo@1.0.0` auto-
#      installs base (if missing) then forks
#   4. Forked subos has .xlings.json workspace inherited
#   5. --from with missing pkg-spec falls through to auto-install
#
# Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M2, E1-E5)
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_xpkg_fork"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
SCENARIO_FIXTURES="$ROOT_DIR/tests/e2e/fixtures/subos_xpkg"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

mkdir -p "$HOME_DIR/subos/default/bin"

# Private pkgindex copy, with subos fixture injected
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/p"
cp "$SCENARIO_FIXTURES/py-demo.lua" "$LOCAL_INDEX_DIR/pkgs/p/py-demo.lua"

cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "subos": { "default": { "dir": "" } },
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
run_xlings "$HOME_DIR" "$ROOT_DIR" self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ─── Test 1: Local fork ───────────────────────────────────────────────
log "Test 1: local fork"
run_xlings "$HOME_DIR" "$ROOT_DIR" subos new base-env --storage shared >/dev/null 2>&1 \
    || fail "subos new base-env failed"
echo "hello from base" > "$HOME_DIR/subos/base-env/marker.txt"

run_xlings "$HOME_DIR" "$ROOT_DIR" subos new fork-env --from base-env >/dev/null 2>&1 \
    || fail "subos new fork-env --from base-env failed"

[[ -f "$HOME_DIR/subos/fork-env/marker.txt" ]] \
    || fail "fork-env: marker.txt not inherited from base"
[[ "$(cat "$HOME_DIR/subos/fork-env/marker.txt")" == "hello from base" ]] \
    || fail "fork-env: marker content mismatch"
log "  ok: local fork inherited content"

# Modifying fork must not affect base
echo "modified" > "$HOME_DIR/subos/fork-env/marker.txt"
[[ "$(cat "$HOME_DIR/subos/base-env/marker.txt")" == "hello from base" ]] \
    || fail "fork modification leaked back to base (no isolation)"
log "  ok: fork is independent (modifications don't leak to base)"

# ─── Test 2: pkg-spec fork with auto-install ─────────────────────────
log "Test 2: pkg-spec fork (auto-install)"
# subos:py-demo@1.0.0 is NOT pre-installed; --from should auto-install
[[ ! -d "$HOME_DIR/data/xpkgs/subos-x-py-demo/1.0.0" ]] \
    || fail "py-demo base xpkg unexpectedly present before fork (test setup error)"

run_xlings "$HOME_DIR" "$ROOT_DIR" subos new from-pkg --from subos:py-demo@1.0.0 2>&1 \
    > "$RUNTIME_DIR/fork-out.txt" || {
        cat "$RUNTIME_DIR/fork-out.txt"
        fail "subos new from-pkg --from subos:py-demo@1.0.0 failed"
    }

# Base must now be installed
[[ -d "$HOME_DIR/data/xpkgs/subos-x-py-demo/1.0.0" ]] \
    || fail "base xpkg not auto-installed by --from"
log "  ok: base pkg auto-installed when --from'd from spec"

# Forked subos must exist
[[ -d "$HOME_DIR/subos/from-pkg" ]] \
    || fail "from-pkg subos not created"

# Forked subos must have .xlings.json
[[ -f "$HOME_DIR/subos/from-pkg/.xlings.json" ]] \
    || fail "from-pkg: .xlings.json missing"
log "  ok: forked subos has .xlings.json"

# ─── Test 3: Equals-style --from=<spec> ──────────────────────────────
log "Test 3: --from=<value> equals-style"
run_xlings "$HOME_DIR" "$ROOT_DIR" subos new from-pkg2 --from=subos:py-demo@1.0.0 >/dev/null 2>&1 \
    || fail "--from=<spec> equals-style failed"
[[ -d "$HOME_DIR/subos/from-pkg2" ]] || fail "from-pkg2 not created"
log "  ok: --from=<spec> equals form works"

# ─── Test 4: --from <nonexistent local> errors cleanly ───────────────
log "Test 4: --from <nonexistent> errors cleanly"
set +e
ERR_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" subos new bad --from doesnotexist 2>&1)"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || fail "fork from nonexistent should fail"
grep -q "not found" <<< "$ERR_OUT" \
    || fail "error msg for missing source should say 'not found'"
log "  ok: missing source rejected with clear error"

log "PASS: subos new --from works end-to-end (M2)"
