#!/usr/bin/env bash
# E2E test: verify shims are symlinks (not copies) on Unix after self init
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/shim_link"

source "$ROOT_DIR/tests/e2e/project_test_lib.sh"
BIN_SRC="$(find_xlings_bin)"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$BIN_SRC" ]] || fail "built xlings binary not found at $BIN_SRC"

# --- Setup portable home ---
rm -rf "$RUNTIME_DIR"
mkdir -p "$RUNTIME_DIR/portable/bin"

cp "$BIN_SRC" "$RUNTIME_DIR/portable/bin/xlings"

cat > "$RUNTIME_DIR/portable/.xlings.json" <<EOF
{
  "version": "0.4.0",
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "subos": { "default": { "dir": "" } }
}
EOF

pushd "$RUNTIME_DIR/portable" >/dev/null

# --- Run self init ---
env -u XLINGS_HOME ./bin/xlings self init >/dev/null 2>&1 || fail "self init failed"

# --- Verify base shims are symlinks ---
# 0.4.8 collapsed to a single canonical entry point (`xlings`) — earlier
# releases also created xim/xinstall/xsubos/xself shims, which were
# removed. The legacy aliases now print a migration error if invoked.
SHIM_DIR="$RUNTIME_DIR/portable/subos/default/bin"

for shim in xlings; do
  SHIM_PATH="$SHIM_DIR/$shim"
  [[ -e "$SHIM_PATH" ]] || fail "shim '$shim' does not exist"
  [[ -x "$SHIM_PATH" ]] || fail "shim '$shim' is not executable"
  [[ -L "$SHIM_PATH" ]] || fail "shim '$shim' is not a symlink (expected symlink, got regular file)"

  # Verify symlink is relative
  LINK_TARGET="$(readlink "$SHIM_PATH")"
  case "$LINK_TARGET" in
    /*) fail "shim '$shim' symlink is absolute: $LINK_TARGET (expected relative)" ;;
  esac

  # Verify symlink resolves to the real binary
  RESOLVED="$(cd "$SHIM_DIR" && realpath "$SHIM_PATH")"
  EXPECTED="$(realpath "$RUNTIME_DIR/portable/bin/xlings")"
  [[ "$RESOLVED" = "$EXPECTED" ]] || \
    fail "shim '$shim' resolves to '$RESOLVED', expected '$EXPECTED'"
done

# --- Verify legacy alias shims are NOT created ---
for legacy in xim xvm xinstall xsubos xself; do
  [[ ! -e "$SHIM_DIR/$legacy" ]] || \
    fail "legacy alias shim '$legacy' should NOT be created (removed in 0.4.8)"
done

# --- Verify shim works (can execute via symlink) ---
"$SHIM_DIR/xlings" -h >/dev/null 2>&1 || fail "shim xlings -h failed"

popd >/dev/null

# --- Test: install to user home, verify shims ---
INSTALL_USER_DIR="$RUNTIME_DIR/install_user"
mkdir -p "$INSTALL_USER_DIR"

pushd "$RUNTIME_DIR/portable" >/dev/null
HOME="$INSTALL_USER_DIR" PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin" \
  env -u XLINGS_HOME ./bin/xlings self install >/dev/null 2>&1 || fail "self install failed"
popd >/dev/null

INSTALLED_SHIM_DIR="$INSTALL_USER_DIR/.xlings/subos/default/bin"

for shim in xlings; do
  SHIM_PATH="$INSTALLED_SHIM_DIR/$shim"
  [[ -e "$SHIM_PATH" ]] || fail "installed shim '$shim' does not exist"
  [[ -L "$SHIM_PATH" ]] || fail "installed shim '$shim' is not a symlink"
done

for legacy in xim xvm xinstall xsubos xself; do
  [[ ! -e "$INSTALLED_SHIM_DIR/$legacy" ]] || \
    fail "installed legacy alias shim '$legacy' should NOT exist"
done

echo "PASS: all shims are relative symlinks and functional"
