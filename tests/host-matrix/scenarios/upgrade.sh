#!/usr/bin/env bash
# An environment made from an older edition moves to the newest, and back.
source "$(dirname "$0")/../lib.sh"
require_isolation
out="$(L new hmu tiny@0.1.0 2>&1)"; rc=$?
if [[ $rc -ne 0 && ! -e "$XLINGS_HOME/subos/hmu" ]]; then
    record "tiny@0.1.0 is not offered here, and nothing was made" true "$rc" "$out"
    skip "no older edition of tiny for $(uname -m) to upgrade from"
fi
record "luban new hmu tiny@0.1.0" "$([[ $rc -eq 0 ]] && echo true || echo false)" "$rc" "$out"
[[ $rc -eq 0 ]] || { FAILED=1; done_; }
step "the plan" ok -- L upgrade hmu --dry-run
grep -q 'up to date' <<<"$LAST_OUT" && skip "the index has one version of tiny"
step "agent mode: nothing changes without -y" 2 -- L upgrade hmu --agent
step "upgrade" ok -- L upgrade hmu -y
step "it is the newest edition now" ok 'luban-tiny@2026' -- cat "$XLINGS_HOME/config/subos/hmu/instance.json"
step "it still enters" ok ok -- L run hmu -- /bin/sh -c 'echo ok'
step "rollback" ok -- X subos rollback hmu
done_
