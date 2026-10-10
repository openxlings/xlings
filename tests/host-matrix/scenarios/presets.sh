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
step "locked: the host's home is not there" ok 'hidden' -- X subos exec hmp --sandbox=locked --no-degrade -- \
    /bin/sh -c "test -e '$HOME/.xlings/.xlings.json' && echo visible || echo hidden"
done_
