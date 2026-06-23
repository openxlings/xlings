#!/usr/bin/env bash
# Build a bootstrap xlings package for Linux x86_64.
#
# Directory layout:
#   xlings-<ver>-linux-x86_64/
#   ├── .xlings.json
#   ├── data/xim-pkgindex/        # bundled index snapshot
#   └── bin/
#       └── xlings
#
# Remaining runtime directories are created lazily by `xlings self init`.
#
# Output:  build/xlings-<ver>-linux-x86_64.tar.gz
# Usage:   ./tools/linux_release.sh
# Env:     SKIP_NETWORK_VERIFY=1   skip network-dependent tests
#          GIT_CONNECT_TIMEOUT=N   git TCP timeout seconds (default 30)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION=$(sed -n 's/.*VERSION = "\([^"]*\)".*/\1/p' "$PROJECT_DIR/src/core/config.cppm" | head -1)
[[ -z "$VERSION" ]] && VERSION="0.2.0"

# Cross-build aware: derive the package arch from the mcpp target triple, so the
# same script emits xlings-<ver>-linux-{x86_64,aarch64}.tar.gz. The aarch64
# release job sets MCPP_TARGET=aarch64-linux-musl + SKIP_RUN_VERIFY=1 (the cross
# ELF can't execute on the x86_64 builder).
MCPP_TARGET="${MCPP_TARGET:-x86_64-linux-musl}"
ARCH="${MCPP_TARGET%%-*}"   # x86_64 / aarch64
PKG_NAME="xlings-${VERSION}-linux-${ARCH}"
OUT_DIR="$PROJECT_DIR/build/$PKG_NAME"

TEST_DATA=""
cleanup() {
  [[ -n "$TEST_DATA" && -d "$TEST_DATA" ]] && rm -rf "$TEST_DATA"
  return 0  # never fail the script (TEST_DATA is empty on SKIP_RUN_VERIFY cross builds)
}
trap cleanup EXIT

info()  { echo "[release] $*"; }
fail()  { echo "[release] FAIL: $*" >&2; exit 1; }

cd "$PROJECT_DIR"

# ── 1. Build C++ ─────────────────────────────────────────────────
info "Version: $VERSION  |  Arch: $ARCH"
info "Building C++ binary..."
MCPP_BIN="${MCPP_BIN:-mcpp}"
MCPP_TARGET="${MCPP_TARGET:-x86_64-linux-musl}"
command -v "$MCPP_BIN" >/dev/null 2>&1 || fail "mcpp not found; run xlings install first"

rm -rf "$PROJECT_DIR/target/$MCPP_TARGET"
"$MCPP_BIN" build --target "$MCPP_TARGET" --print-fingerprint --no-cache 2>&1 || fail "mcpp build failed"

BIN_SRC="$(find "$PROJECT_DIR/target/$MCPP_TARGET" -path '*/bin/xlings' -type f -perm -111 | sort | tail -1)"
[[ -f "$BIN_SRC" ]] || fail "C++ binary not found at $BIN_SRC"

if command -v file &>/dev/null; then
  file "$BIN_SRC" | grep -qi "statically linked" || fail "binary is not statically linked"
elif command -v ldd &>/dev/null; then
  ldd "$BIN_SRC" 2>&1 | grep -Eq "not a dynamic executable|statically linked" || fail "binary is not statically linked"
fi
info "OK: binary is fully static"

# ── 2. Assemble package ─────────────────────────────────────────
info "Assembling $OUT_DIR ..."
rm -rf "$OUT_DIR"

mkdir -p "$OUT_DIR/bin"

cp "$BIN_SRC"         "$OUT_DIR/bin/xlings"
chmod +x "$OUT_DIR/bin/"*

# .xlings.json — release packages default to GLOBAL. Users can switch mirror
# locally after install, and CI also keeps GLOBAL for github.com endpoints.
MIRROR="${XLINGS_RELEASE_MIRROR:-GLOBAL}"
if [[ "$MIRROR" == "CN" ]]; then
  RES_SERVER_URL="https://gitcode.com/xlings-res"
  INDEX_REPO_URL="https://gitee.com/sunrisepeak/xim-pkgindex.git"
  REPO_URL="https://gitee.com/sunrisepeak/xlings.git"
