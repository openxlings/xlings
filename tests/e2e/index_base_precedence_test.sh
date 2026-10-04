#!/usr/bin/env bash
# E2E: a project's `xim.index-base` wins over the home's.
#
# Both layers are region chains (#598) read into Config. They load at
# different times -- the project's with the manifest, the home's lazily on
# first use -- and while they shared one member, the later (home) load
# overwrote the project's, inverting "project overrides global" (PR #639
# second review, F3). Observable end to end: the index artifact is fetched
# from whichever base wins.
#
# Hermetic: the home's base is a directory that does not exist, the
# project's is a local artifact dir. XLINGS_INDEX_SOURCE=artifact forbids a
# git fallback, so `update` succeeds only through the project's base.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=./project_test_lib.sh
source "$SCRIPT_DIR/project_test_lib.sh"

XLINGS_BIN="${1:-$(find_xlings_bin)}"
[[ -x "$XLINGS_BIN" ]] || { echo "[test] FAIL: no xlings binary found" >&2; exit 1; }
# Absolute: the scenarios below run from other directories.
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"

pass() { echo "[test] OK: $*"; }
fail() { echo "[test] FAIL: $*" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xim-index-base-prec.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ── 1. A local index artifact the project's base will name ─────────
SRC="$WORK/src"; mkdir -p "$SRC/pkgs/p"
echo 'package({name="patchelf"})' > "$SRC/pkgs/p/patchelf.lua"
echo '{"site":{"title":"t"}}'      > "$SRC/.xpkgindex.json"
SERVE="$WORK/serve"; mkdir -p "$SERVE"
bash "$PROJECT_DIR/tools/build_xim_index_artifact.sh" --version 9.9.9 --out "$SERVE" --src "$SRC"
python3 - "$SERVE/xim-index-9.9.9.manifest.json" "$SERVE/xim-index-pointers.json" <<'PY'
import sys, json
m = json.load(open(sys.argv[1]))
json.dump({"format_version": 1, "indexes": {"xim": m}}, open(sys.argv[2], "w"))
PY

# ── 2. Home: its own index-base points nowhere ──────────────────────
export XLINGS_HOME="$WORK/home"
mkdir -p "$XLINGS_HOME"
"$XLINGS_BIN" self init >/dev/null 2>&1 || fail "self init failed"
python3 - "$XLINGS_HOME/.xlings.json" "$WORK/no-such-base" <<'PY'
import json, sys
p, base = sys.argv[1], sys.argv[2]
d = json.load(open(p))
d["mirror"] = "GLOBAL"
d.setdefault("xim", {})["index-base"] = base
json.dump(d, open(p, "w"), indent=2, sort_keys=True)
PY
unset XLINGS_INDEX_BASE_URL
export XLINGS_INDEX_SOURCE=artifact
export XLINGS_NO_AUTO_INSTALL_GIT=1

# ── 3. Control: outside the project the home's (dead) base is used ──
OUTSIDE="$WORK/outside"; mkdir -p "$OUTSIDE"
if (cd "$OUTSIDE" && "$XLINGS_BIN" update >"$WORK/outside.log" 2>&1); then
  cat "$WORK/outside.log" >&2
  fail "control: update succeeded through a home index-base that does not exist -- the test would prove nothing"
fi
# And it failed for the right reason -- the dead base -- not for any other.
grep -q "no-such-base" "$WORK/outside.log" \
  || { cat "$WORK/outside.log" >&2; fail "control: update failed, but not on the home's index-base"; }
pass "control: the home's index-base is honored outside the project"

# ── 4. In the project, the project's base wins ──────────────────────
PROJ="$WORK/proj"; mkdir -p "$PROJ"
python3 - "$PROJ/.xlings.json" "$SERVE" <<'PY'
import json, sys
json.dump({"xim": {"index-base": sys.argv[2]}}, open(sys.argv[1], "w"), indent=2)
PY
if ! (cd "$PROJ" && env -u XLINGS_PROJECT_DIR "$XLINGS_BIN" update >"$WORK/proj.log" 2>&1); then
  cat "$WORK/proj.log" >&2
  fail "update in the project did not use the project's index-base (the home's overrode it)"
fi
pass "project: the project's index-base overrides the home's"

echo "[test] ALL PASSED"
