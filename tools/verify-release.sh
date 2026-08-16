#!/usr/bin/env bash
# Post-release verification for xlings <version>.
#
# Every release job in this repo can go green having changed nothing, so each
# check below reads the ARTIFACT rather than the job status:
#
#   * a job status is a claim about work; the release is the work
#   * `gh release view` (GraphQL) and `gh api .../releases` (REST) have
#     disagreed for minutes at a time -- REST is the one to trust
#   * a gitcode asset answers 401 to HEAD and 206 to a ranged GET even when
#     truncated, so presence is proved by downloading and hashing
#   * `bump-index` opens a PR and merges nothing, so `latest` in the index is
#     read directly, on main, in ALL THREE platform tables
#
# Usage: verify_release.sh 2026.8.17.1
set -uo pipefail
V="${1:?usage: verify_release.sh <version>}"
FAIL=0
note() { printf '\n=== %s ===\n' "$*"; }
bad()  { printf '  ✗ %s\n' "$*"; FAIL=1; }
ok()   { printf '  ✓ %s\n' "$*"; }

note "1. GitHub release assets (expect 8: 4 archives + 4 sha256 sidecars)"
ASSETS=$(gh api "repos/openxlings/xlings/releases" --jq \
  ".[] | select(.tag_name==\"v$V\") | .assets[].name" 2>/dev/null | sort)
N=$(printf '%s\n' "$ASSETS" | grep -c . )
printf '%s\n' "$ASSETS" | sed 's/^/    /'
[ "$N" -eq 8 ] && ok "8 assets" || bad "expected 8 assets, got $N"

note "2. sha256 computed here, vs the published sidecar"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for a in $(printf '%s\n' "$ASSETS" | grep -v '\.sha256$'); do
  url="https://github.com/openxlings/xlings/releases/download/v$V/$a"
  curl -fsSL "$url" -o "$TMP/$a" || { bad "download failed: $a"; continue; }
  mine=$(sha256sum "$TMP/$a" | cut -d' ' -f1)
  theirs=$(curl -fsSL "$url.sha256" 2>/dev/null | tr -d ' \n' | cut -d' ' -f1)
  theirs=${theirs%%[!0-9a-f]*}
  if [ "$mine" = "$theirs" ]; then ok "$a  $mine"
  else bad "$a  computed=$mine sidecar=$theirs"; fi
done

note "3. index 'latest' on xim-pkgindex main, all three platforms"
REC=$(gh api "repos/openxlings/xim-pkgindex/contents/pkgs/x/xlings.lua?ref=main" \
        --jq '.content' 2>/dev/null | base64 -d 2>/dev/null)
if [ -z "$REC" ]; then bad "could not read pkgs/x/xlings.lua"; else
  for plat in linux macosx windows; do
    line=$(printf '%s' "$REC" | awk -v p="$plat" '
      $0 ~ "^[[:space:]]*"p"[[:space:]]*=" {inb=1}
      inb && /latest/ {print; exit}')
    if printf '%s' "$line" | grep -q "$V"; then ok "$plat latest -> $V"
    else bad "$plat latest does not name $V: ${line:-<not found>}"; fi
  done
fi
OPEN=$(gh pr list -R openxlings/xim-pkgindex --state open --json number,title \
        --jq '.[] | "#\(.number) \(.title)"' 2>/dev/null)
[ -n "$OPEN" ] && printf '  open index PRs (bump-index merges nothing):\n%s\n' \
  "$(printf '%s\n' "$OPEN" | sed 's/^/    /')"

note "4. CN mirror — full download and hash, not HEAD"
for a in $(printf '%s\n' "$ASSETS" | grep -v '\.sha256$'); do
  url="https://gitcode.com/xlings-res/xlings/releases/download/$V/$a"
  if curl -fsSL "$url" -o "$TMP/cn-$a" 2>/dev/null; then
    cn=$(sha256sum "$TMP/cn-$a" | cut -d' ' -f1)
    gh_=$(sha256sum "$TMP/$a" 2>/dev/null | cut -d' ' -f1)
    if [ "$cn" = "$gh_" ]; then ok "CN $a matches GitHub byte for byte"
    else bad "CN $a differs (cn=$cn github=$gh_)"; fi
  else bad "CN $a not downloadable"; fi
done

note "RESULT"
[ "$FAIL" -eq 0 ] && echo "  all checks passed" || echo "  SOME CHECKS FAILED"
exit "$FAIL"