else
  RES_SERVER_URL="https://github.com/xlings-res"
  INDEX_REPO_URL="https://github.com/openxlings/xim-pkgindex.git"
  REPO_URL="https://github.com/openxlings/xlings.git"
fi
if command -v jq &>/dev/null && [[ -f config/xlings.json ]]; then
  jq --arg version "$VERSION" --arg mirror "$MIRROR" \
     --arg res_server "$RES_SERVER_URL" --arg index_repo "$INDEX_REPO_URL" \
     --arg repo "$REPO_URL" \
     '. + {"version":$version,"mirror":$mirror,"activeSubos":"default","subos":{"default":{"dir":""}}}
      | .xim["res-server"] = $res_server
      | .xim["index-repo"] = $index_repo
      | .repo = $repo' \
    config/xlings.json > "$OUT_DIR/.xlings.json"
else
  cat > "$OUT_DIR/.xlings.json" << DOTJSON
{"activeSubos":"default","subos":{"default":{"dir":""}},"version":"$VERSION","need_update":false,"mirror":"$MIRROR","xim":{"mirrors":{"index-repo":{"GLOBAL":"https://github.com/openxlings/xim-pkgindex.git","CN":"https://gitee.com/sunrisepeak/xim-pkgindex.git"},"res-server":{"GLOBAL":"https://github.com/xlings-res","CN":"https://gitcode.com/xlings-res"}},"res-server":"$RES_SERVER_URL","index-repo":"$INDEX_REPO_URL"},"repo":"$REPO_URL"}
DOTJSON
fi

bash "$PROJECT_DIR/tools/package_xim_index.sh" "$OUT_DIR"

info "Package assembled: $OUT_DIR"

# ── 4. Verification ─────────────────────────────────────────────
info "=== Verification ==="

# 4a. Check bootstrap files exist
for f in bin/xlings; do
  [[ -x "$OUT_DIR/$f" ]] || fail "$f is missing or not executable"
done
info "OK: all binaries present and executable"

# 4b. Check .xlings.json
[[ -f "$OUT_DIR/.xlings.json" ]] || fail ".xlings.json missing"
if command -v jq &>/dev/null; then
  AS=$(jq -r '.activeSubos' "$OUT_DIR/.xlings.json" 2>/dev/null)
  [[ "$AS" == "default" ]] || fail ".xlings.json activeSubos != 'default' (got '$AS')"
fi
info "OK: .xlings.json present and valid"

[[ -d "$OUT_DIR/data/xim-pkgindex/pkgs" ]] || fail "bundled xim-pkgindex missing"
[[ -f "$OUT_DIR/data/xim-pkgindex/pkgs/p/patchelf.lua" ]] || fail "bundled xim-pkgindex missing patchelf"
info "OK: bundled xim-pkgindex snapshot present"

# 4c. Functional tests (bootstrap home detection). Skipped on cross builds —
# the aarch64 ELF can't execute on the x86_64 builder; the user's device runs
# self-init via `xlings self install` (from quick_install.sh) on first use.
if [[ -n "${SKIP_RUN_VERIFY:-}" ]]; then
  info "Skip: functional run tests (SKIP_RUN_VERIFY set — cross build)"
else
info "Testing bootstrap home execution..."
TEST_DATA="$PROJECT_DIR/build/.release_verify_$$"
mkdir -p "$TEST_DATA"

export XLINGS_HOME="$OUT_DIR"
export PATH="$OUT_DIR/bin:$PATH"

set +e
HELP_OUT=$("$OUT_DIR/bin/xlings" -h 2>&1)
HELP_RC=$?
set -e
if [[ $HELP_RC -ne 0 ]]; then
  info "xlings -h returned exit code $HELP_RC"
  info "xlings -h output: $HELP_OUT"
  fail "xlings -h failed with exit code $HELP_RC"
fi
echo "$HELP_OUT" | grep -q "subos" || fail "xlings -h missing 'subos' command"
info "OK: xlings -h shows subos/self commands"

