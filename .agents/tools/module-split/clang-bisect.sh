#!/usr/bin/env bash
# Binary-search the smallest number of outlined members of ONE file that makes
# the clang build fail.  Member N (1-based, in outline order) is the trigger.
#
#   clang_bisect.sh <file.cppm> <hi>
set -u
FILE="${1:?file}"; HI="${2:?upper bound}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
cd "$ROOT" || exit 1
lo=0
hi=$HI
while [ $((hi - lo)) -gt 1 ]; do
  mid=$(( (lo + hi) / 2 ))
  out=$(LIMIT=$mid bash "$HERE/clang-variant.sh" "b$mid" "$FILE" 2>&1 | tail -1)
  if echo "$out" | grep -q 'rc=0 errors=0'; then
    echo "  limit=$mid  clean -> good"
    lo=$mid
  else
    echo "  limit=$mid  FAIL  -> bad   ${out#*errors=}"
    hi=$mid
  fi
done
echo "BOUNDARY: $lo good, $hi bad -> member #$hi is the trigger"
