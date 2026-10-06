#!/usr/bin/env bash
# Deployment S (design §4): xlings installed by a system package.
#
# CI-only, on a throwaway runner: it installs a root-owned entry with sudo.
#
#   1. `self update` leaves a system package's binary to its package manager:
#      it says so, names the alternative, and downloads nothing;
#   2. `self update --user` is the explicit ask and is not refused;
#   3. `self doctor` says which binary is answering;
#   4. a home that has never been laid out is told it has no entry, and the
#      first install links it to the system binary, so what it installs runs.
#
# xtest: covers=HOME-SYSTEM-MODE,DEPLOY-S-ENTRY requires=sudo,network
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
log "4. a fresh home links its entry to the system binary"
FRESH="$RUNTIME_DIR/fresh"
mkdir -p "$FRESH"
echo '{"mirror":"GLOBAL"}' > "$FRESH/.xlings.json"
F() { ( cd /tmp && XLINGS_HOME="$FRESH" "$SYS_DIR/xlings" "$@" ); }
out="$(F self doctor 2>&1 || true)"
grep -q "this home has no entry" <<<"$out" || fail "doctor did not say the home has no entry"
[[ ! -e "$FRESH/bin/xlings" ]] || fail "a read-only doctor created the entry"
F install -y xz >/dev/null 2>&1 || fail "install into a fresh home failed"
[[ "$(readlink "$FRESH/bin/xlings")" == "$SYS_DIR/xlings" ]] \
  || fail "the entry is not a link to the system binary: $(ls -l "$FRESH/bin/xlings" 2>&1)"
[[ -x "$FRESH/subos/default/bin/xz" ]] || fail "no shim for what was installed"
"$FRESH/subos/default/bin/xz" --version | grep -q "xz (XZ Utils)" \
  || fail "the installed xz does not run through its shim"
out="$( (cd /tmp && XLINGS_HOME="$FRESH" "$FRESH/bin/xlings" self update) 2>&1 || true)"
grep -q 'belongs to a system package' <<<"$out" || fail "self update through the link did not refuse"

log "PASS: a system package's xlings is updated by the system"
