#!/usr/bin/env bash
# Deployment S (design §4): xlings installed by a system package.
#
# CI-only, on a throwaway runner: it installs a root-owned entry with sudo.
#
#   1. `self update` leaves a system package's binary to its package manager:
#      it says so, names the alternative, and downloads nothing;
#   2. `self update --user` is the explicit ask and is not refused;
#   3. `self doctor` says which binary is answering.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

sudo -n true || fail "needs non-interactive sudo"

BIN="$(find_xlings_bin)"
RUNTIME_DIR="$(runtime_home_dir system_install)"
export XLINGS_HOME="$RUNTIME_DIR/home"
rm -rf "$RUNTIME_DIR"; mkdir -p "$XLINGS_HOME"
echo '{"mirror":"GLOBAL"}' > "$XLINGS_HOME/.xlings.json"

SYS_DIR=/usr/local/lib/xlings-system-install-test
cleanup() { sudo rm -rf "$SYS_DIR"; }
trap cleanup EXIT
sudo install -d -m 755 "$SYS_DIR"
sudo install -m 755 "$BIN" "$SYS_DIR/xlings"
X() { ( cd /tmp && "$SYS_DIR/xlings" "$@" ); }

log "1. self update refuses to replace a system package's binary"
set +e
start=$SECONDS
out="$(X self update 2>&1)"; rc=$?
set -e
printf '%s\n' "$out" | sed 's/^/      | /'
[[ $rc -ne 0 ]] || fail "self update reported success on a system package's binary"
grep -q 'belongs to a system package' <<<"$out" || fail "the refusal does not say why"
grep -q 'self update --user' <<<"$out" || fail "the alternative was not named"
! grep -qi 'updating package index' <<<"$out" || fail "it started updating anyway"
(( SECONDS - start < 20 )) || fail "the refusal took $((SECONDS - start))s"

log "2. --user is accepted (it goes on to the usual update)"
set +e
out="$(timeout 20 bash -c "cd /tmp && '$SYS_DIR/xlings' self update --user" 2>&1)"
set -e
! grep -q 'belongs to a system package' <<<"$out" || fail "--user was refused"

log "3. self doctor names the entry"
out="$(X self doctor 2>&1 || true)"
grep -q "entry: $SYS_DIR/xlings (a system package's" <<<"$out" || fail "the doctor did not name the entry"
log "PASS: a system package's xlings is updated by the system"
