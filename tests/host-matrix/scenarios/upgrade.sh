#!/usr/bin/env bash
# An environment made from an older edition moves to the newest, and back.
source "$(dirname "$0")/../lib.sh"
require_isolation
step "luban new hmu tiny@0.1.0" ok -- L new hmu tiny@0.1.0
step "the plan" ok -- L upgrade hmu --dry-run
grep -q 'up to date' <<<"$LAST_OUT" && skip "the index has one version of tiny"
step "agent mode: nothing changes without -y" 2 -- L upgrade hmu --agent
step "upgrade" ok -- L upgrade hmu -y
step "it is the newest edition now" ok 'luban-tiny@2026' -- cat "$XLINGS_HOME/config/subos/hmu/instance.json"
step "it still enters" ok ok -- L run hmu -- /bin/sh -c 'echo ok'
step "rollback" ok -- X subos rollback hmu
done_
