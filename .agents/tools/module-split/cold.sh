#!/usr/bin/env bash
# Cold full-build timing.  Usage: cold.sh <label> [runs]
#
# "Cold" = the project's own target/ is removed, so every project TU is
# recompiled.  The GLOBAL dependency cache (~/.mcpp/build-cache) stays warm
# on purpose: we are measuring xlings's own sources, not its dependencies.
set -uo pipefail
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LABEL="${1:?usage: cold.sh <label> [runs]}"
RUNS="${2:-1}"
OUT="$WT/build/bench"
cd "$WT" || exit 1
for i in $(seq 1 "$RUNS"); do
  rm -rf target
  s=$(date +%s.%N)
  mcpp build >"$OUT/cold.$LABEL.$i.log" 2>&1
  rc=$?
  e=$(date +%s.%N)
  printf 'COLD %-22s run=%s rc=%s secs=%.2f\n' "$LABEL" "$i" "$rc" "$(echo "$e - $s" | bc)"
  if [ "$rc" -ne 0 ]; then
    echo "--- tail of $OUT/cold.$LABEL.$i.log ---"
    tail -30 "$OUT/cold.$LABEL.$i.log"
    exit "$rc"
  fi
done
