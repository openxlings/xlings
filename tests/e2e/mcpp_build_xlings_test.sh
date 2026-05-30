#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$(runtime_home_dir mcpp_build_xlings)"
MCPP_HOME_DIR="$RUNTIME_DIR/mcpp-home"
XLINGS_HOME_DIR="$RUNTIME_DIR/xlings-home"
MCPP_BIN="${MCPP_BIN:-mcpp}"
MCPP_TARGET="${MCPP_TARGET:-x86_64-linux-musl}"

resolve_mcpp_bin() {
  local candidate="$1"
  local resolved=""

  if [[ "$candidate" == */* ]]; then
    resolved="$candidate"
  else
    resolved="$(command -v "$candidate" || true)"
  fi

  [[ -n "$resolved" && -x "$resolved" ]] || fail "mcpp binary not found; set MCPP_BIN"

  if [[ "$(basename "$(readlink -f "$resolved")")" == "xlings" ]]; then
    local source_xlings_home="${XLINGS_HOME:-$HOME/.xlings}"
    local direct_bin
    direct_bin="$(
      find "$source_xlings_home/data/xpkgs" -path '*/data/xpkgs/*-x-mcpp/*/bin/mcpp' -type f -executable 2>/dev/null |
        while IFS= read -r bin; do
          local version
          version="$("$bin" --version 2>/dev/null | sed -n 's/^mcpp \([^ ]*\).*/\1/p' | head -1)"
          [[ -n "$version" ]] && printf '%s\t%s\n' "$version" "$bin"
        done |
        sort -t $'\t' -k1,1V |
        tail -1 |
        cut -f2-
    )"
    [[ -n "$direct_bin" ]] || fail "mcpp shim resolved to xlings; set MCPP_BIN to a real mcpp binary"
    resolved="$direct_bin"
  fi

  printf '%s\n' "$resolved"
}

cleanup() {
  if [[ "${KEEP_MCPP_RUNTIME:-0}" == "1" ]]; then
    return
  fi
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
if [[ "${KEEP_MCPP_RUNTIME:-0}" != "1" ]]; then
  cleanup
fi

mkdir -p "$MCPP_HOME_DIR" "$XLINGS_HOME_DIR"
MCPP_BIN="$(resolve_mcpp_bin "$MCPP_BIN")"

log "Testing mcpp can build xlings with isolated homes..."
rm -rf "$ROOT_DIR/target/$MCPP_TARGET"
(
  cd "$ROOT_DIR"
  env -u XLINGS_PROJECT_DIR \
    MCPP_HOME="$MCPP_HOME_DIR" \
    XLINGS_HOME="$XLINGS_HOME_DIR" \
    "$MCPP_BIN" build --target "$MCPP_TARGET" --print-fingerprint --no-cache
)

XLINGS_MCPP_BIN="$(find "$ROOT_DIR/target/$MCPP_TARGET" -path '*/bin/xlings' -type f -executable | head -1)"
[[ -n "$XLINGS_MCPP_BIN" ]] || fail "mcpp build did not produce a target/*/bin/xlings binary"

HELP_OUTPUT_FILE="$RUNTIME_DIR/help.out"
env -u XLINGS_PROJECT_DIR XLINGS_HOME="$XLINGS_HOME_DIR" "$XLINGS_MCPP_BIN" -h > "$HELP_OUTPUT_FILE"
grep -aq "USAGE" "$HELP_OUTPUT_FILE" || fail "mcpp-built xlings binary did not print help"
grep -aq "xlings \\[OPTIONS\\]" "$HELP_OUTPUT_FILE" || fail "mcpp-built xlings help is missing command synopsis"

log "PASS: mcpp builds a runnable xlings binary"
