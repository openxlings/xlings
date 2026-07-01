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
    DEFAULT_ASSETS="xlings-${VER}-linux-x86_64.tar.gz xlings-${VER}-linux-aarch64.tar.gz xlings-${VER}-macosx-arm64.tar.gz xlings-${VER}-windows-x86_64.zip"
    ;;
  mcpp)
    : "${SRC_REPO:=mcpp-community/mcpp}"
    # mcpp ships a .sha256 sidecar next to each archive; mirror both so the
    # xlings-res/mcpp release stays byte-for-byte equivalent to upstream.
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
if [[ -n "${XLINGS_RES_TOKEN:-}" ]] || gh auth status >/dev/null 2>&1; then
  info "GitHub $GH_DST tag $VER"
  GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release view "$VER" -R "$GH_DST" >/dev/null 2>&1 \
    || GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release create "$VER" -R "$GH_DST" --title "$VER" --notes "$PROJ $VER (mirror of $SRC_REPO)"
  for a in "${ASSETS[@]}"; do
    GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release upload "$VER" "$DL/$a" -R "$GH_DST" --clobber
  done
else
  info "no github auth; skipping github mirror"
fi

# ── GitCode (gtc, per-file retry — multi-file upload can 502 and drop files) ──
if [[ -n "${GITCODE_TOKEN:-}" ]] && command -v gtc >/dev/null 2>&1; then
  info "GitCode $GTC_DST tag $VER"
  gtc release create "$GTC_DST" --tag "$VER" --name "$VER" 2>/dev/null || true
  # Upload then verify the actual DOWNLOAD is 200 (gtc can report success yet
  # leave a phantom/missing asset — obs_callback flakiness), retry up to 5.
  for a in "${ASSETS[@]}"; do
    for try in 1 2 3 4 5; do
      gtc release upload "$GTC_DST" "$DL/$a" --tag "$VER" >/dev/null 2>&1 || true
      if [[ "$(curl -fsSL -o /dev/null -w '%{http_code}' -L "https://gitcode.com/${GTC_DST}/releases/download/${VER}/${a}" 2>/dev/null)" == 200 ]]; then
        break
      fi
      echo "[mirror] gtc $a not 200 after try $try, retrying..."; sleep 4
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
    code=$(curl -fsSL -o /dev/null -w '%{http_code}' -L "https://${host}/releases/download/${VER}/${a}" 2>/dev/null || echo ERR)
    echo "  $code  https://${host}/releases/download/${VER}/${a}"
    [[ "$code" == 200 ]] || rc=1
  done
done
[[ $rc == 0 ]] && info "all platforms mirrored OK" || { echo "[mirror] WARN: some assets not 200" >&2; }
exit $rc
