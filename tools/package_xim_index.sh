#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-}"
[[ -z "$OUT_DIR" ]] && { echo "usage: $0 <release-package-dir>" >&2; exit 1; }

MIRROR="${XLINGS_RELEASE_MIRROR:-GLOBAL}"
REF="${XLINGS_RELEASE_PKGINDEX_REF:-main}"

case "$MIRROR" in
  GLOBAL) URL="${XLINGS_RELEASE_PKGINDEX_URL:-https://github.com/openxlings/xim-pkgindex.git}" ;;
  CN)     URL="${XLINGS_RELEASE_PKGINDEX_URL:-https://gitee.com/sunrisepeak/xim-pkgindex.git}" ;;
  *)      URL="${XLINGS_RELEASE_PKGINDEX_URL:-https://github.com/openxlings/xim-pkgindex.git}" ;;
esac

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/xlings-pkgindex.XXXXXX")"
cleanup() { rm -rf "$TMP_ROOT"; }
trap cleanup EXIT

echo "[release] Bundling xim-pkgindex snapshot: $URL ($REF)"
if ! git clone --depth 1 --branch "$REF" "$URL" "$TMP_ROOT/xim-pkgindex"; then
  rm -rf "$TMP_ROOT/xim-pkgindex"
  git clone "$URL" "$TMP_ROOT/xim-pkgindex"
  git -C "$TMP_ROOT/xim-pkgindex" checkout --quiet "$REF"
fi

git -C "$TMP_ROOT/xim-pkgindex" remote set-url origin "$URL"

rm -rf "$OUT_DIR/data/xim-pkgindex"
mkdir -p "$OUT_DIR/data"
mv "$TMP_ROOT/xim-pkgindex" "$OUT_DIR/data/xim-pkgindex"

test -d "$OUT_DIR/data/xim-pkgindex/pkgs" || {
  echo "[release] FAIL: bundled xim-pkgindex missing pkgs/" >&2
  exit 1
}
