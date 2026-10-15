#!/usr/bin/env bash
# Luban editions on this host: the host is checked before anything is
# fetched (agent mode: exit 2 and the command), each edition the index has is
# made and entered, and luban reports them.
source "$(dirname "$0")/../lib.sh"
has() { X info "subos:$1" >/dev/null 2>&1; }   # search echoes the query: not a test
first=tiny; has luban-nano && first=nano
if [[ -z "${HM_SETUP_DONE:-}" ]] && ! X self doctor --isolation >/dev/null 2>&1; then
    # Agent mode never waits. Either the host needs the one-time setup (exit 2,
    # naming it), or what was missing was a package xlings installs itself (a
    # bwrap with overlays) and the system is made: a distribution container
    # whose kernel already allows user namespaces is the second case (U18).
    agent_out="$(L new hma --edition "$first" --agent 2>&1)"; agent_rc=$?
    if [[ $agent_rc -eq 2 ]] && grep -Eq 'self doctor --isolation --fix|luban setup' <<<"$agent_out"; then
        record "agent mode: luban new asks nothing, exits 2 with the setup" true 2 "$agent_out"
        needs_setup "$agent_out"
    elif [[ $agent_rc -eq 0 ]]; then
        record "agent mode: luban new asks nothing; this host needed no administrator" true 0 "$agent_out"
        L rm hma -y >/dev/null 2>&1
    else
        record "agent mode: luban new asks nothing" false "$agent_rc" "$agent_out"; FAILED=1
    fi
fi
require_isolation
if has luban-nano; then
    step "luban new hmn --edition nano" ok -- L new hmn --edition nano
    step "nano: xlings runs inside (no shell there)" ok -- X subos exec hmn -- /usr/bin/xlings --version
else
    record "skip: the index has no luban-nano yet" true 0 ""
fi
step "luban new hmt --edition tiny" ok -- L new hmt --edition tiny
step "a name in use is said in luban's words, before anything is fetched" 2 'luban rm hmt' -- L new hmt --edition tiny
step "tiny: a shell and Luban's os-release" ok 'ID=luban' -- L run hmt -- /bin/sh -c 'cat /usr/share/factory/etc/os-release /etc/os-release 2>/dev/null'
step "tiny: luban runs inside" ok -- L run hmt -- /usr/bin/luban --version
step "luban status" ok -- L status hmt
# Its self-check (L §11), asked from the host (it runs inside), and read by
# a machine.
step "luban status --check: usable" ok 'usable' -- L status hmt --check
step "luban status --check --json: every item, and ok" ok '"ok":true' -- L status hmt --check --json
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
