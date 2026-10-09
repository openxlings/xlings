#!/usr/bin/env bash
# Deployment M (design part 2 §10): a root-owned system layer every user of
# the machine shares.
#
#   1. `sudo xlings install --system xz` lays /xlings out (mode multi, its
#      own entry) and installs into it, root's files;
#   2. a user's shell (their profile) finds the system's xz after their own
#      home and before the host, without a copy in their home;
#   3. the user cannot write the system layer;
#   4. the user's own install wins over the system's;
#   5. self doctor names the layer.
#
# CI-only: it creates /xlings with sudo.
#
# xtest: covers=DEPLOY-M-SYSTEM,HOME-LAYER-RESOLVE requires=sudo,network
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

sudo -n true || fail "needs non-interactive sudo"
BIN="$(find_xlings_bin)"
RUNTIME_DIR="$(runtime_home_dir system_layer)"
rm -rf "$RUNTIME_DIR"; mkdir -p "$RUNTIME_DIR"
cleanup() { sudo -n rm -rf /xlings; }
trap cleanup EXIT
sudo -n rm -rf /xlings

log "1. sudo install --system"
sudo -n env -u XLINGS_HOME -u XLINGS_ACTIVE_SUBOS "$BIN" install --system -y xz > "$RUNTIME_DIR/system.log" 2>&1 \
  || { tail -30 "$RUNTIME_DIR/system.log"; fail "install --system"; }
[[ "$(stat -c %U /xlings/data/xpkgs)" == root ]] || fail "the system layer is not root's"
grep -q '"mode": "multi"' /xlings/.xlings-home || fail "no multi marker"
[[ -x /xlings/subos/default/bin/xz ]] || fail "no xz in the system layer"

log "2. a user's shell resolves it, no copy"
U="$RUNTIME_DIR/home"
mkdir -p "$U/bin"; cp "$BIN" "$U/bin/xlings"
echo '{"mirror":"GLOBAL"}' > "$U/.xlings.json"
( cd /tmp && XLINGS_HOME="$U" "$U/bin/xlings" self init >/dev/null 2>&1 ) || fail "user self init"
shell() { env -i PATH=/usr/bin:/bin HOME="$RUNTIME_DIR" bash -c ". '$U/config/shell/xlings-profile.sh'; $1"; }
out="$(shell 'command -v xz; xz --version | head -1')"
grep -q '^/xlings/subos/default/bin/xz$' <<<"$out" || fail "the shell does not find the system's xz first: $out"
grep -q 'XZ Utils' <<<"$out" || fail "the system's xz does not run for the user: $out"
[[ ! -d "$U/data/xpkgs/xim-x-xz" ]] || fail "the user's home got a copy"

log "3. the user cannot write it"
touch /xlings/data/xpkgs/probe 2>/dev/null && fail "a user wrote into the system layer"
out="$(env -u XLINGS_HOME "$BIN" install --system -y zlib 2>&1)" && fail "a user installed into the system layer"
grep -q "root's" <<<"$out" || fail "the refusal does not say why: $out"

log "4. the user's own wins"
( cd /tmp && XLINGS_HOME="$U" "$U/bin/xlings" install -y xz >/dev/null 2>&1 ) || fail "user install xz"
out="$(shell 'command -v xz')"
[[ "$out" == "$U/subos/current/bin/xz" ]] || fail "the user's xz does not come first: $out"

log "5. doctor names the layer"
out="$( (cd /tmp && XLINGS_HOME="$U" "$U/bin/xlings" self doctor) 2>&1 || true)"
grep -q "system layer: /xlings" <<<"$out" || fail "doctor does not name the layer: $out"
log "PASS: one system layer, every user, their own first"
