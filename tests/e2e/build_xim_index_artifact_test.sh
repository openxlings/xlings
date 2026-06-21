#!/usr/bin/env bash
# E2E: tools/build_xim_index_artifact.sh produces a tar.gz + manifest whose
# recorded sha256 matches the artifact and whose tarball contains pkgs/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$PROJECT_DIR/tools/build_xim_index_artifact.sh"

pass() { echo "[test] OK: $*"; }
fail() { echo "[test] FAIL: $*" >&2; exit 1; }

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xim-artifact-test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# Fixture index tree (no network; use --src).
SRC="$WORK/src"
mkdir -p "$SRC/pkgs/p"
echo 'package({name="patchelf"})' > "$SRC/pkgs/p/patchelf.lua"
echo '{"site":{"title":"t"}}'      > "$SRC/.xpkgindex.json"
mkdir -p "$SRC/.git"; echo "junk" > "$SRC/.git/HEAD"   # must be stripped

OUT="$WORK/out"
bash "$BUILD" --version 9.9.9 --out "$OUT" --src "$SRC"

ART="$OUT/xim-index-9.9.9.tar.gz"
MAN="$OUT/xim-index-9.9.9.manifest.json"
[[ -f "$ART" ]] || fail "artifact not produced"
[[ -f "$MAN" ]] || fail "manifest not produced"
pass "artifact + manifest produced"

# Manifest sha256 must equal the real artifact sha256.
MAN_SHA="$(grep -o '"sha256": *"[0-9a-f]*"' "$MAN" | grep -o '[0-9a-f]\{64\}')"
REAL_SHA="$(sha256_of "$ART")"
[[ "$MAN_SHA" == "$REAL_SHA" ]] || fail "manifest sha256 ($MAN_SHA) != artifact sha256 ($REAL_SHA)"
pass "manifest sha256 matches artifact"

# format_version + reserved signature slot present.
grep -q '"format_version": 1' "$MAN" || fail "manifest missing format_version=1"
grep -q '"signature": null'   "$MAN" || fail "manifest missing reserved signature slot"
pass "manifest has format_version + reserved signature"

# Tarball contains pkgs/ and NOT .git
LIST="$(tar -tzf "$ART")"
echo "$LIST" | grep -q 'pkgs/p/patchelf.lua' || fail "tarball missing pkgs/p/patchelf.lua"
echo "$LIST" | grep -q '\.git/' && fail "tarball still contains .git/"
pass "tarball contains pkgs/, .git stripped"

echo "[test] ALL PASSED"
