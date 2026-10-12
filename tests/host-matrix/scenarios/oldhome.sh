#!/usr/bin/env bash
# A home an older client filled, moved to the build under test: an
# environment made there has no failed node (the plan migrates or rebuilds
# what the older client left), and a home of the environment's own -- what
# mcpp makes for its toolchains -- initialises inside it.
#
# The older client is the upgrade's starting point, pinned on purpose (as
# fresh-install pins MCPP_OLD): it is not the version under test.
source "$(dirname "$0")/../lib.sh"
require_isolation
command -v curl >/dev/null || skip "no curl to fetch the older client"
OLD="${HM_OLD_XLINGS:-2026.10.9.2}"
old="$HOME/hm-oldhome"
rm -rf "$old" "$HOME/hm-old-client"; mkdir -p "$HOME/hm-old-client"
url="https://github.com/openxlings/xlings/releases/download/v$OLD/xlings-$OLD-linux-$(uname -m).tar.gz"
step "the older client ($OLD)" ok -- curl -fsSL "$url" -o "$HOME/hm-old-client/x.tar.gz"
tar -xzf "$HOME/hm-old-client/x.tar.gz" -C "$HOME/hm-old-client"
oldbin="$(find "$HOME/hm-old-client" -path '*/bin/xlings' -type f | head -1)"
# As its user's shell has it: the home's SubOS bin on PATH (that client
# looked for patchelf there).
O() { PATH="$old/subos/default/bin:$old/bin:$PATH" XLINGS_HOME="$old" "$oldbin" "$@"; }
N() { XLINGS_HOME="$old" "$XLINGS_HOME/bin/xlings" "$@"; }
NL() { XLINGS_HOME="$old" "$XLINGS_HOME/bin/luban" "$@"; }
step "it lays out a home" ok -- O self init
O config --mirror "${HM_MIRROR:-GLOBAL}" >/dev/null 2>&1
# patchelf on its own first: that client could not relocate a payload with a
# patchelf of the same plan (fixed in 2026.10.12.1).
step "the older client installs patchelf" ok -- O install -y patchelf
step "the older client installs what an edition reuses" ok -- O install -y make ninja zlib
# A client older still wrote no resolution records at all.
rm -f "$old"/data/xpkgs/xim-x-patchelf/*/.xlings-resolution.json
# The upgrade: the build under test becomes the home's entry.
cp "$XLINGS_HOME/bin/xlings" "$old/bin/xlings"
step "the build under test takes the home over" ok -- N self init
step "luban new on it: no failed node" ok -- NL new hmo tiny -y
absent() { ! grep -Eq -- "$1" <<<"$LAST_OUT"; }
check "nothing asked for --reconfig" -- absent '--reconfig'
step "what the older client installed, into the root" ok -- NL run hmo -- xlings install -y patchelf make ninja zlib
check "no node failed" -- absent 'failed|not installed because'
step "a home of the environment's own initialises inside it (mcpp's registry)" ok -- \
    NL run hmo -- /bin/sh -c 'XLINGS_HOME=/root/own-home xlings self init && test -f /root/own-home/subos/default/.xlings.json'
NL rm hmo -y >/dev/null 2>&1
done_
