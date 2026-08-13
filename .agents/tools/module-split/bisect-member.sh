#!/usr/bin/env bash
# Binary-search the smallest number of outlined members of ONE file that makes
# the build ICE.  Prints the boundary; member N (1-based) is the trigger.
#
# Usage: bisect_member.sh <file.cppm> <hi>
set -u
FILE="${1:?file}"; HI="${2:?upper bound}"
lo=0            # known good
hi=$HI          # known bad
while [ $((hi - lo)) -gt 1 ]; do
  mid=$(( (lo + hi) / 2 ))
  git checkout b1563fe -- src/
  git clean -fq src/
  python3 .agents/tools/module-split/split.py --all --write >/dev/null
  python3 .agents/tools/module-split/outline.py --write --limit "$mid" "$FILE" >/dev/null
  rm -rf target/x86_64-linux-gnu/*/gcm.cache
  mcpp build >build/bench/bm.log 2>&1
  ice=$(grep -ac 'internal compiler error' build/bench/bm.log)
  err=$(grep -ac ' error: ' build/bench/bm.log)
  if [ "$ice" -gt 0 ]; then
    echo "  limit=$mid  ICE      -> bad"
    hi=$mid
  elif [ "$err" -gt 0 ]; then
    echo "  limit=$mid  errors=$err (not an ICE) -> treating as bad"
    grep -a ' error: ' build/bench/bm.log | head -2
    hi=$mid
  else
    echo "  limit=$mid  clean    -> good"
    lo=$mid
  fi
done
echo "BOUNDARY: $lo good, $hi bad -> member #$hi is the trigger"
