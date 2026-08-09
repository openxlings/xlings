#!/usr/bin/env bash
# E2E: removing a delegating provider must ignore another provider's version.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/remove_foreign_provider_delegator"
HOME_DIR="$RUNTIME_DIR/home"
DELEGATE_INDEX_DIR="$RUNTIME_DIR/delegate-index"
LOCAL_INDEX_DIR="$RUNTIME_DIR/local-index"
MARKER="$RUNTIME_DIR/delegate-uninstall.marker"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"

RUN() {
  ( cd "$RUNTIME_DIR" && env -i HOME="$HOME_DIR" USER=xlings-ci \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

RUN_SHIM() {
  ( cd "$RUNTIME_DIR" && env -i HOME="$HOME_DIR" USER=xlings-ci \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$SHIM" "$@" )
}

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
for index in "$DELEGATE_INDEX_DIR" "$LOCAL_INDEX_DIR"; do
  mkdir -p "$index"
  cp -r "$FIXTURE_INDEX_DIR/libs" "$index/libs"
  printf 'xim_indexrepos = {}\n' > "$index/xim-indexrepos.lua"
  mkdir -p "$index/pkgs/g"
done

cat > "$DELEGATE_INDEX_DIR/pkgs/g/gcc.lua" <<LUA
package = {
    spec = "1",
    name = "gcc",
    description = "Delegating provider that intentionally registers no gcc version",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["2"] = {} },
        macosx  = { ["2"] = {} },
        windows = { ["2"] = {} },
    },
}

import("xim.libxpkg.pkginfo")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(dir)
    io.writefile(path.join(dir, "delegated.txt"), "xim:gcc@2")
    return true
end

function config()
    return true
end

function uninstall()
    io.writefile("$MARKER", "ran")
    return true
end
LUA

cat > "$LOCAL_INDEX_DIR/pkgs/g/gcc.lua" <<'LUA'
package = {
    spec = "1",
    name = "gcc",
    description = "Foreign provider that owns the gcc target",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    programs = {"gcc"},
    xpm = {
        linux   = { ["1"] = {} },
        macosx  = { ["1"] = {} },
        windows = { ["1"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "gcc"), "#!/bin/sh\necho local-gcc-1\n")
    if os.host() ~= "windows" then
        os.exec("chmod +x " .. path.join(bindir, "gcc"))
    end
    return true
end

function config()
    xvm.add("gcc", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("gcc")
    return true
end
LUA

cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "index_repos": [
    {"name": "xim", "url": "$DELEGATE_INDEX_DIR"},
    {"name": "local", "url": "$LOCAL_INDEX_DIR"}
  ]
}
JSON

if ! RUN self init >"$RUNTIME_DIR/self-init.out" 2>&1; then
  sed 's/^/    | /' "$RUNTIME_DIR/self-init.out" >&2
  fail "self init failed"
fi
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

RUN install local:gcc@1 -y >/dev/null 2>&1 \
  || fail "foreign provider install failed"
RUN install xim:gcc@2 -y >/dev/null 2>&1 \
  || fail "delegator install failed"

HOME_STATE="$HOME_DIR/.xlings.json"
SUBOS_STATE="$HOME_DIR/subos/default/.xlings.json"
SHIM="$HOME_DIR/subos/default/bin/gcc"
FOREIGN_PAYLOAD="$HOME_DIR/data/xpkgs/local-x-gcc/1"
DELEGATE_PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-gcc/2"

python3 - "$HOME_STATE" "$SUBOS_STATE" <<'PY' \
  || fail "setup did not create the expected foreign-provider ownership"
import json, pathlib, sys
home = json.loads(pathlib.Path(sys.argv[1]).read_text())
subos = json.loads(pathlib.Path(sys.argv[2]).read_text())
entry = home["versions"]["gcc"]["versions"]["local:1"]
assert entry["bindingGroup"]["provider"] == "local:gcc", entry
workspace = subos["workspace"]["gcc"]
assert workspace["active"] == "local:1", workspace
assert "local:1" in workspace["installed"], workspace
PY
[[ -e "$SHIM" ]] || fail "setup: foreign gcc shim is missing"
[[ -d "$FOREIGN_PAYLOAD" ]] || fail "setup: foreign gcc payload is missing"
[[ -d "$DELEGATE_PAYLOAD" ]] || fail "setup: delegator payload is missing"
[[ "$(RUN_SHIM)" == "local-gcc-1" ]] \
  || fail "setup: gcc shim does not dispatch to the foreign provider"

FOREIGN_DB_BEFORE="$(python3 - "$HOME_STATE" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
print(json.dumps(data["versions"]["gcc"], sort_keys=True))
PY
)"
FOREIGN_WS_BEFORE="$(python3 - "$SUBOS_STATE" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
print(json.dumps(data["workspace"]["gcc"], sort_keys=True))
PY
)"

rm -f "$MARKER"
REMOVE_OUT="$RUNTIME_DIR/remove.out"
if ! RUN remove xim:gcc@2 -y >"$REMOVE_OUT" 2>&1; then
  sed 's/^/    | /' "$REMOVE_OUT" >&2
  fail "removing a zero-target delegator was blocked by the foreign gcc version"
fi

[[ -f "$MARKER" ]] \
  || fail "delegator uninstall hook did not run"
[[ ! -d "$DELEGATE_PAYLOAD" ]] \
  || fail "delegator payload survived removal"
[[ -d "$FOREIGN_PAYLOAD" ]] \
  || fail "foreign provider payload was removed"
[[ -e "$SHIM" ]] \
  || fail "foreign provider shim was removed"
[[ "$(RUN_SHIM)" == "local-gcc-1" ]] \
  || fail "foreign provider shim no longer dispatches after delegator removal"

FOREIGN_DB_AFTER="$(python3 - "$HOME_STATE" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
print(json.dumps(data["versions"]["gcc"], sort_keys=True))
PY
)"
FOREIGN_WS_AFTER="$(python3 - "$SUBOS_STATE" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
print(json.dumps(data["workspace"]["gcc"], sort_keys=True))
PY
)"
[[ "$FOREIGN_DB_AFTER" == "$FOREIGN_DB_BEFORE" ]] \
  || fail "foreign provider version data changed during delegator removal"
[[ "$FOREIGN_WS_AFTER" == "$FOREIGN_WS_BEFORE" ]] \
  || fail "foreign provider workspace data changed during delegator removal"

log "PASS: delegator removal ignores and preserves a foreign provider release"
