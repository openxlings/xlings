#!/usr/bin/env bash
# Build a bootstrap xlings package for macOS arm64.
#
# Directory layout:
#   xlings-<ver>-macosx-arm64/
#   ├── .xlings.json
#   ├── data/xim-pkgindex/        # bundled index snapshot
#   └── bin/
#       └── xlings
#
# Remaining runtime directories are created lazily by `xlings self init`.
#
# Output:  build/xlings-<ver>-macosx-arm64.tar.gz
# Usage:   ./tools/macos_release.sh
# Env:     SKIP_NETWORK_VERIFY=1   skip network-dependent tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION=$(sed -n 's/.*VERSION = "\([^"]*\)".*/\1/p' "$PROJECT_DIR/src/core/config.cppm" | head -1)
[[ -z "$VERSION" ]] && VERSION="0.2.0"

ARCH="arm64"
PKG_NAME="xlings-${VERSION}-macosx-${ARCH}"
OUT_DIR="$PROJECT_DIR/build/$PKG_NAME"

TEST_DATA=""
cleanup() {
  [[ -n "$TEST_DATA" && -d "$TEST_DATA" ]] && rm -rf "$TEST_DATA"
}
trap cleanup EXIT

info()  { echo "[release] $*"; }
fail()  { echo "[release] FAIL: $*" >&2; exit 1; }

cd "$PROJECT_DIR"

# ── 1. Build C++ ─────────────────────────────────────────────────
info "Version: $VERSION  |  Arch: $ARCH"
info "Building C++ binary..."
LLVM_PREFIX_DEFAULT="${LLVM_PREFIX:-}"
if [[ -n "$LLVM_PREFIX_DEFAULT" && -x "$LLVM_PREFIX_DEFAULT/bin/clang++" ]]; then
  export LLVM_PREFIX="$LLVM_PREFIX_DEFAULT"
  export SDKROOT="${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}"
  export PATH="$LLVM_PREFIX/bin:$PATH"
  xmake f -c -p macosx -m release --toolchain=llvm --sdk="$LLVM_PREFIX" --target_minver=11.0 -y \
    || fail "xmake configure with llvm failed"
fi
xmake clean -q 2>/dev/null || true
xmake build -y xlings 2>&1 || fail "xmake build failed"

BIN_SRC="build/macosx/${ARCH}/release/xlings"
[[ -f "$BIN_SRC" ]] || fail "C++ binary not found at $BIN_SRC"

info "Verifying no LLVM toolchain dependency..."
if otool -L "$BIN_SRC" | grep -q "llvm"; then
    otool -L "$BIN_SRC"
    fail "binary still links against LLVM runtime dylibs"
else
    info "OK: binary has no LLVM runtime dependency"
fi

# ── 2. Assemble package ─────────────────────────────────────────
info "Assembling $OUT_DIR ..."
rm -rf "$OUT_DIR"

mkdir -p "$OUT_DIR/bin"

cp "$BIN_SRC"          "$OUT_DIR/bin/xlings"
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

for f in bin/xlings; do
  [[ -x "$OUT_DIR/$f" ]] || fail "$f is missing or not executable"
done
info "OK: all binaries present and executable"

OTOOL_OUT="$(otool -L "$OUT_DIR/bin/xlings")"
echo "$OTOOL_OUT" | grep -q "llvm" && fail "packaged bin/xlings still links against LLVM runtime dylibs"
info "OK: packaged bin/xlings has no LLVM runtime dependency"

[[ -f "$OUT_DIR/.xlings.json" ]] || fail ".xlings.json missing"
info "OK: .xlings.json present"

[[ -d "$OUT_DIR/data/xim-pkgindex/pkgs" ]] || fail "bundled xim-pkgindex missing"
[[ -f "$OUT_DIR/data/xim-pkgindex/pkgs/p/patchelf.lua" ]] || fail "bundled xim-pkgindex missing patchelf"
info "OK: bundled xim-pkgindex snapshot present"

TEST_DATA="$PROJECT_DIR/build/.release_verify_$$"
mkdir -p "$TEST_DATA"

export XLINGS_HOME="$OUT_DIR"
export PATH="$OUT_DIR/bin:$PATH"

HELP_OUT=$("$OUT_DIR/bin/xlings" -h 2>&1)
echo "$HELP_OUT" | grep -q "subos" || { echo "[release] xlings -h output: $HELP_OUT"; fail "xlings -h missing 'subos' command"; }
info "OK: xlings -h shows subos/self commands"

CONFIG_OUT=$("$OUT_DIR/bin/xlings" config 2>&1)
echo "$CONFIG_OUT" | grep -q "XLINGS_HOME" || fail "config output missing XLINGS_HOME"
info "OK: xlings config prints correct paths"

INIT_OUT=$("$OUT_DIR/bin/xlings" self init 2>&1) || fail "xlings self init failed"
echo "$INIT_OUT" | grep -q "init ok" || fail "self init output missing success marker"
for d in data/xpkgs data/runtimedir data/xim-index-repos data/local-indexrepo subos/default/bin subos/default/lib subos/default/usr subos/default/generations config/shell; do
  [[ -d "$OUT_DIR/$d" ]] || fail "directory $d missing after self init"
done
[[ -L "$OUT_DIR/subos/current" ]] || fail "subos/current symlink missing after self init"
[[ -x "$OUT_DIR/subos/default/bin/xlings" ]] || fail "subos/default/bin/xlings missing after self init"
[[ -L "$OUT_DIR/subos/default/bin/xlings" ]] || fail "subos/default/bin/xlings should be a symlink on macOS"
info "OK: self init materialized bootstrap home"

export XLINGS_DATA="$OUT_DIR/data"
export XLINGS_SUBOS="$OUT_DIR/subos/current"
export PATH="$OUT_DIR/subos/current/bin:$OUT_DIR/bin:$PATH"

SHIM_HELP_OUT=$("$OUT_DIR/subos/current/bin/xlings" -h 2>&1) || fail "subos/current/bin/xlings -h failed"
echo "$SHIM_HELP_OUT" | grep -q "subos" || fail "shim xlings help output missing subos command"
info "OK: symlink shim dispatch works with bundled runtime"

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

  info "Verify: xlings install d2x -y (timeout 300s)..."
  if ! run_with_timeout 300 bash -c \
    'PATH="$1/subos/current/bin:$1/bin:/usr/local/bin:/usr/bin:/bin" "$1/bin/xlings" install d2x -y' _ "$OUT_DIR"; then
    fail "install d2x failed. Set SKIP_NETWORK_VERIFY=1 to skip."
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
