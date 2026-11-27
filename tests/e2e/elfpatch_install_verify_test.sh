#!/usr/bin/env bash
set -euo pipefail

# E2E test: elfpatch correctness after installing a dynamically-linked package.
#
# Installs d2x into a custom XLINGS_HOME, then verifies:
#   1. PT_INTERP points to subos ld-linux
#   2. RUNPATH/RPATH contains dependency lib dirs
#   3. Binary actually runs

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/release_test_lib.sh"

ARCHIVE_PATH="${1:-$ROOT_DIR/build/release.tar.gz}"
require_release_archive "$ARCHIVE_PATH"
require_fixture_index

PKG_DIR="$(extract_release_archive "$ARCHIVE_PATH" elfpatch_verify)"
write_fixture_release_config "$PKG_DIR"

export XLINGS_HOME="$PKG_DIR"
export PATH="$XLINGS_HOME/bin:$(minimal_system_path)"

xlings --verbose self init
export PATH="$XLINGS_HOME/subos/current/bin:$XLINGS_HOME/bin:$(minimal_system_path)"
xlings --verbose update

# Install d2x (dynamically linked on Linux)
D2X_VERSION="0.1.3"
log "installing d2x@$D2X_VERSION ..."
INSTALL_OUT="$(xlings --verbose install "d2x@$D2X_VERSION" -y 2>&1 | strip_ansi)" || true
echo "$INSTALL_OUT"

# Locate binary
D2X_BIN="$XLINGS_HOME/data/xpkgs/xim-x-d2x/$D2X_VERSION/d2x"
[[ -f "$D2X_BIN" ]] || fail "d2x binary not found at $D2X_BIN"

# Verify it's dynamically linked
file "$D2X_BIN" | grep -q "dynamically linked" || fail "d2x is not dynamically linked"

# ── Test 1: elfpatch result in install output ────────────────────────
log "Test 1: elfpatch reported success"
# libxpkg's structured result (design 2026-11-27 §9.3): `done` with d2x's ELF
# patched; a relocation that fails is a failed install, not a count.
if echo "$INSTALL_OUT" | grep -qE "d2x: elfpatch: done \([0-9]+ scanned, [1-9][0-9]* patched"; then
    ELFPATCH_LINE="$(echo "$INSTALL_OUT" | grep "d2x: elfpatch: done" | head -1)"
    log "  $ELFPATCH_LINE"
else
    echo "$INSTALL_OUT" | grep "elfpatch" || true
    fail "elfpatch did not report d2x relocated"
fi

# ── Test 2: interpreter points to local ld-linux (xpkgs or subos) ─────
log "Test 2: PT_INTERP points to XLINGS_HOME"
INTERP="$(readelf -l "$D2X_BIN" 2>&1 | grep "Requesting program interpreter:" | sed 's/.*: //; s/]//')"
log "  interpreter: $INTERP"

[[ -n "$INTERP" ]] || fail "no interpreter found in d2x binary"
echo "$INTERP" | grep -q "$XLINGS_HOME/" || fail "interpreter '$INTERP' does not point to XLINGS_HOME"
[[ -f "$INTERP" ]] || fail "interpreter file does not exist: $INTERP"

# ── Test 3: RUNPATH/RPATH contains dependency lib paths ──────────────
log "Test 3: RUNPATH/RPATH contains dependency libs"
RPATH_OUT="$(readelf -d "$D2X_BIN" 2>&1)"
RPATH_LINE="$(echo "$RPATH_OUT" | grep -E "RUNPATH|RPATH" | head -1)" || true
log "  $RPATH_LINE"

[[ -n "$RPATH_LINE" ]] || fail "no RPATH/RUNPATH found"
echo "$RPATH_LINE" | grep -q "glibc" || fail "RPATH does not contain glibc lib path"

# ── Test 4: binary actually runs ─────────────────────────────────────
log "Test 4: d2x runs via subos interpreter + rpath"
unset LD_LIBRARY_PATH 2>/dev/null || true
D2X_RUN="$("$D2X_BIN" --version 2>&1)" || fail "d2x --version failed — runtime resolution broken"
log "  output: $D2X_RUN"

log "PASS: elfpatch install verification"
