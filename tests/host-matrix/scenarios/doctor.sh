#!/usr/bin/env bash
# The doctor's verdict is the truth of this host: satisfied means every
# preset enters; not satisfied names the fix when there is one.
source "$(dirname "$0")/../lib.sh"
require_isolation
X subos new hmd >/dev/null 2>&1
step "a sandbox enters, as the doctor said" ok inside -- X subos exec hmd --sandbox=dev -- /bin/sh -c 'echo inside'
step "the doctor, again" ok -- X self doctor --isolation
check "no global sysctl is ever advised" -- eval '! grep -q "sysctl -w" <<<"$LAST_OUT"'
done_
