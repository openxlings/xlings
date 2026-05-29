#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/release_test_lib.sh"

ARCHIVE_PATH="${1:-$ROOT_DIR/build/release.tar.gz}"
require_release_archive "$ARCHIVE_PATH"

PKG_DIR="$(extract_release_archive "$ARCHIVE_PATH" release_self_install)"
INSTALL_USER_DIR="$RUNTIME_ROOT/release_self_install_user"
rm -rf "$INSTALL_USER_DIR"
mkdir -p "$INSTALL_USER_DIR"

[[ -f "$PKG_DIR/.xlings.json" ]] || fail "bootstrap config missing in release package"
[[ -x "$PKG_DIR/bin/xlings" ]] || fail "bootstrap binary missing in release package"

HOME="$INSTALL_USER_DIR" \
PATH="$(minimal_system_path)" \
env -u XLINGS_HOME "$PKG_DIR/bin/xlings" self install

INSTALLED_HOME="$INSTALL_USER_DIR/.xlings"
[[ -x "$INSTALLED_HOME/bin/xlings" ]] || fail "installed home missing bin/xlings"
[[ -L "$INSTALLED_HOME/subos/current" ]] || fail "installed home missing subos/current link"
[[ -f "$INSTALLED_HOME/config/shell/xlings-profile.sh" ]] || fail "installed home missing shell profile"

# 0.4.8 collapsed to a single canonical entry point. The xim/xvm/xself/xsubos/
# xinstall shims were removed (see src/core/compact/xself.cppm).
[[ -x "$INSTALLED_HOME/subos/default/bin/xlings" ]] || fail "shim xlings missing after self install"
for legacy in xim xvm xsubos xself xinstall; do
  [[ ! -e "$INSTALLED_HOME/subos/default/bin/$legacy" ]] || \
    fail "legacy alias shim '$legacy' should NOT be created (removed in 0.4.8)"
done

INSTALLED_PATH="$INSTALLED_HOME/subos/current/bin:$INSTALLED_HOME/bin:$(minimal_system_path)"
HOME="$INSTALL_USER_DIR" PATH="$INSTALLED_PATH" env -u XLINGS_HOME "$INSTALLED_HOME/bin/xlings" -h >/dev/null
# Verify config command runs successfully (exit code check)
HOME="$INSTALL_USER_DIR" PATH="$INSTALLED_PATH" env -u XLINGS_HOME \
  "$INSTALLED_HOME/bin/xlings" --verbose config >/dev/null 2>&1 \
  || fail "installed home config command failed"

# Verify patchelf was installed as runtime dependency (Linux only)
if [[ "$(uname -s)" == "Linux" ]]; then
  [[ -x "$INSTALLED_HOME/data/xpkgs/xim-x-patchelf/0.18.0/bin/patchelf" ]] || \
    fail "patchelf binary not found after self install"
fi

log "PASS: release self install scenario"
