#!/usr/bin/env bash
# At a terminal, making an environment finishes (a nested install once
# stopped on SIGTTOU and waited forever).
source "$(dirname "$0")/../lib.sh"
command -v script >/dev/null || skip "no script(1) for a terminal"
require_isolation
edition=tiny; X info subos:luban-nano >/dev/null 2>&1 && edition=nano
step "luban new at a terminal finishes ($edition)" ok -- timeout 900 script -qec "$XLINGS_HOME/bin/luban new hmtt --edition $edition" /dev/null
# Inside, at a terminal: the prompt says where, and an install there (run by
# the broker on this terminal) neither stops the session nor waits forever --
# it once stopped `luban enter` itself with SIGTTOU. In a tiny (nano has no
# shell).
shelled=hmtt
if [[ "$edition" != tiny ]]; then shelled=hmtp; step "luban new hmtp --edition tiny" ok -- L new hmtp --edition tiny; fi
absent() { ! grep -Eqi -- "$1" <<<"$LAST_OUT"; }
# Typed as a person types, once the shell is there: typed ahead, the lines
# are read by the terminal queries of the commands starting up (the colour
# probe), as they would be on any terminal.
step "luban enter: the prompt, and an install inside finishes" ok "luban\\].*@$shelled:" -- \
    bash -c "( sleep 10; printf 'echo PROMPT=\$PS1\n'; sleep 2; printf 'xlings install -y ninja; echo INSTALL=\$?\n'; sleep 240 ) | timeout 600 script -qec '$XLINGS_HOME/bin/luban enter $shelled' /dev/null"
check "the install inside ended" -- grep -q 'INSTALL=0' <<<"$LAST_OUT"
check "nothing was stopped" -- absent 'stopped'
done_
