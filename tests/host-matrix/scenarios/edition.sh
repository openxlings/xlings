#!/usr/bin/env bash
# Luban editions on this host: the host is checked before anything is
# fetched (agent mode: exit 2 and the command), each edition the index has is
# made and entered, and luban reports them.
source "$(dirname "$0")/../lib.sh"
has() { X info "subos:$1" >/dev/null 2>&1; }   # search echoes the query: not a test
first=tiny; has luban-nano && first=nano
if [[ -z "${HM_SETUP_DONE:-}" ]] && ! X self doctor --isolation >/dev/null 2>&1; then
    step "agent mode: luban new asks nothing, exits 2 with the setup" 2 'self doctor --isolation --fix|luban setup' -- \
        L new hmn --edition "$first" --agent
    needs_setup "$LAST_OUT"
fi
require_isolation
if has luban-nano; then
    step "luban new hmn --edition nano" ok -- L new hmn --edition nano
    step "nano: xlings runs inside (no shell there)" ok -- X subos exec hmn -- /usr/bin/xlings --version
else
    record "skip: the index has no luban-nano yet" true 0 ""
fi
step "luban new hmt --edition tiny" ok -- L new hmt --edition tiny
step "tiny: a shell and Luban's os-release" ok 'ID=luban' -- L run hmt -- /bin/sh -c 'cat /usr/share/factory/etc/os-release /etc/os-release 2>/dev/null'
step "tiny: luban runs inside" ok -- L run hmt -- /usr/bin/luban --version
step "luban status" ok -- L status hmt
step "luban ls" ok 'hmt' -- L ls
if has luban-tiny-musl; then
    out="$(L new hmm --edition tiny-musl 2>&1)"; rc=$?
    if grep -q 'E_UNSUPPORTED_TARGET' <<<"$out"; then
        # Not built for this machine (musl has no build for it): refused
        # before anything is made -- that is the answer to check.
        check "tiny-musl is refused before anything is made (not built for $(uname -m))" -- \
            test "$rc" -ne 0 -a ! -e "$XLINGS_HOME/subos/hmm"
    else
        record "luban new hmm --edition tiny-musl" "$([[ $rc -eq 0 ]] && echo true || echo false)" "$rc" "$out"
        [[ $rc -eq 0 ]] || FAILED=1
        step "tiny-musl: a shell on musl" ok musl -- L run hmm -- /bin/sh -c 'ls /lib/ld-musl-* /usr/lib/ld-musl-* 2>/dev/null'
    fi
fi
done_
