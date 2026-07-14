#!/usr/bin/env bash
# MANUAL POST-RELEASE STEP — top up the GitCode binary mirror for the latest
# release. Resolve the latest published <project> release and (re)mirror its
# binaries to xlings-res via mirror_res.sh. Idempotent: already-mirrored assets
# are skipped, so a run with nothing missing is a cheap no-op (~40 s).
#
# WHY THIS IS MANUAL (not in CI): GitHub-hosted runners CANNOT upload large
# (>~8 MiB) assets to GitCode — the cross-border runner->Huawei-Cloud-OBS path
# sustains only ~15-30 KB/s and every upload method times out (proven in
# openxlings/xlings probe PR #371; runbook:
# .agents/docs/2026-07-15-gitcode-large-asset-mirror-runbook.md). So the
# release/mirror CI is fail-fast on big assets, and a maintainer finishes the
# CN mirror by running THIS from any environment with a healthy GitCode route
# (a CN shell/box/cloud terminal — NOT tied to any one machine). It uploads
# only the assets CI could not.
#
# Not automated on a schedule on purpose: correctness does not depend on it —
# CN clients already fall back to the GitHub asset URLs (see
# build_xlings_res_fallback_urls_ in src/core/xim/installer.cppm); the GitCode
# copy is only a CN-acceleration convenience, needed for ~2-4 big files per
# release. Standing infra (CN cron/VM/self-hosted runner) is an option if
# releases ever get frequent enough to make the manual step tiresome.
#
# Token handling: GITCODE_TOKEN is loaded from ~/.config/gitcode-tool/config.json
# when unset. The GitHub mirror side uses XLINGS_RES_TOKEN or `gh auth` and is
# skipped (not failed) when neither is present — GitHub assets are already
# published by release CI; this run only fixes the GitCode side.
#
# Usage: tools/mirror-latest.sh [project]     # project: xlings (default) | mcpp
set -euo pipefail

PROJ="${1:-xlings}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "$PROJ" in
  xlings) SRC_REPO="openxlings/xlings" ;;
  mcpp)   SRC_REPO="mcpp-community/mcpp" ;;
  *) echo "[mirror-latest] unknown project '$PROJ' (expected xlings|mcpp)" >&2; exit 2 ;;
esac

# GitCode token from the standard gtc config when not already in the env.
if [[ -z "${GITCODE_TOKEN:-}" ]]; then
  cfg="${HOME}/.config/gitcode-tool/config.json"
  if [[ -f "$cfg" ]]; then
    GITCODE_TOKEN="$(python3 -c "import json;print(json.load(open('$cfg')).get('token',''))" 2>/dev/null || true)"
    export GITCODE_TOKEN
  fi
fi
if [[ -z "${GITCODE_TOKEN:-}" ]]; then
  echo "[mirror-latest] WARN: no GITCODE_TOKEN (env or $HOME/.config/gitcode-tool/config.json) — GitCode side will be skipped" >&2
fi

# Latest published release tag -> bare version. Prefer gh; fall back to the
# unauthenticated GitHub API so a locked-keyring timer still resolves it.
tag="$(gh release view -R "$SRC_REPO" --json tagName --jq .tagName 2>/dev/null || true)"
if [[ -z "$tag" ]]; then
  tag="$(curl -s --connect-timeout 10 --max-time 30 \
           "https://api.github.com/repos/$SRC_REPO/releases/latest" \
         | python3 -c "import json,sys;print(json.load(sys.stdin).get('tag_name',''))" 2>/dev/null || true)"
fi
[[ -n "$tag" ]] || { echo "[mirror-latest] ERROR: could not resolve latest $SRC_REPO release" >&2; exit 1; }
ver="${tag#v}"
echo "[mirror-latest] $PROJ latest release = $tag -> version $ver"

export PATH="$here:$PATH"   # ensure the vendored gtc is found
exec "$here/mirror_res.sh" "$PROJ" "$ver"
