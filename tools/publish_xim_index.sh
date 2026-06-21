#!/usr/bin/env bash
# Publish a built xim-index artifact + manifest to xlings-res/xim-index on both
# GitHub (gh) and GitCode (gtc). Idempotent: creates the release if missing,
# then uploads with clobber. Also refreshes a stable "latest" pointer asset
# (<base>.manifest.json copied to xim-index[-<name>]-latest.json).
#
# Usage:
#   tools/publish_xim_index.sh --version <ver> --dir <artifact-dir> [--name <sub>] \
#       [--github-repo xlings-res/xim-index] [--gitcode-repo xlings-res/xim-index] \
#       [--dry-run] [--skip-github] [--skip-gitcode]
#
# Auth: gh uses its own login; gtc uses GITCODE_TOKEN env or `gtc --token`.
set -euo pipefail

VERSION="" DIR="" NAME="" DRY=0 SKIP_GH=0 SKIP_GTC=0
GH_REPO="xlings-res/xim-index" GTC_REPO="xlings-res/xim-index"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)     VERSION="$2"; shift 2 ;;
    --dir)         DIR="$2"; shift 2 ;;
    --name)        NAME="$2"; shift 2 ;;
    --github-repo) GH_REPO="$2"; shift 2 ;;
    --gitcode-repo)GTC_REPO="$2"; shift 2 ;;
    --dry-run)     DRY=1; shift ;;
    --skip-github) SKIP_GH=1; shift ;;
    --skip-gitcode)SKIP_GTC=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[[ -z "$VERSION" || -z "$DIR" ]] && { echo "usage: $0 --version <ver> --dir <artifact-dir> [--name <sub>] [--dry-run]" >&2; exit 2; }

info() { echo "[publish] $*"; }
run()  { if [[ "$DRY" == 1 ]]; then echo "[dry-run] $*"; else "$@"; fi; }

if [[ -z "$NAME" ]]; then BASE="xim-index-${VERSION}"; LATEST="xim-index-latest.json";
else BASE="xim-index-${NAME}-${VERSION}"; LATEST="xim-index-${NAME}-latest.json"; fi

TAG="v${VERSION}"
ART="$DIR/${BASE}.tar.gz"
MAN="$DIR/${BASE}.manifest.json"
[[ -f "$ART" ]] || { echo "missing artifact: $ART" >&2; exit 1; }
[[ -f "$MAN" ]] || { echo "missing manifest: $MAN" >&2; exit 1; }

# Stable latest pointer (copy of this manifest).
LATEST_PATH="$DIR/$LATEST"
cp "$MAN" "$LATEST_PATH"

# ── GitHub via gh ────────────────────────────────────────────────
if [[ "$SKIP_GH" == 0 ]]; then
  info "GitHub: $GH_REPO  tag=$TAG"
  if [[ "$DRY" == 1 ]]; then
    echo "[dry-run] gh release view $TAG -R $GH_REPO || gh release create $TAG -R $GH_REPO"
    echo "[dry-run] gh release upload $TAG $ART $MAN $LATEST_PATH -R $GH_REPO --clobber"
  else
    gh release view "$TAG" -R "$GH_REPO" >/dev/null 2>&1 \
      || gh release create "$TAG" -R "$GH_REPO" --title "$VERSION" --notes "xim package index artifact $VERSION"
    gh release upload "$TAG" "$ART" "$MAN" "$LATEST_PATH" -R "$GH_REPO" --clobber
  fi
fi

# ── GitCode via gtc ──────────────────────────────────────────────
if [[ "$SKIP_GTC" == 0 ]]; then
  info "GitCode: $GTC_REPO  tag=$TAG"
  if [[ "$DRY" == 1 ]]; then
    echo "[dry-run] gtc release create $GTC_REPO --tag $TAG --name $VERSION"
    echo "[dry-run] gtc release upload $GTC_REPO $ART $MAN $LATEST_PATH --tag $TAG"
  else
    gtc release create "$GTC_REPO" --tag "$TAG" --name "$VERSION" 2>/dev/null || true
    gtc release upload "$GTC_REPO" "$ART" "$MAN" "$LATEST_PATH" --tag "$TAG"
  fi
fi

DESTS=""
[[ "$SKIP_GH"  == 0 ]] && DESTS="$GH_REPO(gh)"
[[ "$SKIP_GTC" == 0 ]] && DESTS="${DESTS:+$DESTS, }$GTC_REPO(gtc)"
info "Published $BASE (+ $LATEST pointer) to: ${DESTS:-<none>}"
