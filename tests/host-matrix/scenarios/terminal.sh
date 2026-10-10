#!/usr/bin/env bash
# At a terminal, making an environment finishes (a nested install once
# stopped on SIGTTOU and waited forever).
source "$(dirname "$0")/../lib.sh"
command -v script >/dev/null || skip "no script(1) for a terminal"
require_isolation
step "luban new at a terminal finishes" ok -- timeout 900 script -qec "$XLINGS_HOME/bin/luban new hmtt nano" /dev/null
done_
