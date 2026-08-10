#!/usr/bin/env bash
# Regression for #524: a config hook must be able to locate the payload of a
# dependency THE RESOLVER JUST INSTALLED, on a home where neither was present.
#
# Why a cold home is the whole point. On a warm home the consumer is already
# installed, so the install is skipped and the config hook never runs -- which
# is exactly why every CI lane stayed green while `xlings install xim:gcc` was
# broken on every fresh machine. The first detector was a downstream repo's
# cache miss. This test is that detector, in-tree.
#
# The shape is gcc's, minimally: declare a namespaced, RANGED coordinate, let
# the resolver pick a concrete version, then ask for the payload from the
# config hook. Under xlings 2026.8.10.1 + libxpkg 0.0.55/0.0.56 the ask
# returned nil and the hook reported "payload not found" with the payload
# sitting next to it.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/config_hook_resolves_declared_dep"
HOME_DIR="$RUNTIME_DIR/xlings-home"
INDEX_DIR="$RUNTIME_DIR/index"
USER_HOME="$RUNTIME_DIR/user"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

mkdir -p "$HOME_DIR/subos/default/bin" "$INDEX_DIR/pkgs/d" "$INDEX_DIR/pkgs/c" "$USER_HOME"
printf 'xim_indexrepos = {}\n' > "$INDEX_DIR/xim-indexrepos.lua"

cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "version": "0.4.39",
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "subos": {"default": {"dir": ""}},
  "index_repos": [
    {"name": "xim", "url": "$INDEX_DIR"}
  ]
}
JSON

# The dependency. No url: an empty payload directory is enough -- this test is
# about RESOLUTION, and downloading anything would make it about the network.
# Two versions, so the range below has something to choose between and the
# recorded answer distinguishes "resolved" from "guessed the first match".
cat > "$INDEX_DIR/pkgs/d/depender-payload.lua" <<'LUA'
package = {
    spec = "1",
    name = "depender-payload",
    description = "Payload a consumer's config hook must be able to locate",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["latest"] = { ref = "1.2.0" }, ["1.0.0"] = {}, ["1.2.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.2.0" }, ["1.0.0"] = {}, ["1.2.0"] = {} },
        windows = { ["latest"] = { ref = "1.2.0" }, ["1.0.0"] = {}, ["1.2.0"] = {} },
    },
}
LUA

# The consumer. Declares a RANGE, asks three ways in config():
#
#   namespaced       the coordinate as declared -- must always work
#   bare             what every recipe in xim-pkgindex actually writes; the
#                    record set answers it uniquely, so it must work too
#
# The undeclared case gets its own consumer below, because a hook's output is
# only surfaced when the hook FAILS -- which is also the only moment a user
# reads it.
cat > "$INDEX_DIR/pkgs/c/dep-consumer.lua" <<'LUA'
package = {
    spec = "1",
    name = "dep-consumer",
    description = "Resolves its declared dependency from a config hook",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { deps = { "xim:depender-payload@>=1.0" },
                    ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { deps = { "xim:depender-payload@>=1.0" },
                    ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { deps = { "xim:depender-payload@>=1.0" },
                    ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")

function config()
    local ns    = pkginfo.dep_install_dir("xim:depender-payload")
    local bare  = pkginfo.dep_install_dir("depender-payload")
    local range = pkginfo.dep_install_dir("xim:depender-payload", ">=1.0")

    local f = io.open(path.join(pkginfo.install_dir(), "resolved.txt"), "w")
    if not f then return false end
    f:write("ns=" .. tostring(ns) .. "\n")
    f:write("bare=" .. tostring(bare) .. "\n")
    f:write("range=" .. tostring(range) .. "\n")
    f:close()

    -- Fail the way gcc fails, so a regression shows up as a failed install and
    -- not as a file nobody reads.
    return ns ~= nil and bare ~= nil and range ~= nil
end
LUA

RUN() {
  ( cd /tmp && env -i \
      HOME="$USER_HOME" \
      USER=xlings-test \
      SHELL=/bin/sh \
      PATH=/usr/bin:/bin \
      NO_COLOR=1 \
      XLINGS_HOME="$HOME_DIR" \
      "$XLINGS_BIN" "$@" )
}

log "materialize the isolated index"
RUN update >/dev/null 2>&1 || fail "failed to materialize the isolated index"

PAYLOAD_ROOT="$HOME_DIR/data/xpkgs/xim-x-depender-payload"
[[ ! -d "$PAYLOAD_ROOT" ]] \
  || fail "home is not cold: $PAYLOAD_ROOT already exists before the install"

log "cold install: the resolver must place the dependency, then the hook must find it"
set +e
install_output="$(RUN install xim:dep-consumer@1.0.0 -y 2>&1)"
install_rc=$?
set -e
[[ "$install_rc" -eq 0 ]] \
  || fail "consumer install failed (exit $install_rc) -- this is #524
$install_output"

RESULT_FILE="$HOME_DIR/data/xpkgs/xim-x-dep-consumer/1.0.0/resolved.txt"
[[ -f "$RESULT_FILE" ]] || fail "config hook recorded nothing
$install_output"

EXPECT="$PAYLOAD_ROOT/1.2.0"
ns="$(sed -n 's/^ns=//p' "$RESULT_FILE")"
bare="$(sed -n 's/^bare=//p' "$RESULT_FILE")"
range="$(sed -n 's/^range=//p' "$RESULT_FILE")"

[[ "$ns" == "$EXPECT" ]] \
  || fail "namespaced query resolved '$ns', expected '$EXPECT'"
[[ "$bare" == "$EXPECT" ]] \
  || fail "bare query resolved '$bare', expected '$EXPECT' -- the record set
answers this uniquely, and every recipe in xim-pkgindex writes it this way"
[[ "$range" == "$EXPECT" ]] \
  || fail "ranged query resolved '$range', expected '$EXPECT' -- the caller
restates the RANGE it declared, never the concrete version the resolver picked"
log "an unanswerable coordinate must fail LOUDLY, naming itself"

# A second consumer, because a hook's output reaches the user only when the
# hook fails. "Cannot answer" and "the dependency is not installed" produce the
# same nil, and #524 is what happens when they also produce the same message:
# the report chased a missing glibc that was sitting on disk.
cat > "$INDEX_DIR/pkgs/c/dep-stranger.lua" <<'LUA'
package = {
    spec = "1",
    name = "dep-stranger",
    description = "Asks for a coordinate it never declared",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { deps = { "xim:depender-payload@>=1.0" },
                    ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { deps = { "xim:depender-payload@>=1.0" },
                    ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { deps = { "xim:depender-payload@>=1.0" },
                    ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")

function config()
    return pkginfo.dep_install_dir("xim:never-declared") ~= nil
end
LUA

RUN update >/dev/null 2>&1 || fail "failed to refresh the isolated index"

set +e
stranger_output="$(RUN install xim:dep-stranger@1.0.0 -y 2>&1)"
stranger_rc=$?
set -e
[[ "$stranger_rc" -ne 0 ]] \
  || fail "asking for an undeclared coordinate must not report success
$stranger_output"
grep -q "never-declared" <<<"$stranger_output" \
  || fail "the failure never named the coordinate that could not be answered
$stranger_output"
grep -qi "not a declared dependency" <<<"$stranger_output" \
  || fail "the failure must say WHY it cannot answer, not just that it did not
$stranger_output"

log "PASS: a config hook resolves its declared dependency on a cold home"
