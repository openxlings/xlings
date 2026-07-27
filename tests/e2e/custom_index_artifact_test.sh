#!/usr/bin/env bash
# E2E-30 (#377): user-defined index repos with a declared artifact source.
#   A. source=artifact + dead git URL  -> installs purely from artifact (exact key)
#   B. sole-entry key fallback         -> pointer key != repo name still resolves
#   C. auto + broken artifact          -> falls back to the local-link path
#   D. existing git checkout + artifact-> migrates (.git gone, marker present)
#   E. source=git + artifact declared  -> artifact never fetched
# Hermetic: the official main index is served from a local XLINGS_INDEX_BASE_URL
# dir; the custom repo uses its own local flat base (which also proves the
# global base override does NOT leak into custom sources).
#
# Usage: tests/e2e/custom_index_artifact_test.sh [path-to-xlings-binary]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Binary discovery is shared, not local. This used to be
#   find "$PROJECT_DIR/build" -path '*debug*' -name xlings
# with a SKIP-and-exit-0 when nothing turned up. Two ways that lied:
#   * run_all.sh passes no argument here, and CI builds into target/ (mcpp),
#     not build/ -- so in CI the find matched nothing, the test skipped, and
#     run_all recorded "PASS: E2E-30 (7ms)". The custom-index-artifact
#     feature had no CI coverage at all while appearing to have it.
#   * run standalone on a dev box the find DID match -- a three-week-old
#     debug build -- and the test failed against behaviour that had since
#     changed.
# find_xlings_bin picks the newest build under target/ and warns when its
# version does not match the source. See project_test_lib.sh.
# shellcheck source=./project_test_lib.sh
source "$SCRIPT_DIR/project_test_lib.sh"

XLINGS_BIN="${1:-$(find_xlings_bin)}"
# Not a skip. A test that cannot find the thing it tests has failed to test
# it, and saying so is the whole point.
[[ -x "$XLINGS_BIN" ]] || { echo "[test] FAIL: no xlings binary found" >&2; exit 1; }

