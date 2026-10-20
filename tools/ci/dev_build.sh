#!/usr/bin/env bash
# The development build the Linux test lanes run on, one profile:
#
#   dev_build.sh dev      xlings, luban, the Luban OS app, xdesk and xdev
#   dev_build.sh asan     xlings instrumented (mcpp.toml [profile.asan]), and xdev
#   dev_build.sh release  the release candidate (tools/linux_release.sh, from the
#                         state it starts with) and a static xdev for the lanes
#                         that run it (target/xdev/runner/bin/xdev)
#
# In CI it names what it built in $GITHUB_ENV (LUBAN_OS_BIN, XDESK_BIN, XDEV).
set -euo pipefail
profile="${1:?usage: dev_build.sh dev|asan|release}"
if [[ "$profile" == release ]]; then
  # Built static (musl) so every job can run it: a glibc dev build's loader
  # lives in the building runner's mcpp registry. linux_release.sh rebuilds
  # the musl target, so it is kept outside that tree.
  mcpp build -p xdev --target x86_64-linux-musl || mcpp build -p xdev --target x86_64-linux-musl
  xdev="$(find target apps/xdev -path '*musl*' -name xdev -type f -perm -111 2>/dev/null | xargs -r ls -t | head -1)"
  [[ -n "$xdev" ]] || { echo "::error::mcpp build -p xdev produced no xdev"; exit 1; }
  mkdir -p target/xdev/runner/bin
  cp "$xdev" target/xdev/runner/bin/xdev
  SKIP_NETWORK_VERIFY=1 XLINGS_RELEASE_REUSE=1 bash tools/linux_release.sh
  exit 0
fi
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
  # (luban/tests/e2e/test_luban*.cpp; the app's headless tests under SDL's offscreen
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
