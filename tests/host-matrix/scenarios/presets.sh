#!/usr/bin/env bash
# Each preset enters, and does what it says: locked has no network and no
# nested user namespaces (a setuid bwrap cannot forbid them: refused first).
source "$(dirname "$0")/../lib.sh"
require_isolation
X subos new hmp >/dev/null 2>&1
step "dev enters" ok inside -- X subos exec hmp --sandbox=dev -- /bin/sh -c 'echo inside'
step "locked: loopback only" ok '^lo,$' -- X subos exec hmp --sandbox=locked --no-degrade -- \
    /bin/sh -c 'tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d " " | tr "\n" ,; echo'
if command -v unshare >/dev/null; then
    step "locked: no nested user namespaces" ok 'nested=no' -- X subos exec hmp --sandbox=locked --no-degrade -- \
        /bin/sh -c 'if unshare -U true 2>/dev/null; then echo nested=yes; else echo nested=no; fi'
fi
echo secret > "$HOME/hm-secret"
step "locked: the user's files are not there" ok 'hidden' -- X subos exec hmp --sandbox=locked --no-degrade -- \
    /bin/sh -c "test -e '$HOME/hm-secret' && echo visible || echo hidden"
step "dev: the user's files are not there either" ok 'hidden' -- X subos exec hmp --sandbox=dev -- \
    /bin/sh -c "test -e '$HOME/hm-secret' && echo visible || echo hidden"
done_