CONFIG_OUT=$("$OUT_DIR/bin/xlings" config 2>&1) || fail "xlings config failed"
echo "$CONFIG_OUT" | grep -q "XLINGS_HOME" || fail "config output missing XLINGS_HOME"
info "OK: xlings config prints correct paths"

INIT_OUT=$("$OUT_DIR/bin/xlings" self init 2>&1) || fail "xlings self init failed"
echo "$INIT_OUT" | grep -q "init ok" || fail "self init output missing success marker"
for d in data/xpkgs data/runtimedir data/xim-index-repos data/local-indexrepo subos/default/bin subos/default/lib subos/default/usr subos/default/generations config/shell; do
  [[ -d "$OUT_DIR/$d" ]] || fail "directory $d missing after self init"
done
[[ -L "$OUT_DIR/subos/current" ]] || fail "subos/current symlink missing after self init"
[[ "$(readlink "$OUT_DIR/subos/current")" == "$OUT_DIR/subos/default" || "$(readlink "$OUT_DIR/subos/current")" == "default" ]] || fail "subos/current does not point to default after self init"
[[ -x "$OUT_DIR/subos/default/bin/xlings" ]] || fail "subos/default/bin/xlings missing after self init"
info "OK: self init materialized bootstrap home"
fi  # end SKIP_RUN_VERIFY gate

export XLINGS_DATA="$OUT_DIR/data"
export XLINGS_SUBOS="$OUT_DIR/subos/current"
export PATH="$OUT_DIR/subos/current/bin:$OUT_DIR/bin:$PATH"

# 4e. Network-dependent tests
if [[ "${SKIP_NETWORK_VERIFY:-}" == "1" ]]; then
  info "Skip: network-dependent tests (SKIP_NETWORK_VERIFY=1)"
else
  export GIT_TERMINAL_PROMPT=0
  export GIT_CONNECT_TIMEOUT="${GIT_CONNECT_TIMEOUT:-30}"

  run_with_timeout() {
    local t="$1"; shift
    if command -v timeout &>/dev/null; then timeout "$t" "$@"; else "$@"; fi
  }

  info "Verify: xlings update (timeout 300s)..."
  if ! run_with_timeout 300 bash -c \
    'PATH="$1/subos/current/bin:$1/bin:/usr/local/bin:/usr/bin:/bin" "$1/bin/xlings" update' _ "$OUT_DIR"; then
    fail "xlings update failed (network?). Set SKIP_NETWORK_VERIFY=1 to skip."
  fi

  info "Verify: xlings install d2x@0.1.3 -y (timeout 300s)..."
  if ! run_with_timeout 300 bash -c \
    'PATH="$1/subos/current/bin:$1/bin:/usr/local/bin:/usr/bin:/bin" "$1/bin/xlings" install d2x@0.1.3 -y' _ "$OUT_DIR"; then
    fail "install d2x@0.1.3 failed. Set SKIP_NETWORK_VERIFY=1 to skip."
  fi

  XPKG_D2X="$XLINGS_DATA/xpkgs/d2x"
  if [[ -d "$XPKG_D2X" ]] && compgen -G "$XPKG_D2X/*" > /dev/null 2>&1; then
    info "OK: data/xpkgs/d2x installed successfully"
  else
    fail "data/xpkgs/d2x not found after install"
  fi
fi

cleanup
trap - EXIT

# ── 5. Create archive ───────────────────────────────────────────
info ""
info "All checks passed. Creating release archive..."

ARCHIVE="$PROJECT_DIR/build/${PKG_NAME}.tar.gz"
tar -czf "$ARCHIVE" -C "$PROJECT_DIR/build" "$PKG_NAME"

info ""
info "Done."
info "  Package:  $OUT_DIR"
info "  Archive:  $ARCHIVE"
info ""
info "  Unpack & install:"
info "    tar -xzf ${PKG_NAME}.tar.gz"
info "    cd $PKG_NAME"
info "    ./bin/xlings self install"
info ""
info "  Or use without installing:"
info "    ./bin/xlings self init"
info "    export PATH=\"\$(pwd)/subos/current/bin:\$(pwd)/bin:\$PATH\""
info "    xlings config"
info ""
