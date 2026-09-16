#!/usr/bin/env bash
# E2E-113 (#599): `xlings update` bounds its network work.
#
#   A. git inherits transport bounds  -> GIT_HTTP_LOW_SPEED_* + GIT_TERMINAL_PROMPT
#   B. an operator's own values win   -> xlings never overrides what is already set
#   C. a refresh has a wall-clock budget -> the rest is SKIPPED and SAID, not waited on
#
# Why these three and not a real stall: the measured incident (11 min 27 s on
# 2026-09-16) was a socket that COMPLETED its handshake and then went silent,
# so the existing TCP reachability probe in sync_repo passed and git waited for
# the peer. Nothing hermetic reproduces a silent peer; what is testable, and
# what was missing, is that the bound is handed to git at all and that the
# aggregate is capped.
#
# Hermetic: the official index is served from a local base, and git is a fake
# that records its environment.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=./project_test_lib.sh
source "$SCRIPT_DIR/project_test_lib.sh"

XLINGS_BIN="${1:-$(find_xlings_bin)}"
[[ -x "$XLINGS_BIN" ]] || { echo "[test] FAIL: no xlings binary found" >&2; exit 1; }

pass() { echo "[test] OK: $*"; }
fail() { echo "[test] FAIL: $*" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xim-net-bounds.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ── fake git: records its environment, then serves a clone from nothing ──
mkdir -p "$WORK/bin"
cat > "$WORK/bin/git" <<'GIT_EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "--version" ]; then
  echo "LIMIT=${GIT_HTTP_LOW_SPEED_LIMIT-unset} TIME=${GIT_HTTP_LOW_SPEED_TIME-unset} PROMPT=${GIT_TERMINAL_PROMPT-unset}" >> "$FAKE_GIT_ENV_LOG"
  echo "git version 2.99.0"
  exit 0
fi
if [ "${1:-}" = "clone" ]; then
  dest="${@: -1}"
  echo "clone $dest" >> "$FAKE_GIT_CALL_LOG"
  sleep "${FAKE_GIT_SLEEP:-0}"
  mkdir -p "$dest/.git" "$dest/pkgs/f"
  echo 'package({name="fakepkg"})' > "$dest/pkgs/f/fakepkg.lua"
  exit 0
fi
exit 0
GIT_EOF
chmod +x "$WORK/bin/git"
export XLINGS_COMPACT_GIT_BIN="$WORK/bin/git"
export FAKE_GIT_ENV_LOG="$WORK/git-env.log"
export FAKE_GIT_CALL_LOG="$WORK/git-calls.log"
: > "$FAKE_GIT_ENV_LOG"
: > "$FAKE_GIT_CALL_LOG"

# ── official main index, served locally, declaring two git-only sub-indexes ──
MAIN_SRC="$WORK/main-src"; mkdir -p "$MAIN_SRC/pkgs/p"
echo 'package({name="patchelf"})' > "$MAIN_SRC/pkgs/p/patchelf.lua"
cat > "$MAIN_SRC/xim-indexrepos.lua" <<LUA_EOF
xim_indexrepos = {
    ["g1"] = {
        ["GLOBAL"] = "$WORK/remote-g1",
        ["CN"] = "$WORK/remote-g1",
    },
    ["g2"] = {
        ["GLOBAL"] = "$WORK/remote-g2",
        ["CN"] = "$WORK/remote-g2",
    }
}
LUA_EOF
MAIN_SERVE="$WORK/main-serve"; mkdir -p "$MAIN_SERVE"
bash "$PROJECT_DIR/tools/build_xim_index_artifact.sh" --version 9.9.9 --out "$MAIN_SERVE" --src "$MAIN_SRC" >/dev/null
python3 - "$MAIN_SERVE"/xim-index-9.9.9.manifest.json "$MAIN_SERVE/xim-index-pointers.json" <<'PY_EOF'
import sys, json
m = json.load(open(sys.argv[1]))
json.dump({"format_version": 1, "indexes": {"xim": m}}, open(sys.argv[2], "w"))
PY_EOF
export XLINGS_INDEX_BASE_URL="$MAIN_SERVE"

fresh_home() {  # $1 = scenario name
  export XLINGS_HOME="$WORK/home-$1"
  mkdir -p "$XLINGS_HOME"
  "$XLINGS_BIN" self init >/dev/null 2>&1 || fail "self init failed ($1)"
}

# ── A. git is handed transport bounds ────────────────────────────
fresh_home a
: > "$FAKE_GIT_ENV_LOG"
"$XLINGS_BIN" update >"$WORK/a.log" 2>&1 || { cat "$WORK/a.log" >&2; fail "A: update failed"; }
grep -q "LIMIT=1000" "$FAKE_GIT_ENV_LOG" || { cat "$FAKE_GIT_ENV_LOG" >&2; fail "A: GIT_HTTP_LOW_SPEED_LIMIT not set"; }
grep -q "TIME=60"    "$FAKE_GIT_ENV_LOG" || { cat "$FAKE_GIT_ENV_LOG" >&2; fail "A: GIT_HTTP_LOW_SPEED_TIME not set"; }
grep -q "PROMPT=0"   "$FAKE_GIT_ENV_LOG" || { cat "$FAKE_GIT_ENV_LOG" >&2; fail "A: GIT_TERMINAL_PROMPT not disabled"; }
pass "A: git runs with a transfer bound and no credential prompt"

# ── B. an operator's own values are not overridden ───────────────
fresh_home b
: > "$FAKE_GIT_ENV_LOG"
GIT_HTTP_LOW_SPEED_TIME=7 "$XLINGS_BIN" update >"$WORK/b.log" 2>&1 || { cat "$WORK/b.log" >&2; fail "B: update failed"; }
grep -q "TIME=7" "$FAKE_GIT_ENV_LOG" || { cat "$FAKE_GIT_ENV_LOG" >&2; fail "B: xlings overrode an operator-set bound"; }
pass "B: a value the operator already set is left alone"

# ── C. the refresh has a wall-clock budget ───────────────────────
fresh_home c
# `self init` syncs too, so without this the update below would find every
# sub-index already cloned, do nothing, and pass for the wrong reason -- which
# is how this scenario first "failed": the clone log it read was init's.
rm -rf "$XLINGS_HOME/data/xim-index-repos"
: > "$FAKE_GIT_CALL_LOG"
FAKE_GIT_SLEEP=2 XLINGS_UPDATE_TIMEOUT=1 \
  "$XLINGS_BIN" update >"$WORK/c.log" 2>&1 || { cat "$WORK/c.log" >&2; fail "C: update failed"; }
grep -q "refresh budget reached" "$WORK/c.log" \
  || { cat "$WORK/c.log" >&2; fail "C: the budget was reached and nothing said so"; }
grep -q "XLINGS_UPDATE_TIMEOUT" "$WORK/c.log" \
  || { cat "$WORK/c.log" >&2; fail "C: the message does not say how to wait longer"; }
# One sub-index was cloned; the second was skipped rather than waited on.
[[ "$(wc -l < "$FAKE_GIT_CALL_LOG")" -eq 1 ]] \
  || { cat "$FAKE_GIT_CALL_LOG" >&2; fail "C: the budget did not stop the second source"; }
pass "C: past the budget, the remaining sources are skipped and reported"

echo "[test] all index network bound scenarios passed"
