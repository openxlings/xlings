#!/usr/bin/env bash
# Regression for #514: a project-local consumer can resolve an exact payload
# already owned by the shared XLINGS_HOME registry, without consulting MCPP_HOME
# or confusing another namespace with the requested package.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/shared_registry_dep_install_dir"
REGISTRY_DIR="$RUNTIME_DIR/registry"
PROJECT_DIR="$RUNTIME_DIR/member/.mcpp"
GLOBAL_INDEX_DIR="$RUNTIME_DIR/global-index"
MEMBER_INDEX_DIR="$RUNTIME_DIR/member-index"
MCPP_DECOY_HOME="$RUNTIME_DIR/mcpp-home"
USER_HOME="$RUNTIME_DIR/user"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

GLOBAL_PAYLOAD="$REGISTRY_DIR/data/xpkgs/compat-x-zlib/1.3.2"
PROJECT_DECOY="$PROJECT_DIR/.xlings/data/xpkgs/other-x-zlib/1.3.2"
GLOBAL_DECOY="$REGISTRY_DIR/data/xpkgs/other-x-zlib/1.3.2"
MCPP_DECOY="$MCPP_DECOY_HOME/registry/data/xpkgs/compat-x-zlib/1.3.2"

mkdir -p "$REGISTRY_DIR/subos/default/bin" \
         "$GLOBAL_INDEX_DIR/pkgs" \
         "$MEMBER_INDEX_DIR/pkgs/c" \
         "$PROJECT_DIR" \
         "$USER_HOME" \
         "$GLOBAL_PAYLOAD" \
         "$PROJECT_DECOY" \
         "$GLOBAL_DECOY" \
         "$MCPP_DECOY"

printf 'shared registry payload\n' > "$GLOBAL_PAYLOAD/PAYLOAD"
printf 'wrong project namespace\n' > "$PROJECT_DECOY/PAYLOAD"
printf 'wrong global namespace\n' > "$GLOBAL_DECOY/PAYLOAD"
printf 'forbidden MCPP_HOME payload\n' > "$MCPP_DECOY/PAYLOAD"
printf 'xim_indexrepos = {}\n' > "$GLOBAL_INDEX_DIR/xim-indexrepos.lua"
printf 'xim_indexrepos = {}\n' > "$MEMBER_INDEX_DIR/xim-indexrepos.lua"

cat > "$REGISTRY_DIR/.xlings.json" <<JSON
{
  "version": "0.4.39",
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "subos": {"default": {"dir": ""}},
  "index_repos": [
    {"name": "xim", "url": "$GLOBAL_INDEX_DIR"}
  ]
}
JSON

cat > "$PROJECT_DIR/.xlings.json" <<JSON
{
  "index_repos": [
    {"name": "member", "url": "$MEMBER_INDEX_DIR"}
  ],
  "workspace": {
    "member:consumer": "1.0.0"
  }
}
JSON

cat > "$MEMBER_INDEX_DIR/pkgs/c/consumer.lua" <<'LUA'
package = {
    spec = "1",
    name = "consumer",
    description = "Project-local consumer of an independently seeded shared registry payload",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")

function install()
    local resolved = pkginfo.install_dir("compat:zlib", "1.3.2")
    local record = io.open(path.join(pkginfo.install_dir(), "resolved.txt"), "w")
    if not record then return false end
    record:write(tostring(resolved))
    record:close()
    return resolved ~= nil
end
LUA

RUN() {
  ( cd /tmp && env -i \
      HOME="$USER_HOME" \
      USER=xlings-test \
      SHELL=/bin/sh \
      PATH=/usr/bin:/bin \
      NO_COLOR=1 \
      XLINGS_HOME="$REGISTRY_DIR" \
      XLINGS_PROJECT_DIR="$PROJECT_DIR" \
      MCPP_HOME="$MCPP_DECOY_HOME" \
      "$XLINGS_BIN" "$@" )
}

log "materialize the isolated global and project indexes"
RUN update >/dev/null 2>&1 \
  || fail "failed to materialize isolated indexes"

log "install project-local consumer against the shared registry payload"
set +e
install_output="$(RUN install member:consumer@1.0.0 -y 2>&1)"
install_rc=$?
set -e
[[ "$install_rc" -eq 0 ]] \
  || fail "consumer install failed (exit $install_rc)
$install_output"

RESULT_FILE="$PROJECT_DIR/.xlings/data/xpkgs/member-x-consumer/1.0.0/resolved.txt"
[[ -f "$RESULT_FILE" ]] \
  || fail "consumer did not record pkginfo.install_dir in its project-local payload"

resolved="$(cat "$RESULT_FILE")"
[[ "$resolved" == "$GLOBAL_PAYLOAD" ]] \
  || fail "pkginfo.install_dir selected '$resolved', expected exact shared registry path '$GLOBAL_PAYLOAD'"
[[ "$resolved" != "$PROJECT_DECOY" && "$resolved" != "$GLOBAL_DECOY" ]] \
  || fail "pkginfo.install_dir accepted a same-bare-name foreign namespace"
[[ "$resolved" != "$MCPP_DECOY" ]] \
  || fail "pkginfo.install_dir inferred a forbidden path from MCPP_HOME"

log "PASS: project-local hook resolved the exact shared XLINGS_HOME dependency"
