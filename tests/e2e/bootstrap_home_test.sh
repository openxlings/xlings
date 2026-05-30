#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/bootstrap_home"
PORTABLE_DIR="$RUNTIME_DIR/portable"
MOVED_DIR="$RUNTIME_DIR/portable_moved"
INSTALL_USER_DIR="$RUNTIME_DIR/install_user"
FIXTURE_INDEX_DIR="$ROOT_DIR/tests/fixtures/xim-pkgindex"

source "$ROOT_DIR/tests/e2e/project_test_lib.sh"
BIN_SRC="$(find_xlings_bin)"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

strip_ansi() {
  perl -pe 's/\e\[[0-9;]*[a-zA-Z]//g; s/\e\[\?[0-9]*[a-zA-Z]//g'
}

[[ -x "$BIN_SRC" ]] || fail "built xlings binary not found at $BIN_SRC"
"$ROOT_DIR/tests/e2e/prepare_fixture_index.sh" "$FIXTURE_INDEX_DIR"
[[ -d "$FIXTURE_INDEX_DIR/pkgs" ]] || fail "fixture index repo missing at $FIXTURE_INDEX_DIR"

rm -rf "$RUNTIME_DIR"
mkdir -p "$PORTABLE_DIR/bin" "$INSTALL_USER_DIR"

cp "$BIN_SRC" "$PORTABLE_DIR/bin/xlings"

cat > "$PORTABLE_DIR/.xlings.json" <<EOF
{
  "version": "0.4.0",
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "subos": { "default": { "dir": "" } },
  "index_repos": [
    { "name": "projectrepo", "url": "$FIXTURE_INDEX_DIR" }
  ]
}
EOF

pushd "$PORTABLE_DIR" >/dev/null

env -u XLINGS_HOME ./bin/xlings --verbose config 2>&1 || true
# Verify portable home was auto-detected by checking data dirs exist after init


env -u XLINGS_HOME ./bin/xlings --verbose self init >/tmp/xlings-bootstrap-init.log 2>&1 || {
  cat /tmp/xlings-bootstrap-init.log
  fail "self init failed in portable home"
}

for dir in \
  data/xpkgs \
  data/runtimedir \
  data/xim-index-repos \
  data/local-indexrepo \
  subos/default/bin \
  subos/default/lib \
  subos/default/usr \
  subos/default/generations \
  config/shell
do
  [[ -d "$PORTABLE_DIR/$dir" ]] || fail "missing portable runtime dir: $dir"
done

[[ -L "$PORTABLE_DIR/subos/current" ]] || fail "portable subos/current link missing"
[[ -x "$PORTABLE_DIR/subos/default/bin/xlings" ]] || fail "portable builtin shim missing"

popd >/dev/null

mv "$PORTABLE_DIR" "$MOVED_DIR"

pushd "$MOVED_DIR" >/dev/null

env -u XLINGS_HOME ./bin/xlings -h >/dev/null 2>&1 || fail "portable home failed after move"

env -u XLINGS_HOME ./bin/xlings --verbose update >/tmp/xlings-bootstrap-update.log 2>&1 || {
  cat /tmp/xlings-bootstrap-update.log
  fail "portable update failed after move"
}

HOME="$INSTALL_USER_DIR" PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin" \
  env -u XLINGS_HOME ./bin/xlings --verbose self install >/tmp/xlings-bootstrap-install.log 2>&1 || {
    cat /tmp/xlings-bootstrap-install.log
    fail "self install from portable home failed"
  }

popd >/dev/null

INSTALLED_HOME="$INSTALL_USER_DIR/.xlings"
[[ -x "$INSTALLED_HOME/bin/xlings" ]] || fail "installed home missing bin/xlings"
[[ -x "$INSTALLED_HOME/subos/default/bin/xlings" ]] || fail "installed home missing builtin shim"
[[ -f "$INSTALLED_HOME/config/shell/xlings-profile.sh" ]] || fail "installed home missing shell profile"

# Verify installed home works by running config (exit code check only)
cd "$INSTALL_USER_DIR" && HOME="$INSTALL_USER_DIR" PATH="$INSTALLED_HOME/subos/current/bin:$INSTALLED_HOME/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin" env -u XLINGS_HOME "$INSTALLED_HOME/bin/xlings" --verbose config >/dev/null 2>&1 \
  || fail "installed home config command failed"

echo "PASS: bootstrap home works for portable and installed modes"
