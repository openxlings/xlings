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

# The client reads the rolling "latest" tag by default (see indexfetch.cppm
# index_tag_()). We also archive each version under v<ver> for reproducibility
# (room for the future X-full/lockfile pinning).
ROLLING_TAG="latest"
ARCHIVE_TAG="v${VERSION}"
ART="$DIR/${BASE}.tar.gz"
MAN="$DIR/${BASE}.manifest.json"
[[ -f "$ART" ]] || { echo "missing artifact: $ART" >&2; exit 1; }
[[ -f "$MAN" ]] || { echo "missing manifest: $MAN" >&2; exit 1; }

# The "latest" pointer is NOT a release asset (gitcode can't overwrite/delete
# those). It is a repo FILE pushed via git (overwriteable) and served raw — see
# tools/push_index_pointers.sh. Here we just emit the pointer JSON into DIR for
# that step to collect. The artifact tarball IS a release asset: its name is
# version-unique (xim-index-<ver>.tar.gz), so it's always a fresh upload.
LATEST_JSON="$DIR/$LATEST"   # xim-index[-name]-latest.json (collected by pointer push)
cp "$MAN" "$LATEST_JSON"

publish_gh() {  # <tag> <file...>
  local tag="$1"; shift
  if [[ "$DRY" == 1 ]]; then
    echo "[dry-run] gh release create/view $tag -R $GH_REPO; gh release upload $tag $* -R $GH_REPO --clobber"
    return
  fi
  gh release view "$tag" -R "$GH_REPO" >/dev/null 2>&1 \
    || gh release create "$tag" -R "$GH_REPO" --title "$tag" --notes "xim package index ($VERSION)"
  gh release upload "$tag" "$@" -R "$GH_REPO" --clobber
}
publish_gtc() {  # <tag> <file...>
  local tag="$1"; shift
  if [[ "$DRY" == 1 ]]; then
    echo "[dry-run] gtc release create $GTC_REPO --tag $tag; gtc release upload $GTC_REPO $* --tag $tag"
    return
  fi
  gtc release create "$GTC_REPO" --tag "$tag" --name "$tag" 2>/dev/null || true
  # Upload ONE file at a time with retry: a multi-file gtc upload can 502
  # mid-way and silently drop the remaining files (obs_callback flakiness),
  # leaving e.g. only the linux asset. Per-file + retry makes it reliable.
  local f try
  for f in "$@"; do
    for try in 1 2 3; do
      if gtc release upload "$GTC_REPO" "$f" --tag "$tag" 2>&1 | tail -1 | grep -q uploaded; then break; fi
      echo "[publish] gtc upload $(basename "$f") try $try failed, retrying..."; sleep 3
    done
  done
}

if [[ "$SKIP_GH" == 0 ]]; then
  info "GitHub $GH_REPO: artifact -> rolling '$ROLLING_TAG' + archive '$ARCHIVE_TAG'"
  # Artifact (version-unique -> fresh) + the legacy release-asset .json pointer
  # for backward compat: pre-0.4.54 clients read xim-index[-sub]-latest.json from
  # the release (gh --clobber can overwrite it). 0.4.54+ clients read the combined
  # repo-file pointer (tools/push_index_pointers.sh). GitCode can't overwrite a
  # release asset, so the legacy pointer is github-only; old CN clients fall to it.
  publish_gh "$ROLLING_TAG" "$ART" "$LATEST_JSON"
  publish_gh "$ARCHIVE_TAG" "$ART" "$MAN"
fi
if [[ "$SKIP_GTC" == 0 ]]; then
  info "GitCode $GTC_REPO: artifact -> rolling '$ROLLING_TAG' + archive '$ARCHIVE_TAG'"
  publish_gtc "$ROLLING_TAG" "$ART"
  publish_gtc "$ARCHIVE_TAG" "$ART"
fi
echo "[publish] pointer JSON emitted: $LATEST_JSON (push via tools/push_index_pointers.sh)"

DESTS=""
[[ "$SKIP_GH"  == 0 ]] && DESTS="$GH_REPO(gh)"
[[ "$SKIP_GTC" == 0 ]] && DESTS="${DESTS:+$DESTS, }$GTC_REPO(gtc)"
info "Published $BASE (+ $LATEST pointer; tags: $ROLLING_TAG, $ARCHIVE_TAG) to: ${DESTS:-<none>}"
