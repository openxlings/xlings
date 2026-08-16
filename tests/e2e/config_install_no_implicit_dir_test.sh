#!/usr/bin/env bash
# E2E test: type=config packages that do not create install_dir themselves
# must not be materialized by xlings.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/config_install_no_implicit_dir"
HOME_DIR="$RUNTIME_DIR/home"
WORK_DIR="$RUNTIME_DIR/work"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

mkdir -p "$HOME_DIR/subos/default/bin" \
         "$WORK_DIR" \
         "$LOCAL_INDEX_DIR/pkgs/c"

cat > "$LOCAL_INDEX_DIR/pkgs/c/config-no-payload.lua" <<'LUA'
package = {
    spec = "1",
    name = "config-no-payload",
    description = "Test fixture: config install hook does not create install_dir",
    type = "config",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.system")

function install()
    local marker = path.join(system.rundir(), "install-count.txt")
    local count = 0
    if os.isfile(marker) then
        count = tonumber(io.readfile(marker)) or 0
    end
    io.writefile(marker, tostring(count + 1))
    return true
end

function config()
    local marker = path.join(system.rundir(), "config-count.txt")
    local count = 0
    if os.isfile(marker) then
        count = tonumber(io.readfile(marker)) or 0
    end
    io.writefile(marker, tostring(count + 1))
    return true
end

function uninstall()
    local marker = os.getenv("XLINGS_TEST_UNINSTALL_COUNTER")
    local count = 0
    if os.isfile(marker) then
        count = tonumber(io.readfile(marker)) or 0
    end
    io.writefile(marker, tostring(count + 1))
    return true
end
LUA

# A second fixture, for a distinction the first one alone cannot pin down:
# "does not exist" vs "exists, but has no build for THIS platform". Reporting
# the second as the first sends the reader looking for a package that is
# sitting in the index they just synced.
#
# Both directions matter, and each alone is satisfiable by a wrong
# implementation:
#   * config-no-payload@9.9.9 -- a version nobody has. Must still say "not
#     found", and must NOT name platforms, which would state the opposite of
#     the truth about a package that is fine here. (An implementation that
#     ignored the version reported "no build for linux (available on: linux,
#     macosx, windows)" -- naming the platform it called unsupported.)
#   * config-other-platform   -- built only elsewhere. Must say so, and say
#     where.
cat > "$LOCAL_INDEX_DIR/pkgs/c/config-other-platform.lua" <<'LUA'
package = {
    spec = "1",
    name = "config-other-platform",
    description = "Test fixture: exists, but not for the platform asking",
    type = "config",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        -- Deliberately not the host. Linux is where CI runs this; the macOS
        -- leg drops its own row below so the fixture keeps its meaning there.
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

function install() return true end
function config()  return true end
LUA
if [[ "$(uname -s)" == "Darwin" ]]; then
  sed -i.bak '/macosx  = /d' "$LOCAL_INDEX_DIR/pkgs/c/config-other-platform.lua"
  rm -f "$LOCAL_INDEX_DIR/pkgs/c/config-other-platform.lua.bak"
fi

printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"

cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "version": "0.4.39",
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "subos": {"default": {"dir": ""}},
  "index_repos": [
    {"name": "xim", "url": "$LOCAL_INDEX_DIR"}
  ]
}
JSON

XLINGS_BIN="$(find_xlings_bin)"
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"

RUN() {
  ( cd "$WORK_DIR" && env -i HOME="$HOME_DIR" USER=xlings-ci \
      SHELL=/bin/sh PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      XLINGS_TEST_UNINSTALL_COUNTER="$WORK_DIR/uninstall-count.txt" \
      "$XLINGS_BIN" --verbose "$@" )
}

RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

log "Installing config-no-payload twice..."
RUN install config-no-payload -y >/dev/null 2>&1 \
  || fail "first install failed"
RUN install config-no-payload -y >/dev/null 2>&1 \
  || fail "second install failed"

COUNT_FILE="$WORK_DIR/install-count.txt"
[[ -f "$COUNT_FILE" ]] \
  || fail "install hook did not write $COUNT_FILE"

COUNT="$(cat "$COUNT_FILE")"
[[ "$COUNT" == "2" ]] \
  || fail "install hook should run twice when it does not create install_dir; count=$COUNT"

CONFIG_COUNT_FILE="$WORK_DIR/config-count.txt"
[[ -f "$CONFIG_COUNT_FILE" ]] \
  || fail "config hook did not write $CONFIG_COUNT_FILE"

CONFIG_COUNT="$(cat "$CONFIG_COUNT_FILE")"
[[ "$CONFIG_COUNT" == "2" ]] \
  || fail "config hook should run twice without materializing install_dir; count=$CONFIG_COUNT"

INSTALL_DIR="$HOME_DIR/data/xpkgs/xim-x-config-no-payload/1.0.0"
if [[ -e "$INSTALL_DIR" ]]; then
  [[ -d "$INSTALL_DIR" ]] \
    || fail "config package install_dir exists but is not a directory: $INSTALL_DIR"
  [[ -z "$(find "$INSTALL_DIR" -mindepth 1 -print -quit)" ]] \
    || fail "xlings implicitly materialized config package install_dir:
$(ls -la "$INSTALL_DIR")"
fi

UNINSTALL_COUNT_FILE="$WORK_DIR/uninstall-count.txt"
rm -f "$UNINSTALL_COUNT_FILE"

set +e
REMOVE_OUT="$(RUN remove config-no-payload@1.0.0 -y 2>&1)"
REMOVE_RC=$?
set -e

UNINSTALL_COUNT=0
if [[ -f "$UNINSTALL_COUNT_FILE" ]]; then
  UNINSTALL_COUNT="$(cat "$UNINSTALL_COUNT_FILE")"
fi

set +e
MISSING_OUT="$(RUN remove config-no-payload@9.9.9 -y 2>&1)"
MISSING_RC=$?

# The other half of the same contract. `remove <name>` means "make sure it is
# gone", and scripts re-run it defensively; a name the index has never heard of
# is as gone as it gets. `remove <name>@<version>` names something specific, so
# an unresolvable coordinate is a wrong coordinate. Same command, two questions.
set +e
GONE_OUT="$(RUN remove no-such-package-anywhere -y 2>&1)"
GONE_RC=$?
set -e
set -e

UNINSTALL_COUNT_AFTER_MISSING=0
if [[ -f "$UNINSTALL_COUNT_FILE" ]]; then
  UNINSTALL_COUNT_AFTER_MISSING="$(cat "$UNINSTALL_COUNT_FILE")"
fi

OTHER_RC=0
OTHER_OUT="$(RUN info config-other-platform 2>&1)" || OTHER_RC=$?

FAILURES=()
[[ "$OTHER_RC" -ne 0 ]] \
  || FAILURES+=("a package with no build for this platform exited 0: $OTHER_OUT")
grep -qi "no build for" <<<"$OTHER_OUT" \
  || FAILURES+=("another-platform package was not reported as such: $OTHER_OUT")
grep -qi "windows" <<<"$OTHER_OUT" \
  || FAILURES+=("another-platform package did not name where it IS available: $OTHER_OUT")
# `if`, not `grep ... && FAILURES+=(...)`. Under `set -e` that compound
# returns grep's status, so the PASSING case -- grep finding nothing -- would
# kill the script with no output at all. Same shape the four-defect doc
# recorded in §9.3.
if grep -qiE "package .* not found" <<<"$OTHER_OUT"; then
  FAILURES+=("another-platform package was reported as 'not found': $OTHER_OUT")
fi

[[ "$REMOVE_RC" -eq 0 ]] \
  || FAILURES+=("payloadless config remove exited $REMOVE_RC: $REMOVE_OUT")
[[ "$UNINSTALL_COUNT" == "1" ]] \
  || FAILURES+=("payloadless config uninstall hook count should be 1, got $UNINSTALL_COUNT: $REMOVE_OUT")
[[ "$MISSING_RC" -ne 0 ]] \
  || FAILURES+=("never-installed config coordinate unexpectedly exited 0: $MISSING_OUT")
grep -qi "not found" <<<"$MISSING_OUT" \
  || FAILURES+=("never-installed config coordinate did not report not found: $MISSING_OUT")
[[ "$UNINSTALL_COUNT_AFTER_MISSING" == "1" ]] \
  || FAILURES+=("never-installed coordinate changed uninstall count to $UNINSTALL_COUNT_AFTER_MISSING")
[[ "$GONE_RC" -eq 0 ]] \
  || FAILURES+=("defensive re-run of an unknown bare name exited $GONE_RC: $GONE_OUT")

if [[ "${#FAILURES[@]}" -ne 0 ]]; then
  printf '  - %s\n' "${FAILURES[@]}" >&2
  fail "payloadless config removal contract failed"
fi

log "PASS: payloadless config install/remove does not require an implicit directory"
