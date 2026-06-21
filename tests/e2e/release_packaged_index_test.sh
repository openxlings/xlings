#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/release_test_lib.sh"

ARCHIVE_PATH="${1:-$ROOT_DIR/build/release.tar.gz}"
require_release_archive "$ARCHIVE_PATH"

PKG_DIR="$(extract_release_archive "$ARCHIVE_PATH" release_packaged_index)"
INDEX_DIR="$PKG_DIR/data/xim-pkgindex"

[[ -d "$INDEX_DIR/pkgs" ]] || fail "release package missing bundled xim-pkgindex/pkgs"
[[ -f "$INDEX_DIR/pkgs/p/patchelf.lua" ]] || fail "bundled xim-pkgindex missing patchelf package"
# Y-asset: the bundled index is artifact-managed (no .git); the runtime refreshes
# it via `xlings update` from xlings-res/xim-index, not git pull.
[[ ! -d "$INDEX_DIR/.git" ]] || fail "bundled xim-pkgindex should be artifact-managed (no .git)"
[[ -f "$INDEX_DIR/.xlings-index-version" ]] || fail "bundled xim-pkgindex missing .xlings-index-version marker"

if [[ "$(uname -s)" == "Linux" ]]; then
  INSTALL_USER_DIR="$RUNTIME_ROOT/release_packaged_index_no_git_user"
  rm -rf "$INSTALL_USER_DIR"
  mkdir -p "$INSTALL_USER_DIR"

  INSTALL_LOG="$RUNTIME_ROOT/release_packaged_index_no_git_install.log"
  (
    unset XLINGS_HOME
    HOME="$INSTALL_USER_DIR" PATH="$PKG_DIR/bin" XLINGS_INSTALL_MIRROR=GLOBAL \
      "$PKG_DIR/bin/xlings" self install
  ) >"$INSTALL_LOG" 2>&1
  if grep -qE 'git is required|failed to sync repositories|package index not available, updating' "$INSTALL_LOG"; then
    cat "$INSTALL_LOG"
    fail "self install without system git tried to update/sync the package index"
  fi

  INSTALLED_HOME="$INSTALL_USER_DIR/.xlings"
  [[ -x "$INSTALLED_HOME/data/xpkgs/xim-x-patchelf/0.18.0/bin/patchelf" ]] || \
    fail "self install without system git did not install patchelf from bundled index"
  [[ -d "$INSTALLED_HOME/data/xim-pkgindex/pkgs" ]] || \
    fail "installed home missing bundled xim-pkgindex"
  installed_mirror="$(sed -n 's/.*"mirror"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$INSTALLED_HOME/.xlings.json" | head -1)"
  [[ "$installed_mirror" == "GLOBAL" ]] || \
    fail "installed home mirror should be GLOBAL, got $installed_mirror"
fi

log "PASS: release package includes usable xim-pkgindex snapshot"
