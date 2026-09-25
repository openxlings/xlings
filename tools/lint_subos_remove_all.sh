#!/usr/bin/env bash
# Lint: a subos directory is deleted in exactly one place.
#
# SubOS homes hold user data that nothing can put back (see AGENTS.md, "SubOS
# user data"). The only function allowed to delete a whole subos is
# subos::userdata::delete_subos, which takes a confirm::UserConfirmed -- a value
# only an explicit user answer produces. A raw `remove_all` on a subos path
# anywhere else is a second, unconfirmed way in, which is exactly how an
# empty name once deleted every subos in a home (#611).
#
# Two rules:
#   1. in the files that manage subos lifecycles, EVERY `remove_all(` needs a
#      `subos-remove-all-ok: <why>` marker on its line;
#   2. anywhere under src/, a `remove_all(` on a line that mentions "subos"
#      needs the same marker.
# A marker is a claim a reviewer can check, one line from the call it covers.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MARK='subos-remove-all-ok:'
bad=0

lifecycle_files=(
  src/core/subos.cpp
  src/core/xself/install.cpp
  src/core/xself/uninstall.cpp
)
while IFS= read -r f; do lifecycle_files+=("$f"); done \
  < <(find src/core/subos -name '*.cpp' | sort)

for f in "${lifecycle_files[@]}"; do
  [ -f "$f" ] || continue
  while IFS= read -r hit; do
    case "$hit" in *"$MARK"*) ;; *)
      echo "::error file=$f::unmarked remove_all in a subos lifecycle file: $hit"
      bad=$((bad + 1)) ;;
    esac
  done < <(grep -n 'remove_all(' "$f" | grep -v '^\s*[0-9]*:\s*//' || true)
done

while IFS= read -r hit; do
  case "$hit" in *"$MARK"*) ;; *)
    echo "::error::remove_all on a subos path outside the deletion entry point: $hit"
    bad=$((bad + 1)) ;;
  esac
done < <(grep -rn --include='*.cpp' --include='*.cppm' 'remove_all(' src \
           | awk '{ code = $0; sub(/^[^:]*:[0-9]+:/, "", code)
                    if (code ~ /^[ \t]*\/\//) next
                    if (tolower(code) ~ /subos/) print }' || true)

if [ "$bad" -gt 0 ]; then
  echo "subos remove_all lint: FAIL ($bad unmarked call(s)). Delete a subos through"
  echo "subos::userdata::delete_subos, or mark the call '$MARK <why>'."
  exit 1
fi
count=$(grep -rn --include='*.cpp' 'remove_all(' "${lifecycle_files[@]}" | grep -c "$MARK" || true)
echo "subos remove_all lint: PASS ($count marked call(s) in lifecycle files)"
