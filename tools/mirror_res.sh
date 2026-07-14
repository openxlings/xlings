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

# probe <url> -> "code size", hard wall-clock capped. Every network check in
# this script MUST go through this (or an equivalent bounded call): a stalled
# CDN connection with no --max-time hung the v0.4.65 mirror job for 40+ min
# (3 fast failures, then a timeout-less upload/verify stall).
probe() {
  local out
  out="$(curl -sL --connect-timeout 10 --max-time 90 -o /dev/null \
           -w '%{http_code} %{size_download}' "$1" 2>/dev/null)" \
    && echo "$out" || echo "ERR 0"
}
asset_size() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1"; }

# Expected asset sizes from the SOURCE release — ONE API call, no body
# download. Lets the skip-if-already-mirrored checks below run without first
# pulling ~48 MB every time (steady-state re-runs, e.g. a daily CN top-up
# cron, then move no bytes). Empty entries just fall back to a real download.
declare -A WANT=()
while IFS=$'\t' read -r _n _s; do [[ -n "$_n" ]] && WANT["$_n"]="$_s"; done < <(
  GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release view "v$VER" -R "$SRC_REPO" \
    --json assets --jq '.assets[] | "\(.name)\t\(.size)"' 2>/dev/null || true)

# Download a single source asset into $DL on demand (only when an upload
# actually needs the bytes). Retries because CN->GitHub can be slow/flaky
# (~100 KB/s; a 9 MB asset ~100 s) and a single give-up must not abort the run.
ensure_local() {
  local a="$1" try
  [[ -f "$DL/$a" ]] && return 0
  for try in 1 2 3; do
    if gh release download "v$VER" -R "$SRC_REPO" -D "$DL" -p "$a" --clobber 2>/dev/null; then
      return 0
    fi
    echo "[mirror] download $a attempt $try failed, retrying..." >&2; sleep 5
  done
  echo "[mirror] FAIL: cannot download $a from $SRC_REPO v$VER after 3 tries" >&2
  return 1
}

gh_failed=()
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
    want="${WANT[$a]:-}"
    if [[ -n "$want" ]] && grep -qx "$a $want" <<< "$existing"; then
      info "gh $a already mirrored, skip"
      continue
    fi
    ensure_local "$a" || { gh_failed+=("$a"); continue; }
    # Re-check with the true local size (covers the empty-WANT fallback).
    if grep -qx "$a $(asset_size "$DL/$a")" <<< "$existing"; then
      info "gh $a already mirrored, skip"
      continue
    fi
    GH_TOKEN="${XLINGS_RES_TOKEN:-}" gh release upload "$VER" "$DL/$a" -R "$GH_DST" --clobber
  done
else
  info "no github auth; skipping github mirror"
fi
if [[ ${#gh_failed[@]} -gt 0 ]]; then
  echo "[mirror] github incomplete (${#gh_failed[@]} asset(s)): ${gh_failed[*]}" >&2
fi

# ── GitCode (gtc, per-file — multi-file upload can 502 and drop files) ──────
if [[ -n "${GITCODE_TOKEN:-}" ]] && command -v gtc >/dev/null 2>&1; then
  info "GitCode $GTC_DST tag $VER"
  gtc release create "$GTC_DST" --tag "$VER" --name "$VER" 2>/dev/null || true
  # One public-API call: which asset NAMES are registered on the release.
  # Distinguishes "missing" (never uploaded — upload can fix it) from
  # "registered but not downloadable" (broken/phantom attachment — a
  # same-name upload CANNOT replace it; it must be deleted in the GitCode
  # release UI first, so report and move on instead of retrying).
  registered="$(curl -s --connect-timeout 10 --max-time 30 \
      "https://api.gitcode.com/api/v5/repos/${GTC_DST}/releases/tags/${VER}" \
    | python3 -c 'import json,sys
try:
    d = json.load(sys.stdin)
    print("\n".join(a["name"] for a in d.get("assets", []) if a.get("type") == "attach"))
except Exception:
    pass' || true)"

  gtc_failed=()
  for a in "${ASSETS[@]}"; do
    url="https://gitcode.com/${GTC_DST}/releases/download/${VER}/${a}"
    want="${WANT[$a]:-}"
    # Skip if already fully mirrored (using the source size when known), so
    # re-runs only touch actual gaps — no download for assets already present.
    read -r code size <<< "$(probe "$url")"
    if [[ -n "$want" && "$code" == 200 && "$size" == "$want" ]]; then
      info "gtc $a already mirrored, skip"
      continue
    fi
    if grep -qxF "$a" <<< "$registered"; then
      echo "[mirror] WARN: gtc $a registered on the release but not downloadable (code=$code size=$size/want=$want) — broken attachment; delete it in the GitCode release UI, then re-run mirror-binaries.yml" >&2
      gtc_failed+=("$a")
      continue
    fi
    # Need the bytes to (re)upload — fetch on demand, then settle the true
    # size (covers the empty-WANT fallback and re-checks the skip condition).
    ensure_local "$a" || { gtc_failed+=("$a"); continue; }
    want="$(asset_size "$DL/$a")"
    if [[ "$code" == 200 && "$size" == "$want" ]]; then
      info "gtc $a already mirrored, skip"
      continue
    fi
    # Upload, then verify the actual DOWNLOAD is 200 with the right size
    # (gtc can report success yet register nothing — obs_callback
    # flakiness). Budget per asset: at most ONE retry, and only for FAST
    # failures. A healthy upload finishes in seconds; an attempt killed by
    # the 90s cap (rc=124) means GitCode's ingest is stalling on this file
    # (observed 2026-07-14: every asset >8 MiB stalled while <8 MiB ones
    # sailed through, across two separate runs) — that is systemic, a
    # retry just burns the budget. Whole gitcode section stays <5 min.
    ok=""
    for try in 1 2; do
      rc=0
      timeout -k 10 90 gtc release upload "$GTC_DST" "$DL/$a" --tag "$VER" >/dev/null 2>&1 || rc=$?
      read -r code size <<< "$(probe "$url")"
      if [[ "$code" == 200 && "$size" == "$want" ]]; then ok=1; break; fi
      if [[ "$rc" == 124 || "$rc" == 137 ]]; then
        echo "[mirror] gtc $a upload STALLED (killed at 90s) — systemic on GitCode's side, not retrying" >&2
        break
      fi
      [[ "$try" == 1 ]] && echo "[mirror] gtc $a not OK (rc=$rc code=$code size=$size/want=$want), one retry..."
    done
    [[ -n "$ok" ]] || gtc_failed+=("$a")
  done
  if [[ ${#gtc_failed[@]} -gt 0 ]]; then
    echo "[mirror] gitcode incomplete (${#gtc_failed[@]} asset(s)): ${gtc_failed[*]}" >&2
  fi
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
