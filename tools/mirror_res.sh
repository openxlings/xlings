#!/usr/bin/env bash
# Mirror a project's release binaries from its upstream GitHub release to the
# xlings-res/<project> resource repo on GitHub AND GitCode, so `XLINGS_RES`
# downloads (esp. the CN path, which is GitCode-only for package binaries)
# resolve for ALL platforms.
#
# Generic over <project> — currently `xlings` and `mcpp`. Tag scheme on
# xlings-res/<project> is the BARE version (e.g. 0.4.63 / 0.0.82); the upstream
# source tag is v<version>. Asset filenames are identical on both ends (the
# xlings-res convention `<project>-<ver>-<platform>.<ext>` matches upstream).
#
# Usage: tools/mirror_res.sh <project> <version>      # e.g. mcpp 0.0.82
# Auth:  XLINGS_RES_TOKEN (github write to xlings-res), GITCODE_TOKEN (+ gtc on PATH)
# Env:   SRC_REPO / GH_DST / GTC_DST / ASSETS (space-separated) override the
#        per-project defaults below.
set -euo pipefail

PROJ="${1:?usage: mirror_res.sh <project> <version>}"
VER="${2:?usage: mirror_res.sh <project> <version>}"

# ── Per-project defaults (source repo + platform asset list) ──────
case "$PROJ" in
  xlings)
    : "${SRC_REPO:=openxlings/xlings}"
    p="xlings-${VER}"
    DEFAULT_ASSETS="${p}-linux-x86_64.tar.gz ${p}-linux-x86_64.tar.gz.sha256 ${p}-linux-aarch64.tar.gz ${p}-linux-aarch64.tar.gz.sha256 ${p}-macosx-arm64.tar.gz ${p}-macosx-arm64.tar.gz.sha256 ${p}-windows-x86_64.zip ${p}-windows-x86_64.zip.sha256"
    ;;
  mcpp)
    : "${SRC_REPO:=mcpp-community/mcpp}"
    # Mirror each archive and its authoritative checksum sidecar so resource
    # index generation can verify every platform before publishing a recipe.
    p="mcpp-${VER}"
    DEFAULT_ASSETS="${p}-linux-x86_64.tar.gz ${p}-linux-x86_64.tar.gz.sha256 ${p}-linux-aarch64.tar.gz ${p}-linux-aarch64.tar.gz.sha256 ${p}-macosx-arm64.tar.gz ${p}-macosx-arm64.tar.gz.sha256 ${p}-windows-x86_64.zip ${p}-windows-x86_64.zip.sha256"
    ;;
  *)
    echo "[mirror] unknown project '$PROJ' (expected xlings|mcpp)" >&2
    exit 2
    ;;
esac
: "${GH_DST:=xlings-res/$PROJ}"
: "${GTC_DST:=xlings-res/$PROJ}"
read -r -a ASSETS <<< "${ASSETS:-$DEFAULT_ASSETS}"

info() { echo "[mirror] $*"; }

DL="$(mktemp -d)"; trap 'rm -rf "$DL"' EXIT

info "downloading $SRC_REPO v$VER assets ($PROJ)"
for a in "${ASSETS[@]}"; do
  gh release download "v$VER" -R "$SRC_REPO" -D "$DL" -p "$a" 2>/dev/null || { echo "[mirror] FAIL: missing $a in $SRC_REPO v$VER" >&2; exit 1; }
done

# ── GitHub (gh --clobber, reliable) ───────────────────────────────
# probe <url> -> "code size", hard wall-clock capped. Every network check in
# this script MUST go through this (or an equivalent bounded call): a stalled
# CDN connection with no --max-time hung the v0.4.65 mirror job for 40+ min
# (3 fast failures, then a timeout-less upload/verify stall).
probe() {
  curl -sL --connect-timeout 10 --max-time 90 -o /dev/null \
       -w '%{http_code} %{size_download}' "$1" 2>/dev/null || echo "ERR 0"
}
asset_size() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1"; }

if [[ -n "${XLINGS_RES_TOKEN:-}" ]] || gh auth status >/dev/null 2>&1; then
  info "GitHub $GH_DST tag $VER"
  GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release view "$VER" -R "$GH_DST" >/dev/null 2>&1 \
    || GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release create "$VER" -R "$GH_DST" --title "$VER" --notes "$PROJ $VER (mirror of $SRC_REPO)"
  # One asset-list call so idempotent re-runs (release re-run after a cancel,
  # manual top-up) skip what is already fully mirrored. Size must match the
  # local file — a partial upload from an interrupted run is re-clobbered.
  existing="$(GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release view "$VER" -R "$GH_DST" \
                --json assets --jq '.assets[] | "\(.name) \(.size)"' 2>/dev/null || true)"
  for a in "${ASSETS[@]}"; do
    if grep -qx "$a $(asset_size "$DL/$a")" <<< "$existing"; then
      info "gh $a already mirrored, skip"
      continue
    fi
    GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release upload "$VER" "$DL/$a" -R "$GH_DST" --clobber
  done
else
  info "no github auth; skipping github mirror"
fi

# ── GitCode (gtc, per-file — multi-file upload can 502 and drop files) ──────
if [[ -n "${GITCODE_TOKEN:-}" ]] && command -v gtc >/dev/null 2>&1; then
  info "GitCode $GTC_DST tag $VER"
  gtc release create "$GTC_DST" --tag "$VER" --name "$VER" 2>/dev/null || true
  for a in "${ASSETS[@]}"; do
    url="https://gitcode.com/${GTC_DST}/releases/download/${VER}/${a}"
    want="$(asset_size "$DL/$a")"
    # Skip if already fully mirrored (same download-verify the retry loop
    # uses), so re-runs only touch the assets that are actually missing.
    read -r code size <<< "$(probe "$url")"
    if [[ "$code" == 200 && "$size" == "$want" ]]; then
      info "gtc $a already mirrored, skip"
      continue
    fi
    # Upload, then verify the actual DOWNLOAD is 200 with the right size (gtc
    # can report success yet leave a phantom/missing asset — obs_callback
    # flakiness). Each attempt is bounded: 180s upload cap + 90s probe cap,
    # so a fully-degraded GitCode fails this asset in <15 min instead of
    # hanging the job (release.yml also has a job-level timeout backstop).
    for try in 1 2 3; do
      timeout -k 10 180 gtc release upload "$GTC_DST" "$DL/$a" --tag "$VER" >/dev/null 2>&1 || true
      read -r code size <<< "$(probe "$url")"
      [[ "$code" == 200 && "$size" == "$want" ]] && break
      echo "[mirror] gtc $a not OK after try $try (code=$code size=$size/want=$want), retrying..."; sleep 5
    done
  done
else
  info "no GITCODE_TOKEN/gtc; skipping gitcode mirror"
fi

# ── Verify every platform on both hosts ───────────────────────────
info "verify:"
rc=0
for host in "github.com/$GH_DST" "gitcode.com/$GTC_DST"; do
  for a in "${ASSETS[@]}"; do
    read -r code _ <<< "$(probe "https://${host}/releases/download/${VER}/${a}")"
    echo "  $code  https://${host}/releases/download/${VER}/${a}"
    [[ "$code" == 200 ]] || rc=1
  done
done
[[ $rc == 0 ]] && info "all platforms mirrored OK" || { echo "[mirror] WARN: some assets not 200" >&2; }
exit $rc
