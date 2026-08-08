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

# mcpp's first-time sandbox bootstrap downloads patchelf + ninja from the
# network. On a fresh CI runner (cold cache) that fetch occasionally flakes
# (transient 5xx/404 / connection reset), leaving the sandbox half-initialized
# ("failed to bootstrap patchelf ... ; patchelf missing") and failing the whole
# E2E suite. Retry the build a few times, wiping the isolated mcpp sandbox
# between attempts so the bootstrap re-runs from a clean slate. We retry ONLY
# transient bootstrap failures; real build/compile errors are surfaced
# immediately instead of being masked (and paid for 3x).
MCPP_BUILD_ATTEMPTS="${MCPP_BUILD_ATTEMPTS:-3}"
BUILD_LOG="$RUNTIME_DIR/mcpp_build.out"

run_mcpp_build() {
  rm -rf "$ROOT_DIR/target/$MCPP_TARGET" "$MCPP_HOME_DIR"
  mkdir -p "$MCPP_HOME_DIR"
  (
    cd "$ROOT_DIR"
    env -u XLINGS_PROJECT_DIR \
      MCPP_HOME="$MCPP_HOME_DIR" \
      XLINGS_HOME="$XLINGS_HOME_DIR" \
      "$MCPP_BIN" build --target "$MCPP_TARGET" --print-fingerprint --no-cache
  )
}

attempt=1
while true; do
  status=0
  run_mcpp_build 2>&1 | tee "$BUILD_LOG" || status="${PIPESTATUS[0]}"
  [[ "$status" -eq 0 ]] && break

  if ! grep -qE 'failed to bootstrap (patchelf|ninja)|patchelf missing|ninja missing' "$BUILD_LOG"; then
    fail "mcpp build failed (exit $status) — not a transient sandbox-bootstrap flake"
  fi
  if [[ "$attempt" -ge "$MCPP_BUILD_ATTEMPTS" ]]; then
    fail "mcpp sandbox bootstrap kept failing after $MCPP_BUILD_ATTEMPTS attempts"
  fi
  log "mcpp build attempt $attempt hit a transient sandbox-bootstrap flake; resetting sandbox and retrying ($((attempt + 1))/$MCPP_BUILD_ATTEMPTS)..."
  attempt=$((attempt + 1))
  sleep 5
done

# Newest, not first. The path is target/<triple>/<fingerprint>/bin/xlings and
# the fingerprint changes with any build input (toolchain, mcpp version,
# .xlings.json), so several can coexist. `head -1` takes whatever find happens
# to walk first, which means this test can assert that a binary built BEFORE
# the change under test still runs -- passing while proving nothing.
XLINGS_MCPP_BIN="$(find "$ROOT_DIR/target/$MCPP_TARGET" -path '*/bin/xlings' -type f -executable \
  -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)"
[[ -n "$XLINGS_MCPP_BIN" ]] || fail "mcpp build did not produce a target/*/bin/xlings binary"
log "mcpp-built binary: $XLINGS_MCPP_BIN"

HELP_OUTPUT_FILE="$RUNTIME_DIR/help.out"
env -u XLINGS_PROJECT_DIR XLINGS_HOME="$XLINGS_HOME_DIR" "$XLINGS_MCPP_BIN" -h > "$HELP_OUTPUT_FILE"
grep -aq "USAGE" "$HELP_OUTPUT_FILE" || fail "mcpp-built xlings binary did not print help"
grep -aq "xlings \\[OPTIONS\\]" "$HELP_OUTPUT_FILE" || fail "mcpp-built xlings help is missing command synopsis"

log "PASS: mcpp builds a runnable xlings binary"
