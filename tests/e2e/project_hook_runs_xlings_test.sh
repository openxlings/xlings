#!/usr/bin/env bash
# A hook of a package installed into a PROJECT's store runs `xlings` from its
# bin directory (`system.bindir()`), as gcc's config hook does to rewrite its
# specs through `xlings script`. A home's own subos has that entry from
# `self init`; a project's subos (`.mcpp/.xlings/subos/_`, the store mcpp keeps
# for a project's indexes) held only the shims of what it installed, so the
# hook failed with `sh: <project>/.xlings/subos/_/bin/xlings: not found` and
# the install with it. Measured from mcpp: a project depending on a package
# whose edge is `xim:gcc` could not install it on a home where gcc's specs
# needed a rewrite.
#
# The install hook here runs `<bindir>/xlings --version` and records the
# answer; the install succeeds only when the entry is there.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/project_hook_runs_xlings"
REGISTRY_DIR="$RUNTIME_DIR/registry"
PROJECT_DIR="$RUNTIME_DIR/member/.mcpp"
GLOBAL_INDEX_DIR="$RUNTIME_DIR/global-index"
MEMBER_INDEX_DIR="$RUNTIME_DIR/member-index"
USER_HOME="$RUNTIME_DIR/user"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

mkdir -p "$REGISTRY_DIR/subos/default/bin" "$REGISTRY_DIR/bin" \
         "$GLOBAL_INDEX_DIR/pkgs" "$MEMBER_INDEX_DIR/pkgs/h" \
         "$PROJECT_DIR" "$USER_HOME"
# The home's entry, as an installed home has it: what every shim names.
ln -s "$XLINGS_BIN" "$REGISTRY_DIR/bin/xlings"
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
    "member:hook-runs-xlings": "1.0.0"
  }
}
JSON

cat > "$MEMBER_INDEX_DIR/pkgs/h/hook-runs-xlings.lua" <<'LUA'
package = {
    spec = "1",
    name = "hook-runs-xlings",
    description = "An install hook that runs xlings from its bin directory",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.system")

function install()
    local out = path.join(pkginfo.install_dir(), "xlings-version.txt")
    local ok = os.execute(string.format([["%s" --version > "%s" 2>&1]],
                          path.join(system.bindir(), "xlings"), out))
    return ok == true or ok == 0
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
      "$XLINGS_BIN" "$@" )
}

log "materialize the isolated global and project indexes"
RUN update >/dev/null 2>&1 || fail "failed to materialize isolated indexes"

log "install a project package whose hook runs xlings"
set +e
install_output="$(RUN install member:hook-runs-xlings@1.0.0 -y 2>&1)"
install_rc=$?
set -e
[[ "$install_rc" -eq 0 ]] || fail "the install failed (exit $install_rc)
$install_output"

RESULT="$PROJECT_DIR/.xlings/data/xpkgs/member-x-hook-runs-xlings/1.0.0/xlings-version.txt"
[[ -f "$RESULT" ]] || fail "the hook recorded nothing"
grep -qi "xlings" "$RESULT" || fail "the hook's xlings answered: $(cat "$RESULT")"
[[ -e "$PROJECT_DIR/.xlings/subos/_/bin/xlings" ]] \
  || fail "the project's subos has no xlings entry"

log "PASS: a project package's hook ran xlings from its bin directory"
