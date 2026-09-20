#!/usr/bin/env bash
# E2E: a package that registers a shell script which does not parse must FAIL
# to install — at install time, not at first use.
#
# WHAT THIS DEFENDS (openxlings/xlings#522, #608)
#
# `xim-x-glibc` 2.39 shipped a `bin/ldd` whose `RTLDLIST="` assignment had been
# eaten by a hand-rolled path rewrite. The dangling quote swallowed the next
# nine lines and bash died on line 38. The install reported success, registered
# `ldd`, and put it first on PATH — so the first symptom a user saw was Steam
# announcing that a 32-bit libc was missing.
#
# R4 (assert on the artifact, not on the intent): the rewrite is checked by its
# RESULT, at install time, where it is still cheap to reject.
#
# This is deliberately NOT a duplicate of libxpkg's
# `elfpatch.relocate_build_paths`, which already syntax-checks what it rewrites.
# That assertion protects the recipes that CALL it; 2.39 is the standing proof
# that a recipe can simply not call it. This one is the acceptance gate.
#
# BOTH DIRECTIONS ARE MEASURED. A gate that refuses everything would pass the
# broken row on its own, so the good-script row is not decoration — it is what
# makes the broken row mean something.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

case "$(uname -s)" in
  Linux|Darwin) ;;
  *) log "SKIP: shebang scripts are a POSIX concern (this is $(uname -s))"; exit 0 ;;
esac
command -v bash >/dev/null 2>&1 || { log "SKIP: no bash to check scripts with"; exit 0; }

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/script_assert"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
SCENARIO_FIXTURES="$ROOT_DIR/tests/e2e/fixtures/script_assert"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

mkdir -p "$HOME_DIR/subos/default/bin"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/b" "$LOCAL_INDEX_DIR/pkgs/g"
cp "$SCENARIO_FIXTURES/brokenscript.lua" "$LOCAL_INDEX_DIR/pkgs/b/brokenscript.lua"
cp "$SCENARIO_FIXTURES/goodscript.lua"   "$LOCAL_INDEX_DIR/pkgs/g/goodscript.lua"

cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "${XLINGS_TEST_MIRROR:-GLOBAL}",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
run_xlings "$HOME_DIR" "$ROOT_DIR" self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ── control: a valid script installs normally ────────────────────────
log "installing goodscript (must succeed) ..."
GOOD_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" install goodscript -y 2>&1)" || {
    echo "$GOOD_OUT"
    fail "a package registering a VALID shell script failed to install — the gate refuses everything"
}
[[ -e "$HOME_DIR/subos/default/bin/goodscript" ]] \
    || fail "goodscript installed but registered no shim"
log "  ok: a valid registered script installs and is routed"

# ── the gate: a script that does not parse must not be accepted ───────
log "installing brokenscript (must fail) ..."
set +e
BROKEN_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" install brokenscript -y 2>&1)"
BROKEN_RC=$?
set -e
echo "$BROKEN_OUT"

[[ "$BROKEN_RC" -ne 0 ]] \
    || fail "installing a package whose registered script does not parse exited 0"
log "  ok: exit code is non-zero ($BROKEN_RC)"

assert_contains "$BROKEN_OUT" "does not parse" \
    "the failure must say WHAT is wrong — 'install failed' sends the user nowhere"
log "  ok: the reason names the defect"

assert_contains "$BROKEN_OUT" "brokenscript" \
    "the failure must name the package and the program it registered"
log "  ok: the reason names the package"

log "PASS: install asserts that registered scripts parse"
