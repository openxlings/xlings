#!/usr/bin/env bash
# Lint: platform selection is `if constexpr`, and headers stay where they are
# used (AGENTS.md, "Platform code lives in xlings.platform").
#
# Two rules:
#
#   1. Outside modules/platform, a preprocessor branch on the platform or the
#      compiler (_WIN32, __linux__, __APPLE__, __unix__, _MSC_VER, __MINGW*)
#      needs `platform-if-ok: <why>` on its line. The default is
#      `if constexpr (platform::is_windows)`: every build then compiles every
#      branch, so the Linux CI catches a typo in the Windows one. What cannot
#      be a constexpr branch -- a declaration that differs per compiler, a
#      module below xlings.platform -- says so in one line.
#
#   2. Inside modules/platform, an interface unit (.cppm) includes no system
#      header: only the C++-wrapped C library (<cstdio> and friends) that a
#      declaration needs. The system headers belong to the implementation
#      unit that makes the call, so they are parsed once, by one unit, and a
#      partition's interface reads the same on every platform.
#
# modules/testkit and tests/ are outside the product (see
# lint_platform_headers.sh); modules/libs/src/json is vendored.
set -euo pipefail
cd "$(dirname "$0")/.."

MARK='platform-if-ok:'
bad=0

branch='^[[:space:]]*#[[:space:]]*(if|elif|ifdef|ifndef)\b.*(_WIN32|__linux__|__APPLE__|__unix__|_MSC_VER|__MINGW)'
while IFS= read -r hit; do
  case "$hit" in *"$MARK"*) ;; *)
    echo "::error::platform #if outside modules/platform (use if constexpr, or mark '$MARK <why>'): $hit"
    bad=$((bad + 1)) ;;
  esac
done < <(grep -rnE --include='*.cpp' --include='*.cppm' "$branch" src modules apps 2>/dev/null \
           | grep -v '^modules/platform/' | grep -v '^modules/testkit/' \
           | grep -v '^modules/libs/src/json/' || true)

while IFS= read -r hit; do
  echo "::error::system header in a platform interface unit (move it to the .cpp that calls it): $hit"
  bad=$((bad + 1))
done < <(grep -rnE --include='*.cppm' '^[[:space:]]*#[[:space:]]*include[[:space:]]*<' modules/platform/src 2>/dev/null \
           | grep -vE '#[[:space:]]*include[[:space:]]*<c[a-z]+>' || true)

if [ "$bad" -gt 0 ]; then
  echo "platform branch lint: FAIL ($bad finding(s))"
  exit 1
fi
marked=$(grep -rn --include='*.cpp' --include='*.cppm' "$MARK" src modules apps 2>/dev/null | wc -l)
echo "platform branch lint: PASS ($marked marked #if outside modules/platform)"
