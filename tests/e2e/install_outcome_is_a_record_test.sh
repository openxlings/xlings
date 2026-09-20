#!/usr/bin/env bash
# E2E: what `install` SAYS is read off what it DID.
#
# WHAT THIS DEFENDS (openxlings/xlings#606, #376)
#
# "Did this package get installed" had six answerers in cmd_install — a
# plan-level expected<>, two counters, the Failed events, an already-installed
# table, a post-hoc state probe — and the branch that prints
#
#     X@V installed, but 'X' still resolves to Y — `xlings use X V` to switch
#
# asked none of them. So a download that failed printed `installed` and handed
# the user a `xlings use` with nothing to switch to.
#
# The installer now keeps one per-node outcome record and every statement reads
# it. Both directions are measured here, because a notice that never prints
# would pass the failure row on its own — and that notice is load-bearing: it
# is the only thing that tells a user their `install X@V` did not change which
# version runs.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/install_outcome"
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
mkdir -p "$LOCAL_INDEX_DIR/pkgs/t"
cp "$SCENARIO_FIXTURES/twover.lua" "$LOCAL_INDEX_DIR/pkgs/t/twover.lua"

cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "${XLINGS_TEST_MIRROR:-GLOBAL}",
  "index_repos": [ { "name": "xim", "url": "$LOCAL_INDEX_DIR" } ]
}
EOF
run_xlings "$HOME_DIR" "$ROOT_DIR" self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

NOTICE='installed, but'

# ── 1. install 1.0 so something is active ────────────────────────────
run_xlings "$HOME_DIR" "$ROOT_DIR" install twover@1.0 -y >/dev/null 2>&1 \
  || fail "installing twover@1.0 failed"
GOT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" list twover 2>&1 | strip_ansi)"
case "$GOT" in *1.0*) ;; *) fail "twover@1.0 is not recorded: $GOT" ;; esac
log "  ok: twover 1.0 installed and active"

# ── 2. a FAILED install must not claim it installed anything ─────────
set +e
OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" install twover@2.0 -y 2>&1 | strip_ansi)"
RC=$?
set -e
echo "$OUT"

[ "$RC" -ne 0 ] || fail "a failed install exited 0"
log "  ok: exit code is non-zero"

case "$OUT" in
  *"$NOTICE"*) fail "a failed install printed the 'installed, but ...' notice:
$OUT" ;;
esac
log "  ok: no 'installed, but ...' notice for an install that did not happen"

# And the remedy it used to print must not be there either — a command a user
# copies and runs has to be runnable.
case "$OUT" in
  *"xlings use twover 2.0"*) fail "a failed install offered a remedy that cannot work:
$OUT" ;;
esac
log "  ok: no unrunnable remedy"

# ── 3. the notice still fires when it is TRUE ────────────────────────
#
# Without this row the fix could be "delete the notice" and everything above
# would still pass — while a user who installs a version that does not become
# active is told nothing at all, which is the silent failure the notice was
# added for in the first place (`install llvm@20.1.7` printed success while
# clang++ stayed on 22.1.8).
#
# 3.0 installs successfully while 1.0 is active, so the notice is TRUE here.
OUT2="$(run_xlings "$HOME_DIR" "$ROOT_DIR" install twover@3.0 -y 2>&1 | strip_ansi)"
echo "$OUT2"
case "$OUT2" in
  *"$NOTICE"*) log "  ok: the notice fires when the requested version really is installed and not active" ;;
  *) fail "a SUCCESSFUL install of a non-active version printed no notice — the user
cannot tell that 'twover' still runs 1.0:
$OUT2" ;;
esac

# And the remedy it prints has to be runnable, since that is what a user copies.
case "$OUT2" in
  *"xlings use twover 3.0"*) ;;
  *) fail "the notice did not offer the switch command: $OUT2" ;;
esac
run_xlings "$HOME_DIR" "$ROOT_DIR" use twover 3.0 >/dev/null 2>&1 \
  || fail "the remedy the notice printed does not work"
log "  ok: the remedy it prints actually switches"

# ── 4. and it stays quiet for the version that IS active ─────────────
OUT3="$(run_xlings "$HOME_DIR" "$ROOT_DIR" install twover@3.0 -y 2>&1 | strip_ansi)"
case "$OUT3" in
  *"$NOTICE"*) fail "the notice fired for the version that IS active:
$OUT3" ;;
esac
log "  ok: no notice when the requested version is the active one"

log "PASS: install says what it did"
