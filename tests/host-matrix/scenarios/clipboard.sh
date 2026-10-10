#!/usr/bin/env bash
# The clipboard is configuration: not granted, an environment has no
# clipboard tools of the host's; granted, wl-copy & co. inside are the
# bridge to the host's clipboard, and each direction is its own grant.
source "$(dirname "$0")/../lib.sh"
require_isolation
X subos new hmc >/dev/null 2>&1
X subos config hmc --sandbox dev >/dev/null 2>&1
step "not granted: no bridge" ok 'none' -- X subos exec hmc -- /bin/sh -c 'test -e /run/xlings/bin/wl-copy && echo bridge || echo none'
step "grant clipboard" ok -- X subos config hmc --allow clipboard
step "granted: wl-copy is the bridge, first on PATH" ok '/run/xlings/bin/wl-copy' -- \
    X subos exec hmc -- /bin/sh -c 'command -v wl-copy'
step "paste is its own grant" ok 'clipboard-paste' -- X subos exec hmc -- /bin/sh -c 'wl-paste 2>&1; true'
done_
