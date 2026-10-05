#!/usr/bin/env bash
# self doctor --isolation --fix on a stock Ubuntu 24.04 (#640's comment).
#
# CI-only, on a throwaway runner: it installs /usr/lib/xlings/bwrap and an
# AppArmor profile with sudo. The kernel's user-namespace restriction is left
# exactly as the image ships it -- the whole point is that the repair works
# WITHOUT turning it off.
#
#   1. the payload bwrap (`xlings install bwrap`) cannot make a sandbox: it is
#      unconfined, and AppArmor denies it user namespaces;
#   2. the doctor says so, names the restriction, and does not advise sysctl;
#   3. --fix -y installs the root-owned copy and its profile;
#   4. a sandbox now enters, through /usr/lib/xlings/bwrap.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

[[ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null)" == 1 ]] \
  || { log "SKIP: this host does not restrict user namespaces through AppArmor"; exit 0; }
sudo -n true || fail "needs non-interactive sudo"

BIN="$(find_xlings_bin)"
RUNTIME_DIR="$(runtime_home_dir isolation_doctor_fix)"
export XLINGS_HOME="$RUNTIME_DIR/home"
rm -rf "$RUNTIME_DIR"; mkdir -p "$XLINGS_HOME"
X() { ( cd /tmp && "$BIN" "$@" ); }

X self init >/dev/null
X install -y bwrap >/dev/null
X subos new box >/dev/null

log "1. before: the doctor reports the restriction and no usable bwrap"
set +e
before="$(X self doctor --isolation 2>&1)"; rc=$?
set -e
printf '%s\n' "$before" | sed 's/^/      | /'
[[ $rc -ne 0 ]] || fail "the doctor reported a usable bwrap before the fix"
grep -q 'apparmor_restrict_unprivileged_userns *1' <<<"$before" || fail "restriction not named"
grep -q 'self doctor --isolation --fix' <<<"$before" || fail "the fix was not offered"
! grep -q 'sysctl -w' <<<"$before" || fail "a global sysctl was advised"

log "1b. before: entering a sandbox quotes bwrap and offers the same fix (F12)"
set +e
entry="$(X subos exec box --sandbox -- true 2>&1)"; rc=$?
set -e
printf '%s\n' "$entry" | sed 's/^/      | /'
[[ $rc -ne 0 ]] || fail "a sandbox was entered before the fix"
grep -q 'self doctor --isolation --fix' <<<"$entry" || fail "the entry failure did not offer the fix"
! grep -q 'sysctl -w' <<<"$entry" || fail "the entry failure advised a global sysctl"

log "2. --fix -y"
X self doctor --isolation --fix -y

log "3. after: a sandbox enters through the root-owned bwrap"
[[ "$(stat -c '%u %a' /usr/lib/xlings/bwrap)" == "0 755" ]] || fail "not root-owned 0755"
out="$(X subos exec box --sandbox -- /bin/sh -c 'echo inside=$(ls /proc | grep -c "^[0-9]*$")')"
grep -q 'inside=' <<<"$out" || fail "no sandbox: $out"
X self doctor --isolation --json | grep -q '"source":"root-owned"' || fail "not the root-owned bwrap"
log "PASS: self doctor --isolation --fix makes sandboxes work with the restriction left on"
