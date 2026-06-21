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
cp "$SERVE/xim-index-9.9.9.manifest.json" "$SERVE/xim-index-latest.json"   # pointer
pass "fixture artifact + pointer built"

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

echo "[test] ALL PASSED"
