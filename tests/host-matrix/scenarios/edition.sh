#!/usr/bin/env bash
# Luban editions on this host: the host is checked before anything is
# fetched (agent mode: exit 2 and the command), nano and tiny are made and
# entered, and luban reports them.
source "$(dirname "$0")/../lib.sh"
if [[ -z "${HM_SETUP_DONE:-}" ]] && ! X self doctor --isolation >/dev/null 2>&1; then
    step "agent mode: luban new asks nothing, exits 2 with the setup" 2 'self doctor --isolation --fix|luban setup' -- \
        L new hmn nano --agent
    needs_setup "$LAST_OUT"
fi
require_isolation
step "luban new hmn nano" ok -- L new hmn nano
step "nano: xlings runs inside (no shell there)" ok -- X subos exec hmn -- /usr/bin/xlings --version
step "luban new hmt tiny" ok -- L new hmt tiny
step "tiny: a shell and Luban's os-release" ok 'ID=luban' -- L run hmt -- /bin/sh -c 'cat /usr/share/factory/etc/os-release /etc/os-release 2>/dev/null'
step "tiny: luban runs inside" ok -- L run hmt -- /usr/bin/luban --version
step "luban status" ok -- L status hmt
step "luban ls" ok 'hmt' -- L ls
done_
