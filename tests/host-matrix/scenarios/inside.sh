#!/usr/bin/env bash
# Inside a Luban system (design 2026-10-16 L §12): what a person meets once
# in, for one edition and user, on this host -- its state (L0), its terminal
# (L1, L1t), its environment and paths (L2), its own tools (L3), and the
# package manager (L4). Parameters: HM_INSIDE_EDITION (tiny), HM_INSIDE_USER
# (root | a name), HM_INSIDE_PERSIST (all | home | none).
source "$(dirname "$0")/../lib.sh"
command -v python3 >/dev/null || skip "no python3 for the terminal driver"
require_isolation
edition="${HM_INSIDE_EDITION:-tiny}"
user="${HM_INSIDE_USER:-root}"
persist="${HM_INSIDE_PERSIST:-all}"
X info "subos:luban-$edition" >/dev/null 2>&1 || skip "the index has no luban-$edition"
name=hmi
PTY() { python3 "$(dirname "$0")/../term.py" "$@"; }
new_args=(--edition "$edition")
[[ "$user" != root ]] && new_args+=(--user "$user" --no-password)
step "luban new $name (${new_args[*]})" ok -- L new "$name" "${new_args[@]}"
[[ "$persist" != all ]] && step "it keeps $persist" ok -- L config "$name" --persist "$persist"

# L0, L3: its own self-check, what the system says of itself.
step "L0/L3: its self-check finds it usable" ok '"ok":true' -- L status "$name" --check --json
# nano has no shell: what it holds is its self-check's to say.
[[ "$edition" == nano ]] && done_

# L1: a terminal of its own, job control, the window's size; no shell
# complains (design L §4). The prompt is any shell's.
prompt='[$#>] ?$'
step "L1: a terminal, job control, the window's size" ok -- \
    PTY --out "$HM_STATE/inside-l1.log" \
        --step "$prompt ::: echo TTY=\$(tty)\\r" \
        --step "TTY=/ ::: stty size | sed 's/^/SIZE=/'\\r" \
        --step "SIZE=[0-9] ::: sleep 30 & jobs >/dev/null; kill %1; echo JOBS=\$?\\r" \
        --step "JOBS=[0-9] ::: exit\\r" \
        --expect 'TTY=/dev/pts/[0-9]+' --expect 'SIZE=40 120' --expect 'JOBS=0' \
        --reject "can't access tty|job control turned off|tcgetpgrp|Inappropriate ioctl|cannot set terminal process group" \
        -- "$XLINGS_HOME/bin/luban" enter "$name"
# Every shell the system lists starts at its terminal without a word.
shells="$(L run "$name" -- /bin/sh -c 'for s in $(grep "^/" /etc/shells 2>/dev/null); do [ -x "$s" ] && echo "$s"; done' 2>/dev/null | sort -u)"
for sh in $shells; do
    step "L1: $sh starts at a terminal" ok -- \
        PTY --out "$HM_STATE/inside-shell-$(basename "$sh").log" \
            --step "$prompt ::: exec $sh\\r" --step "$prompt ::: echo SHELL  OK\\r" --step "SHELL OK ::: exit\\r" \
            --expect 'SHELL OK' \
            --reject "can't access tty|job control turned off|tcgetpgrp|Inappropriate ioctl|cannot set terminal process group" \
            -- "$XLINGS_HOME/bin/luban" enter "$name"
done
# L1t: the terminal is described where the system has descriptions.
step "L1t: the terminal is described, or there are none to have" ok 'TERMINFO=(ok|none)' -- \
    L run "$name" -- /bin/sh -c 'if [ -d /usr/share/terminfo ]; then t=${TERM:-xterm-256color}; ls /usr/share/terminfo/*/"$t" >/dev/null 2>&1 && echo TERMINFO=ok || echo TERMINFO=missing; else echo TERMINFO=none; fi'

# L2: paths are what a person types (design L §6), and its environment is its own.
step "L2: /usr/bin is /usr/bin, /lib is /usr/lib" ok 'USR=/usr/bin LIB=/usr/lib' -- \
    L run "$name" -- /bin/sh -c 'cd /usr/bin && u=$(pwd -P); cd /lib && l=$(pwd -P); echo "USR=$u LIB=$l"'
step "L2: HOME and PATH are the system's" ok 'PATH=.*/usr/bin' -- \
    L run "$name" -- /bin/sh -c 'echo "HOME=$HOME"; echo "PATH=$PATH"'
check "L2: no host path in its PATH" -- bash -c '! grep -q "PATH=.*$1/\.xlings" <<<"$2"' _ "$HOME" "$LAST_OUT"

# L4: the package manager inside -- the index is there for every query
# (design L §7), and an install asks before it changes anything.
step "L4: xlings list inside" ok -- L run "$name" -- /usr/bin/xlings list
step "L4: xlings search inside" ok 'busybox|ninja' -- L run "$name" -- /usr/bin/xlings search busybox
step "L4: xlings info inside" ok -- L run "$name" -- /usr/bin/xlings info busybox
# A package this home's store does not have yet: one that asks.
absent=""
for p in tree jq fd ripgrep fzf; do
    [[ -d "$XLINGS_HOME/data/xpkgs/xim-x-$p" ]] && continue
    X info "$p" >/dev/null 2>&1 && { absent="$p"; break; }
done
if [[ -n "$absent" ]]; then
    step "L4: an install without -y asks, and n changes nothing ($absent)" ok -- \
        PTY --out "$HM_STATE/inside-confirm.log" \
            --step "$prompt ::: xlings install $absent; echo INSTALL=\$?\\r" \
            --step '\[[Yy]/[Nn]\] ?$ ::: n\r' \
            --step "INSTALL=[0-9] ::: exit\\r" \
            --expect '\[[Yy]/[Nn]\]' \
            -- "$XLINGS_HOME/bin/luban" enter "$name"
    check "L4: n installed nothing ($absent)" -- test ! -d "$XLINGS_HOME/data/xpkgs/xim-x-$absent"
fi
if [[ "$user" != root ]]; then
    # A regular user's xlings is a view of the system's store (design
    # 2026-10-20 §4): what root added is theirs, not installed again; a
    # sandbox installs through its broker into the system.
    step "L4: root adds ninja for every user" ok 'INSTALL=0' -- \
        L run "$name" --user root -- /bin/sh -c 'xlings install -y ninja; echo INSTALL=$?'
    step "L4: $user has it from the system" ok 'provided by the system' -- \
        L run "$name" --user "$user" -- /bin/sh -c 'xlings install ninja; echo INSTALL=$?'
fi
done_
