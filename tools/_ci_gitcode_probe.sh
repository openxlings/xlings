#!/usr/bin/env bash
# THROWAWAY diagnostic — do NOT merge. Determines whether a GitHub-hosted
# runner can upload a large (>8 MiB) release asset to GitCode at all, and if
# so via which method. Background: v0.4.65's mirror job stalled on every asset
# >8 MiB (killed at 90s) while the SAME files uploaded in ~10s from a CN
# developer machine — pointing at cross-border runner->OBS throttling rather
# than a GitCode fault. This probes several upload strategies against the real
# 27 MiB asset and prints a comparison table.
#
# Each method uploads under a DISTINCT asset name so results don't collide.
# A failed presigned PUT can leave a phantom (registered-but-empty) asset;
# the caller's cleanup step best-effort deletes the whole probe release.
#
# Usage: tools/_ci_gitcode_probe.sh <path-to-large-file>
set -uo pipefail   # NOT -e: every method must run so we get a full table

SRC="${1:?usage: _ci_gitcode_probe.sh <large-file>}"
export REPO="${GTC_DST:-xlings-res/xlings}"
export TAG="${PROBE_TAG:-ci-upload-probe}"
export API="${GITCODE_API:-https://api.gitcode.com/api/v5}"
: "${GITCODE_TOKEN:?need GITCODE_TOKEN}"

SIZE="$(stat -c%s "$SRC" 2>/dev/null || stat -f%z "$SRC")"
echo "== gitcode upload probe =="
echo "repo=$REPO tag=$TAG src=$SRC size=$SIZE bytes ($((SIZE/1024/1024)) MiB)"
echo

# Ensure the throwaway release exists (idempotent; ignore 'already exists').
curl -s -o /dev/null -X POST "$API/repos/$REPO/releases" \
  -H "PRIVATE-TOKEN: $GITCODE_TOKEN" -H 'Content-Type: application/json' \
  -d "{\"tag_name\":\"$TAG\",\"name\":\"$TAG\",\"body\":\"throwaway CI upload network probe\",\"target_commitish\":\"main\"}" \
  --connect-timeout 10 --max-time 30 || true

# Fetch a fresh presigned OBS PUT URL + signing headers for a given asset name,
# exactly the way tools/gtc does (GET .../releases/{tag}/upload_url).
# Emits: line1 = PUT url; subsequent lines = "Header<TAB>Value".
get_put() {
  ASSET_NAME="$1" python3 - <<'PY'
import json, os, sys, urllib.request, urllib.parse
api   = os.environ["API"]; repo = os.environ["REPO"]; tag = os.environ["TAG"]
token = os.environ["GITCODE_TOKEN"]; fname = os.environ["ASSET_NAME"]
url = f"{api}/repos/{repo}/releases/{tag}/upload_url?file_name={urllib.parse.quote(fname)}"
req = urllib.request.Request(url, headers={"PRIVATE-TOKEN": token, "Accept": "application/json"})
try:
    info = json.loads(urllib.request.urlopen(req, timeout=60).read())
except Exception as e:
    sys.stderr.write(f"upload_url error: {e}\n"); sys.exit(3)
print(info["url"])
for k, v in (info.get("headers") or {}).items():
    print(f"{k}\t{v}")
PY
}

# Download-verify with a short retry (obs registration can lag a few seconds).
verify() {
  local name="$1" code size
  for _ in 1 2 3; do
    read -r code size < <(curl -sL --connect-timeout 10 --max-time 120 -o /dev/null \
        -w '%{http_code} %{size_download}' \
        "https://gitcode.com/$REPO/releases/download/$TAG/$name" 2>/dev/null)
    [[ "$code" == 200 && "$size" == "$SIZE" ]] && { echo "OK $size"; return; }
    sleep 5
  done
  echo "MISS ${code:-ERR}/${size:-0}"
}

# curl-based PUT to the presigned URL. Extra curl opts passed after the name.
# curl's own --max-time lets it exit AND still print -w (so we see how many
# bytes made it before a stall — the key diagnostic vs an external `timeout`).
probe_curl() {
  local label="$1" name="$2"; shift 2
  local meta; meta="$(get_put "$name")" || { printf '  %-16s upload_url FAILED\n' "$label"; return; }
  local put_url; put_url="$(head -1 <<<"$meta")"
  local hargs=(); local k v
  while IFS=$'\t' read -r k v; do [[ -n "$k" ]] && hargs+=(-H "$k: $v"); done < <(tail -n +2 <<<"$meta")
  local t0=$SECONDS w rc
  w="$(curl -s -o /dev/null -X PUT "$put_url" "${hargs[@]}" --data-binary @"$SRC" \
       -w 'http=%{http_code} up=%{size_upload} speed=%{speed_upload}B/s t=%{time_total}s' \
       "$@" 2>/dev/null)"; rc=$?
  local dt=$((SECONDS - t0))
  printf '  %-16s rc=%-3s %s wall=%ss verify=%s\n' "$label" "$rc" "$w" "$dt" "$(verify "$name")"
}

# Baseline: the exact path that failed — tools/gtc (Python urllib PUT),
# wrapped in an external timeout since urllib gives no partial-byte readout.
probe_gtc() {
  local name="probe-a-gtc.bin"
  cp "$SRC" "/tmp/$name"
  local t0=$SECONDS rc
  timeout -k 10 120 gtc release upload "$REPO" --tag "$TAG" "/tmp/$name" >/dev/null 2>&1; rc=$?
  local dt=$((SECONDS - t0))
  local note=""; [[ $rc == 124 || $rc == 137 ]] && note=" (killed at cap)"
  printf '  %-16s rc=%-3s%s wall=%ss verify=%s\n' "gtc-baseline" "$rc" "$note" "$dt" "$(verify "$name")"
}

echo "method            result"
echo "----------------- ------------------------------------------------------------"
probe_gtc
probe_curl "curl-default"    "probe-b-curl.bin"      --max-time 200
probe_curl "curl-http1.1"    "probe-c-http11.bin"    --max-time 200 --http1.1
probe_curl "curl-rate-300k"  "probe-d-rate300k.bin"  --max-time 280 --limit-rate 300k
probe_curl "curl-rate-1m"    "probe-e-rate1m.bin"    --max-time 200 --limit-rate 1M
echo
echo "want size = $SIZE bytes; verify=OK means the asset is downloadable at that size."
