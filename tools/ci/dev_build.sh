#!/usr/bin/env bash
# The development build the Linux test lanes run on, one profile:
#
#   dev_build.sh dev    xlings, luban, the Luban OS app, xdesk and xdev
#   dev_build.sh asan   xlings instrumented (mcpp.toml [profile.asan]), and xdev
#
# In CI it names what it built in $GITHUB_ENV (LUBAN_OS_BIN, XDESK_BIN, XDEV).
set -euo pipefail
profile="${1:?usage: dev_build.sh dev|asan}"
flags=()
[[ "$profile" == dev ]] || flags=(--profile "$profile")
# `mcpp build` can transiently fail with "dependency '...': index entry not
# found in local clone" when the cached package index is stale: mcpp refreshes
# the index ON that failure, so a second attempt succeeds.
mcpp build "${flags[@]}" || {
  echo "::warning::mcpp build failed (likely stale index entry); retrying after auto-refresh"
  mcpp build "${flags[@]}"
}
if [[ "$profile" == dev ]]; then
  # luban, the Luban OS app and xdesk beside the xlings the tests run
  # (tests/e2e/test_luban*.cpp; the app's headless tests under SDL's offscreen
  # driver; xdesk as luban-os's desktop).
  mcpp build -p luban-tool
  mcpp build -p luban-os
  mcpp build -p xdesk-app
fi
mcpp build -p xdev
find_bin() { find "$PWD/target" -path "*bin/$1" -type f -perm -111 | head -1; }
xdev="$(find_bin xdev/xdev)"
test -x "$xdev"
env_out="${GITHUB_ENV:-/dev/null}"
echo "XDEV=$xdev" >> "$env_out"
if [[ "$profile" == dev ]]; then
  luban_os="$(find_bin luban-os/luban-os)"
  xdesk="$(find_bin xdesk-app/xdesk)"
  test -x "$luban_os" && test -x "$xdesk"
  { echo "LUBAN_OS_BIN=$luban_os"; echo "XDESK_BIN=$xdesk"; } >> "$env_out"
  "$xdesk" --probe
fi
