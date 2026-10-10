#!/usr/bin/env bash
# At a terminal, making an environment finishes (a nested install once
# stopped on SIGTTOU and waited forever).
source "$(dirname "$0")/../lib.sh"
command -v script >/dev/null || skip "no script(1) for a terminal"
require_isolation
edition=tiny; X info subos:luban-nano >/dev/null 2>&1 && edition=nano
step "luban new at a terminal finishes ($edition)" ok -- timeout 900 script -qec "$XLINGS_HOME/bin/luban new hmtt $edition" /dev/null
done_