pass() { echo "[test] OK: $*"; }
fail() { echo "[test] FAIL: $*" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xim-custom-idx.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ── shared fixtures ──────────────────────────────────────────────
# official main index artifact (hermetic main sync)
MAIN_SRC="$WORK/main-src"; mkdir -p "$MAIN_SRC/pkgs/p"
echo 'package({name="patchelf"})' > "$MAIN_SRC/pkgs/p/patchelf.lua"
MAIN_SERVE="$WORK/main-serve"; mkdir -p "$MAIN_SERVE"
bash "$PROJECT_DIR/tools/build_xim_index_artifact.sh" --version 9.9.9 --out "$MAIN_SERVE" --src "$MAIN_SRC"
python3 - "$MAIN_SERVE"/xim-index-9.9.9.manifest.json "$MAIN_SERVE/xim-index-pointers.json" <<'PY'
import sys, json
m = json.load(open(sys.argv[1]))
json.dump({"format_version": 1, "indexes": {"xim": m}}, open(sys.argv[2], "w"))
PY

# custom index artifact under its own flat local base dir "myindex"
CUSTOM_SRC="$WORK/custom-src"; mkdir -p "$CUSTOM_SRC/pkgs/h"
echo 'package({name="hellopkg"})' > "$CUSTOM_SRC/pkgs/h/hellopkg.lua"
CUSTOM_BASE="$WORK/myindex"; mkdir -p "$CUSTOM_BASE"
bash "$PROJECT_DIR/tools/build_xim_index_artifact.sh" --version 1.2.3 --out "$CUSTOM_BASE" --src "$CUSTOM_SRC"
CUSTOM_MANIFEST="$(ls "$CUSTOM_BASE"/*.manifest.json | head -1)"
make_pointer() {  # $1=key  -> writes myindex-pointers.json
python3 - "$CUSTOM_MANIFEST" "$CUSTOM_BASE/myindex-pointers.json" "$1" <<'PY'
import sys, json
m = json.load(open(sys.argv[1]))
json.dump({"format_version": 1, "indexes": {sys.argv[3]: m}}, open(sys.argv[2], "w"))
PY
}

# local fallback source (a plain dir with pkgs/ -> local-link fallback path)
LOCAL_SRC="$WORK/localsrc"; mkdir -p "$LOCAL_SRC/pkgs/z"
echo 'package({name="zpkg"})' > "$LOCAL_SRC/pkgs/z/zpkg.lua"

fresh_home() {  # $1=scenario-name $2=repo-json-entry -> sets XLINGS_HOME
  export XLINGS_HOME="$WORK/home-$1"
  mkdir -p "$XLINGS_HOME"
  "$XLINGS_BIN" self init >/dev/null 2>&1 || fail "self init failed ($1)"
  cat > "$XLINGS_HOME/.xlings.json" <<EOF
{"index_repos":[${2}]}
EOF
}
export XLINGS_INDEX_BASE_URL="$MAIN_SERVE"   # official main only; custom has its own base

# ── A. artifact-only custom repo, dead git URL, exact key ────────
make_pointer custom1
fresh_home a "{\"name\":\"custom1\",\"url\":\"https://127.0.0.1:1/dead.git\",\"artifact\":\"$CUSTOM_BASE\",\"source\":\"artifact\"}"
"$XLINGS_BIN" update >"$WORK/a.log" 2>&1 || { cat "$WORK/a.log" >&2; fail "A: update failed"; }
D="$XLINGS_HOME/data/custom1"
[[ -f "$D/pkgs/h/hellopkg.lua" ]]   || { cat "$WORK/a.log" >&2; fail "A: custom index not installed from artifact"; }
[[ -f "$D/.xlings-index-version" ]] || fail "A: version marker missing"
[[ ! -d "$D/.git" ]]                || fail "A: unexpected .git (went to git path)"
pass "A: custom repo installed from artifact (source=artifact, dead git URL)"

# ── B. sole-entry key fallback (pointer key != repo name) ────────
make_pointer weirdkey
fresh_home b "{\"name\":\"custom1\",\"url\":\"https://127.0.0.1:1/dead.git\",\"artifact\":\"$CUSTOM_BASE\",\"source\":\"artifact\"}"
"$XLINGS_BIN" update >"$WORK/b.log" 2>&1 || { cat "$WORK/b.log" >&2; fail "B: update failed"; }
[[ -f "$XLINGS_HOME/data/custom1/pkgs/h/hellopkg.lua" ]] || { cat "$WORK/b.log" >&2; fail "B: sole-entry key fallback broken"; }
pass "B: sole-entry pointer key fallback"

# ── C. auto + broken artifact -> local-link fallback ─────────────
fresh_home c "{\"name\":\"custom1\",\"url\":\"$LOCAL_SRC\",\"artifact\":\"$WORK/empty-base/none\",\"source\":\"auto\"}"
"$XLINGS_BIN" update >"$WORK/c.log" 2>&1 || { cat "$WORK/c.log" >&2; fail "C: update failed"; }
[[ -e "$XLINGS_HOME/data/custom1/pkgs/z/zpkg.lua" ]] || { cat "$WORK/c.log" >&2; fail "C: fallback did not link local source"; }
grep -qi "falling back" "$WORK/c.log" || { cat "$WORK/c.log" >&2; fail "C: no fallback warning logged"; }
pass "C: auto mode fell back past a broken artifact base"

# ── D. migration: pre-existing git checkout replaced by artifact ─
make_pointer custom1
fresh_home d "{\"name\":\"custom1\",\"url\":\"https://127.0.0.1:1/dead.git\",\"artifact\":\"$CUSTOM_BASE\",\"source\":\"auto\"}"
D="$XLINGS_HOME/data/custom1"; mkdir -p "$D/.git" "$D/pkgs/old"
echo 'package({name="oldpkg"})' > "$D/pkgs/old/oldpkg.lua"
"$XLINGS_BIN" update >"$WORK/d.log" 2>&1 || { cat "$WORK/d.log" >&2; fail "D: update failed"; }
[[ -f "$D/pkgs/h/hellopkg.lua" ]]   || { cat "$WORK/d.log" >&2; fail "D: artifact content missing after migration"; }
[[ ! -d "$D/.git" ]]                || fail "D: .git survived migration"
[[ -f "$D/.xlings-index-version" ]] || fail "D: marker missing after migration"
pass "D: existing git checkout migrated to artifact"

# ── E. source=git ignores the artifact declaration ───────────────
fresh_home e "{\"name\":\"custom1\",\"url\":\"$LOCAL_SRC\",\"artifact\":\"$CUSTOM_BASE\",\"source\":\"git\"}"
"$XLINGS_BIN" update >"$WORK/e.log" 2>&1 || { cat "$WORK/e.log" >&2; fail "E: update failed"; }
[[ -e "$XLINGS_HOME/data/custom1/pkgs/z/zpkg.lua" ]] || { cat "$WORK/e.log" >&2; fail "E: git/local path not used"; }
[[ ! -f "$XLINGS_HOME/data/custom1/.xlings-index-version" ]] || fail "E: artifact fetched despite source=git"
pass "E: source=git forces the git/local path"

echo "[test] all custom index artifact scenarios passed"
