#!/usr/bin/env bash
# Mirror the xlings binaries from an openxlings/xlings release to the
# xlings-res/xlings resource repo on GitHub and GitCode, so `XLINGS_RES`
# downloads (esp. the CN path, which is GitCode-only for package binaries)
# resolve for ALL platforms.
#
# Tag scheme on xlings-res/xlings is the BARE version (e.g. 0.4.54), with assets
# xlings-<ver>-{linux-x86_64.tar.gz, macosx-arm64.tar.gz, windows-x86_64.zip}.
#
# Usage: tools/mirror_xlings_res.sh <version>          # e.g. 0.4.54
# Auth:  XLINGS_RES_TOKEN (github write to xlings-res), GITCODE_TOKEN (+ gtc on PATH)
# Env:   SRC_REPO (default openxlings/xlings), GH_DST/GTC_DST (default xlings-res/xlings)
set -euo pipefail

VER="${1:?usage: mirror_xlings_res.sh <version>}"
SRC_REPO="${SRC_REPO:-openxlings/xlings}"
GH_DST="${GH_DST:-xlings-res/xlings}"
GTC_DST="${GTC_DST:-xlings-res/xlings}"

info() { echo "[mirror] $*"; }

DL="$(mktemp -d)"; trap 'rm -rf "$DL"' EXIT
ASSETS=( "xlings-${VER}-linux-x86_64.tar.gz" "xlings-${VER}-linux-aarch64.tar.gz" "xlings-${VER}-macosx-arm64.tar.gz" "xlings-${VER}-windows-x86_64.zip" )

info "downloading $SRC_REPO v$VER assets"
for a in "${ASSETS[@]}"; do
  gh release download "v$VER" -R "$SRC_REPO" -D "$DL" -p "$a" 2>/dev/null || { echo "[mirror] FAIL: missing $a in $SRC_REPO v$VER" >&2; exit 1; }
done

# ── GitHub (gh --clobber, reliable) ───────────────────────────────
if [[ -n "${XLINGS_RES_TOKEN:-}" ]] || gh auth status >/dev/null 2>&1; then
  info "GitHub $GH_DST tag $VER"
  GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release view "$VER" -R "$GH_DST" >/dev/null 2>&1 \
    || GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release create "$VER" -R "$GH_DST" --title "$VER" --notes "xlings $VER (mirror of $SRC_REPO)"
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
