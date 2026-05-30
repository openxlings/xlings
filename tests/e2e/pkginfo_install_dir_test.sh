#!/usr/bin/env bash
# E2E test: install linux-headers via workspace config,
# verify headers are copied into anonymous subos sysroot.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

SCENARIO_DIR="$ROOT_DIR/tests/e2e/scenarios/linux_headers"
HOME_DIR="$(runtime_home_dir linux_headers_home)"
LINUX_HEADERS_INDEX_DIR="$ROOT_DIR/tests/e2e/fixtures/project_index"
[[ -d "$LINUX_HEADERS_INDEX_DIR/pkgs" ]] \
  || fail "linux-headers fixture index missing: $LINUX_HEADERS_INDEX_DIR"

CONFIG_BACKUP="$(prepare_scenario "$SCENARIO_DIR" "$HOME_DIR")"
cleanup() {
  restore_scenario "$SCENARIO_DIR" "$HOME_DIR" "$CONFIG_BACKUP"
}
trap cleanup EXIT

# ── 1. Set up home and sync the offline fixture index ──
write_home_config "$HOME_DIR" "GLOBAL" "$LINUX_HEADERS_INDEX_DIR"

(cd "$SCENARIO_DIR" && run_xlings "$HOME_DIR" "$SCENARIO_DIR" install -y)

# ── 2. Verify anonymous subos sysroot has the headers ──
ANON_SUBOS="$SCENARIO_DIR/.xlings/subos/_"
[[ -d "$ANON_SUBOS" ]] \
  || fail "anonymous subos dir not created at $ANON_SUBOS"

[[ -f "$ANON_SUBOS/usr/include/linux/errno.h" ]] \
  || fail "linux/errno.h not found in anonymous subos sysroot"

log "PASS: linux-headers install (pkginfo.install_dir + anonymous subos)"
