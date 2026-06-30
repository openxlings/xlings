#!/usr/bin/env bash
# E2E: `xlings update` (no target) installs the index from a resource artifact
# (XLINGS_INDEX_BASE_URL local-dir base = the hermetic test + offline/bundled
# path), verifies sha256, atomic-installs into data/xim-pkgindex; and the tamper
# path (sha256 reject) under XLINGS_INDEX_SOURCE=artifact (no git fallback).
#
# Usage: tests/e2e/index_artifact_update_test.sh [path-to-xlings-binary]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

XLINGS_BIN="${1:-}"
if [[ -z "$XLINGS_BIN" ]]; then
  XLINGS_BIN="$(find "$PROJECT_DIR/build" -path '*debug*' -name xlings -type f -perm -111 2>/dev/null | head -1)"
fi
[[ -x "$XLINGS_BIN" ]] || { echo "[test] SKIP: no xlings binary (build first)"; exit 0; }

pass() { echo "[test] OK: $*"; }
fail() { echo "[test] FAIL: $*" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xim-update-test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ── 1. Build a fixture index artifact + manifest + latest pointer ──
SRC="$WORK/src"; mkdir -p "$SRC/pkgs/p"
echo 'package({name="patchelf"})' > "$SRC/pkgs/p/patchelf.lua"
echo '{"site":{"title":"t"}}'      > "$SRC/.xpkgindex.json"

SERVE="$WORK/serve"; mkdir -p "$SERVE"
bash "$PROJECT_DIR/tools/build_xim_index_artifact.sh" --version 9.9.9 --out "$SERVE" --src "$SRC"
# Combined pointer file the client fetches: {"format_version":1,"indexes":{"xim":<manifest>}}
python3 - "$SERVE/xim-index-9.9.9.manifest.json" "$SERVE/xim-index-pointers.json" <<'PY'
import sys, json
m = json.load(open(sys.argv[1]))
json.dump({"format_version": 1, "indexes": {"xim": m}}, open(sys.argv[2], "w"))
PY
pass "fixture artifact + combined pointer built"

# ── 2. Isolated home + self init ──────────────────────────────────
export XLINGS_HOME="$WORK/home"
mkdir -p "$XLINGS_HOME"
"$XLINGS_BIN" self init >/dev/null 2>&1 || fail "self init failed"

# ── 3. Happy path: xlings update (no target) via local artifact base ─
export XLINGS_INDEX_BASE_URL="$SERVE"   # local dir base (copied directly)
export XLINGS_INDEX_SOURCE=artifact     # force artifact, no git fallback
if ! "$XLINGS_BIN" update >"$WORK/update.log" 2>&1; then
  cat "$WORK/update.log" >&2; fail "xlings update (artifact) failed"
fi
INDEX_DIR="$XLINGS_HOME/data/xim-pkgindex"
[[ -f "$INDEX_DIR/pkgs/p/patchelf.lua" ]] || { cat "$WORK/update.log" >&2; fail "index not installed from artifact"; }
[[ -f "$INDEX_DIR/.xlings-index-version" ]] || fail "index version marker missing"
pass "happy path: index installed from artifact"

# ── 4. Tamper path: corrupt artifact, sha256 must reject ─────────
printf 'CORRUPT' >> "$SERVE/xim-index-9.9.9.tar.gz"
if "$XLINGS_BIN" update >"$WORK/update2.log" 2>&1; then
  fail "xlings update accepted a tampered artifact (sha256 not enforced)"
fi
grep -qi "sha256\|artifact failed\|mismatch" "$WORK/update2.log" \
  || { cat "$WORK/update2.log" >&2; fail "tamper failure not reported as integrity error"; }
[[ -f "$INDEX_DIR/pkgs/p/patchelf.lua" ]] || fail "tamper destroyed the good index"
pass "tamper path: sha256 rejected, good index preserved"

# Repair the artifact for the self-heal scenarios below.
bash "$PROJECT_DIR/tools/build_xim_index_artifact.sh" --version 9.9.9 --out "$SERVE" --src "$SRC"
python3 - "$SERVE/xim-index-9.9.9.manifest.json" "$SERVE/xim-index-pointers.json" <<'PY'
import sys, json
m = json.load(open(sys.argv[1]))
json.dump({"format_version": 1, "indexes": {"xim": m}}, open(sys.argv[2], "w"))
PY

# ── 5. Self-heal: a stranded git main (pkgs/, NO marker, fake .git) must
#       migrate back to artifact on the next AUTO update (P0-1 symmetry fix) ──
rm -f "$INDEX_DIR/.xlings-index-version"
mkdir -p "$INDEX_DIR/.git"; echo "ref: refs/heads/main" > "$INDEX_DIR/.git/HEAD"
unset XLINGS_INDEX_SOURCE   # auto: official remote must converge to artifact
if ! "$XLINGS_BIN" update >"$WORK/update3.log" 2>&1; then
  cat "$WORK/update3.log" >&2; fail "auto self-heal update failed"
fi
[[ -f "$INDEX_DIR/.xlings-index-version" ]] || { cat "$WORK/update3.log" >&2; fail "stranded git main did not self-heal to artifact"; }
[[ -d "$INDEX_DIR/.git" ]] && fail "artifact swap left a .git behind (not artifact-managed)"
pass "self-heal: stranded git main migrated back to artifact in auto mode"

# ── 6. Non-destructive fallback: with the marker gone again and artifact
#       UNREACHABLE in auto mode, the existing index must be preserved, never
#       wiped by a git-clone fallback (P0-2). ──
rm -f "$INDEX_DIR/.xlings-index-version"   # orphan state: pkgs/, no .git, no marker
export XLINGS_INDEX_BASE_URL="$WORK/nonexistent-base"   # artifact fetch will fail
export XLINGS_NO_AUTO_INSTALL_GIT=1
"$XLINGS_BIN" update >"$WORK/update4.log" 2>&1 || true   # may report failure; must not destroy
[[ -f "$INDEX_DIR/pkgs/p/patchelf.lua" ]] \
  || { cat "$WORK/update4.log" >&2; fail "non-destructive guard failed: index wiped by git fallback"; }
pass "non-destructive: unreachable artifact in auto mode preserved the index"

echo "[test] ALL PASSED"
