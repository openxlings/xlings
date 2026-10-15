#!/usr/bin/env bash
# At a terminal, making an environment finishes (a nested install once
# stopped on SIGTTOU and waited forever).
source "$(dirname "$0")/../lib.sh"
command -v script >/dev/null || skip "no script(1) for a terminal"
require_isolation
edition=tiny; X info subos:luban-nano >/dev/null 2>&1 && edition=nano
step "luban new at a terminal finishes ($edition)" ok -- timeout 900 script -qec "$XLINGS_HOME/bin/luban new hmtt --edition $edition" /dev/null
# What making it says reaches the terminal (L §5): the downloads and the
# installs, as a share, under the "making" line.
check "luban new shows its progress at a terminal" -- grep -Eq '[0-9]+%' <<<"$LAST_OUT"
step "luban new --json carries progress events" ok '"event":"(progress|data)"' -- L new hmtj --edition "$edition" --json
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
step "luban enter: the prompt, job control, and an install inside finishes" ok "luban\\].*@$shelled:" -- \
    bash -c "( sleep 10; printf 'echo PROMPT=\$PS1\n'; sleep 2; printf 'tty; sleep 30 & jobs; kill %%1; echo JOBS=\$?\n'; sleep 3; printf 'cd /usr/bin; echo USRBIN=\$(pwd -P); cd /lib; echo LIB=\$(pwd -P); cd\n'; sleep 2; printf 'xlings install -y ninja; echo INSTALL=\$?\n'; sleep 240 ) | timeout 600 script -qec '$XLINGS_HOME/bin/luban enter $shelled' /dev/null"
check "the install inside ended" -- grep -q 'INSTALL=0' <<<"$LAST_OUT"
# The shell has its own terminal and job control (L §4): no warning from sh,
# bash or fish, a terminal name, and a background job it can list and end.
check "the shell has job control" -- absent "can't access tty|job control turned off|tcgetpgrp|Inappropriate ioctl"
check "the shell is on a terminal" -- grep -Eq '/dev/pts/[0-9]+' <<<"$LAST_OUT"
check "a background job is listed and ended" -- grep -q 'JOBS=0' <<<"$LAST_OUT"
check "nothing was stopped" -- absent 'stopped'
# Its directories have their FHS paths (L §6.2): /usr is the generation
# mounted, not a link into the store.
check "/usr/bin is /usr/bin" -- grep -q 'USRBIN=/usr/bin' <<<"$LAST_OUT"
check "/lib is /usr/lib" -- grep -q 'LIB=/usr/lib' <<<"$LAST_OUT"
check "no store path is shown for /usr" -- absent 'USRBIN=.*root\.gen|LIB=.*root\.gen'
done_
