#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

run_and_capture() {
  local output_file
  local status
  output_file="$(mktemp)"
  set +e
  "$@" 2>&1 | tee "$output_file"
  status=${PIPESTATUS[0]}
  set -e
  cat "$output_file"
  rm -f "$output_file"
  if [[ $status -ne 0 ]]; then
    return $status
  fi
}

HOME_DIR="$(runtime_home_dir subos_refcount_home)"
REFCOUNT_INDEX_DIR="$ROOT_DIR/tests/e2e/fixtures/project_index"
[[ -d "$REFCOUNT_INDEX_DIR/pkgs" ]] || fail "refcount fixture index missing: $REFCOUNT_INDEX_DIR"
rm -rf "$HOME_DIR"
write_home_config "$HOME_DIR" "GLOBAL" "$REFCOUNT_INDEX_DIR"

run_xlings "$HOME_DIR" "$ROOT_DIR" self init >/dev/null
run_xlings "$HOME_DIR" "$ROOT_DIR" update >/dev/null

run_xlings "$HOME_DIR" "$ROOT_DIR" subos new s1 >/dev/null
run_xlings "$HOME_DIR" "$ROOT_DIR" subos new s2 >/dev/null

for subos_name in s1 s2; do
  SUBOS_DIR="$HOME_DIR/subos/$subos_name"
  [[ -d "$SUBOS_DIR/bin" ]] || fail "$subos_name bin dir missing after subos new"
  [[ -d "$SUBOS_DIR/lib" ]] || fail "$subos_name lib dir missing after subos new"
  [[ -d "$SUBOS_DIR/usr" ]] || fail "$subos_name usr dir missing after subos new"
  [[ -d "$SUBOS_DIR/generations" ]] || fail "$subos_name generations dir missing after subos new"
  [[ -f "$SUBOS_DIR/.xlings.json" ]] || fail "$subos_name .xlings.json missing after subos new"
  grep -q '"workspace"[[:space:]]*:[[:space:]]*{' "$SUBOS_DIR/.xlings.json" || \
    fail "$subos_name .xlings.json missing workspace object"
done

run_xlings "$HOME_DIR" "$ROOT_DIR" subos use s1 --global >/dev/null
run_and_capture run_xlings "$HOME_DIR" "$ROOT_DIR" install node@22.17.1 -y

PAYLOAD_DIR="$HOME_DIR/data/xpkgs/xim-x-node/22.17.1"
[[ -x "$PAYLOAD_DIR/bin/node" ]] || fail "node payload missing after s1 install"
[[ -x "$HOME_DIR/subos/s1/bin/node" ]] || fail "s1 shim missing after install"

run_xlings "$HOME_DIR" "$ROOT_DIR" subos use s2 --global >/dev/null
run_and_capture run_xlings "$HOME_DIR" "$ROOT_DIR" install node@22.17.1 -y

# Verify s2 reuses existing payload (shim created, payload still at same location)
[[ -x "$HOME_DIR/subos/s2/bin/node" ]] || fail "s2 shim missing after reused install"
[[ -x "$PAYLOAD_DIR/bin/node" ]] || fail "payload should still exist for s2"

NODE_VER_S2="$(
  env XLINGS_HOME="$HOME_DIR" "$HOME_DIR/subos/s2/bin/node" --version
)"
[[ "$NODE_VER_S2" == "v22.17.1" ]] || fail "s2 node shim did not resolve to v22.17.1"

run_xlings "$HOME_DIR" "$ROOT_DIR" subos use s1 --global >/dev/null
REMOVE_S1="$(run_and_capture run_xlings "$HOME_DIR" "$ROOT_DIR" remove node -y)"
[[ -x "$PAYLOAD_DIR/bin/node" ]] || fail "payload removed even though s2 still referenced it"
[[ ! -e "$HOME_DIR/subos/s1/bin/node" ]] || fail "s1 shim still present after detach"

run_xlings "$HOME_DIR" "$ROOT_DIR" subos use s2 --global >/dev/null
REMOVE_S2="$(run_and_capture run_xlings "$HOME_DIR" "$ROOT_DIR" remove node -y)"
[[ ! -e "$PAYLOAD_DIR" ]] || fail "payload still present after last subos reference was removed"
[[ ! -e "$HOME_DIR/subos/s2/bin/node" ]] || fail "s2 shim still present after final remove"

log "PASS: subos payload refcount scenario"
