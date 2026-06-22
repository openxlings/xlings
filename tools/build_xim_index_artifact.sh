#!/usr/bin/env bash
# Build a versioned xim package-index *artifact* + manifest for publishing
# to xlings-res (Y-asset model; see .agents/docs/2026-06-22-index-as-resource-impl-plan.md).
#
# An index artifact is a plain tarball of the index tree (pkgs/, .xpkgindex.json,
# xim-indexrepos.lua, ...) with the .git history stripped, plus a manifest.json
# describing it (sha256/size + format_version + a reserved signature slot for a
# future minisign/X-full upgrade).
#
# Output (in OUT_DIR):
#   xim-index[-<name>]-<ver>.tar.gz
#   xim-index[-<name>]-<ver>.manifest.json
#
# Usage:
#   tools/build_xim_index_artifact.sh --version <ver> --out <dir> [--name <sub>] [--src <dir>]
#
# Env:
#   XLINGS_RELEASE_MIRROR=GLOBAL|CN     pick clone origin when --src omitted (default GLOBAL)
#   XLINGS_RELEASE_PKGINDEX_URL         override clone URL
#   XLINGS_RELEASE_PKGINDEX_REF=main    git ref to snapshot (default main)
set -euo pipefail

VERSION="" OUT_DIR="" NAME="" SRC_DIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --out)     OUT_DIR="$2"; shift 2 ;;
    --name)    NAME="$2";    shift 2 ;;
    --src)     SRC_DIR="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[[ -z "$VERSION" || -z "$OUT_DIR" ]] && { echo "usage: $0 --version <ver> --out <dir> [--name <sub>] [--src <dir>]" >&2; exit 2; }

info() { echo "[index-artifact] $*"; }
fail() { echo "[index-artifact] FAIL: $*" >&2; exit 1; }

# sha256 helper (Linux: sha256sum, macOS: shasum -a 256)
sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}';
  else fail "no sha256 tool (sha256sum/shasum)"; fi
}

MIRROR="${XLINGS_RELEASE_MIRROR:-GLOBAL}"
REF="${XLINGS_RELEASE_PKGINDEX_REF:-main}"
# Sub-index name → repo. Main index has no name.
if [[ -z "$NAME" ]]; then
  case "$MIRROR" in
    CN) DEFAULT_URL="https://gitee.com/sunrisepeak/xim-pkgindex.git" ;;
    *)  DEFAULT_URL="https://github.com/openxlings/xim-pkgindex.git" ;;
  esac
  ARTIFACT_BASE="xim-index-${VERSION}"
else
  DEFAULT_URL="https://github.com/d2learn/xim-pkgindex-${NAME}.git"
  ARTIFACT_BASE="xim-index-${NAME}-${VERSION}"
fi
URL="${XLINGS_RELEASE_PKGINDEX_URL:-$DEFAULT_URL}"

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/xim-index-build.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

TREE="$TMP_ROOT/tree"
if [[ -n "$SRC_DIR" ]]; then
  info "Using local source: $SRC_DIR"
  [[ -d "$SRC_DIR/pkgs" ]] || fail "source dir missing pkgs/: $SRC_DIR"
  cp -a "$SRC_DIR" "$TREE"
else
  info "Cloning $URL ($REF)"
  if ! git clone --depth 1 --branch "$REF" "$URL" "$TREE" 2>/dev/null; then
    rm -rf "$TREE"
    git clone "$URL" "$TREE"
    git -C "$TREE" checkout --quiet "$REF"
  fi
fi

# Record source commit (before stripping .git) for traceability.
SOURCE_COMMIT="unknown"
if [[ -d "$TREE/.git" ]]; then
  SOURCE_COMMIT="$(git -C "$TREE" rev-parse HEAD 2>/dev/null || echo unknown)"
fi
rm -rf "$TREE/.git"
[[ -d "$TREE/pkgs" ]] || fail "index tree missing pkgs/ after fetch"

# Deterministic-ish tarball (sorted names, stable owners).
ARTIFACT="$OUT_DIR/${ARTIFACT_BASE}.tar.gz"
info "Packing $ARTIFACT"
tar --sort=name --owner=0 --group=0 --numeric-owner \
    -czf "$ARTIFACT" -C "$TREE" . 2>/dev/null \
  || tar -czf "$ARTIFACT" -C "$TREE" .   # BSD tar fallback (no --sort)

SHA="$(sha256_of "$ARTIFACT")"
SIZE="$(wc -c < "$ARTIFACT" | tr -d ' ')"
GENERATED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

MANIFEST="$OUT_DIR/${ARTIFACT_BASE}.manifest.json"
cat > "$MANIFEST" <<JSON
{
  "format_version": 1,
  "index_version": "${VERSION}",
  "index_name": "${NAME:-xim}",
  "generated_at": "${GENERATED_AT}",
  "source_commit": "${SOURCE_COMMIT}",
  "artifact": {
    "name": "${ARTIFACT_BASE}.tar.gz",
    "sha256": "${SHA}",
    "size": ${SIZE}
  },
  "signature": null
}
JSON

info "Done:"
info "  artifact: $ARTIFACT"
info "  sha256:   $SHA  ($SIZE bytes)"
info "  manifest: $MANIFEST"
