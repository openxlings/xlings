#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/release_test_lib.sh"

ARCHIVE_PATH="${1:-$ROOT_DIR/build/release.tar.gz}"
require_release_archive "$ARCHIVE_PATH"
require_fixture_index

PKG_DIR="$(extract_release_archive "$ARCHIVE_PATH" release_subos_smoke)"
write_fixture_release_config "$PKG_DIR"

export XLINGS_HOME="$PKG_DIR"
export PATH="$XLINGS_HOME/bin:$(minimal_system_path)"

xlings -h >/dev/null
xlings --verbose config >/dev/null
xlings --version >/dev/null

xlings --verbose self init
export PATH="$XLINGS_HOME/subos/current/bin:$XLINGS_HOME/bin:$(minimal_system_path)"
xlings --verbose update
xlings subos list >/dev/null

D2X_VERSION="${D2X_VERSION:-$(default_d2x_version)}"

install_d2x_contract() {
  local subos_name="$1" output status shim
  shim="$XLINGS_HOME/subos/$subos_name/bin/d2x"
  set +e
  output=$(xlings --verbose install "d2x@$D2X_VERSION" -y 2>&1)
  status=$?
  set -e
  if [[ $status -eq 0 ]]; then
    [[ -x "$shim" ]] || fail "$subos_name d2x shim missing"
    return 0
  fi
  grep -q 'E_UNSUPPORTED_TARGET' <<<"$output" \
    || { printf '%s\n' "$output" >&2; fail "$subos_name d2x install failed unexpectedly"; }
  [[ ! -e "$shim" ]] || fail "$subos_name unsupported d2x created a shim"
}

xlings subos new s1
[[ -f "$XLINGS_HOME/subos/s1/.xlings.json" ]] || fail "s1 config missing"
xlings subos use s1 --global
readlink "$XLINGS_HOME/subos/current" | grep -q "s1" || fail "subos/current not pointing to s1"

install_d2x_contract s1

xlings subos new s2
[[ -f "$XLINGS_HOME/subos/s2/.xlings.json" ]] || fail "s2 config missing"
xlings subos use s2 --global
readlink "$XLINGS_HOME/subos/current" | grep -q "s2" || fail "subos/current not pointing to s2"

install_d2x_contract s2

xlings subos use s1 --global
readlink "$XLINGS_HOME/subos/current" | grep -q "s1" || fail "failed to switch back to s1"
xlings subos use s2 --global
readlink "$XLINGS_HOME/subos/current" | grep -q "s2" || fail "failed to switch back to s2"
xlings subos list >/dev/null

xlings subos use default --global
xlings subos remove s1
xlings subos remove s2

log "PASS: release subos smoke scenario"
