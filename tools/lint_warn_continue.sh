#!/usr/bin/env bash
# Lint: in the install, remove and switch flows, a warn is not a result.
#
# The result contract (design 2026-11-27 C1): a step ends Done, Skipped,
# Refused or Failed, and the command's exit code is computed from those. A
# check that found a problem and then only warned -- `log::warn(...)` followed
# by `continue`, or as the last statement of its branch, falling through as if
# nothing happened -- is how a refused activation was followed by a written
# declaration, and an unreadable recipe by an install with no download (§2.4).
#
# Rule: in the flow files below, a `log::warn` that ends its branch (the next
# code line is `continue;`, `break;` or the closing `}`) carries
# `// warn-ok: <why it does not change the conclusion>` on the warn or the two
# lines above it. Anything else becomes an outcome or a finding.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

files=(
  src/core/xim/installer.cpp
  src/core/xim/commands.cpp
  src/core/xvm/commands.cpp
  src/core/xvm/removal.cpp
)

out="$(awk -v MARK='warn-ok:' '
  function flush_candidate() {
    if (pending && !marked) {
      printf "%s:%d: a warn ends its branch without %s <why>\n", FILENAME, wline, MARK
      bad++
    }
    pending = 0
  }
  FNR == 1 { inwarn = 0; pending = 0; h1 = ""; h2 = "" }
  {
    line = $0
    code = line; sub(/^[ \t]+/, "", code)
    if (inwarn) {
      if (index(line, MARK)) marked = 1
      depth += gsub(/\(/, "(", line) - gsub(/\)/, ")", line)
      if (depth <= 0 && line ~ /;[ \t]*(\/\/.*)?$/) { inwarn = 0; pending = 1 }
      h2 = h1; h1 = $0; next
    }
    if (pending) {
      if (code == "" || code ~ /^\/\//) { h2 = h1; h1 = $0; next }
      if (code ~ /^(continue;|break;|\}|\} else)/) flush_candidate(); else pending = 0
    }
    if (code !~ /^\/\// && index(line, "log::warn(")) {
      wline = FNR; marked = (index(line, MARK) || index(h1, MARK) || index(h2, MARK)) ? 1 : 0
      depth = gsub(/\(/, "(", line) - gsub(/\)/, ")", line)
      if (depth <= 0 && line ~ /;[ \t]*(\/\/.*)?$/) pending = 1; else inwarn = 1
    }
    h2 = h1; h1 = $0
  }
  END { exit bad > 0 }
' "${files[@]}")" || {
  echo "$out" | sed 's/^/::error::/'
  echo "warn lint: FAIL. Make it an outcome (Refused/Failed) or a finding, or mark it"
  echo "'// warn-ok: <why it does not change the conclusion>'."
  exit 1
}
count=$(grep -c 'warn-ok:' "${files[@]}" | awk -F: '{ s += $2 } END { print s }')
echo "warn lint: PASS ($count marked warn(s) in the install, remove and switch flows)"
