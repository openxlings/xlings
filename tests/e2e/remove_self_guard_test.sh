#!/usr/bin/env bash
# E2E: `xlings remove xim:xlings` must refuse when xlings has only one
# version installed (would otherwise try to delete the running .exe shim
# on Windows, raising ERROR_SHARING_VIOLATION → uncaught
# filesystem_error → terminate, silent CI fail; on POSIX it'd succeed
# but leave a workspace pointer to a removed version).
#
# Multi-version remove (auto-switch to the highest remaining version)
# must continue to work — the guard only fires on the single-version
# case, which is the only one that hits the broken "no surviving
# versions" branch in the installer.
#
# Fixture: a fake xpkg called "xlings" (two versions, no URL) that just
# drops a stub file and registers via xvm.add. We never actually replace
# the bootstrap — we only exercise the remove-side decision logic.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/remove_self_guard"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/x/xlings.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
    ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

# RUN_RC: like RUN but captures both stdout/stderr and exit code without
# letting `set -e` abort the script. Returns the exit code via $? from a
# tail call so callers can stash it.
RUN_RC() {
    local out
    set +e
    out="$(RUN "$@" 2>&1)"
    local rc=$?
    set -e
    printf '%s' "$out"
    return "$rc"
}

xlings_versions() {
    python3 - "$HOME_DIR" <<'PY'
import json, sys, pathlib
home = sys.argv[1]
data = json.loads(pathlib.Path(home, ".xlings.json").read_text())
vers = sorted((data.get("versions") or {}).get("xlings", {}).get("versions", {}))
print(",".join(vers))
PY
}

xlings_active() {
    python3 - "$HOME_DIR" <<'PY'
import json, sys, pathlib
home = sys.argv[1]
ws = json.loads(pathlib.Path(home, "subos/default/.xlings.json").read_text())
entry = (ws.get("workspace") or {}).get("xlings")
# 0.4.19+: workspace value is {active, installed[]} object form. Pre-0.4.19
# files (or the absent-entry case) still give a plain string / None.
if isinstance(entry, dict):
    print(entry.get("active", "<none>"))
elif isinstance(entry, str):
    print(entry)
else:
    print("<none>")
PY
}

mkdir -p "$HOME_DIR"

# Private copy of the shared fixture index, neutralise sub-index repos.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

# Override pkgs/x/xlings.lua with a fixture that just drops a stub and
# registers via xvm.add — no real binary, no self-replace, no downloads.
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "xlings",
    description = "Local fixture for tests/e2e/remove_self_guard_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "xlings"),
                 "#!/bin/sh\necho 'fixture xlings " .. pkginfo.version() .. "'\n")
    return true
end

function config()
    xvm.add("xlings", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("xlings")
    return true
end
LUA

# Seed XLINGS_HOME pointing at our private (neutralised) index.
mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF

log "init sandbox"
RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ── Scenario 1: install one version, try to remove it → guard refuses ───
log "scenario 1: single-version remove must be refused"
RUN install "xim:xlings@1.0.0" -y >/dev/null 2>&1 \
    || fail "install xim:xlings@1.0.0 failed"

[[ "$(xlings_versions)" == "1.0.0" ]] \
    || fail "post-install: expected versions=1.0.0, got '$(xlings_versions)'"

set +e
out="$(RUN remove xim:xlings -y 2>&1)"
rc=$?
set -e

if [[ "$rc" -ne 2 ]]; then
    echo "$out"
    fail "expected exit code 2 for single-version self-remove, got $rc"
fi

# Error message should mention "only one version" + the right next steps.
echo "$out" | grep -qiE "only.*one.*version|only.*has.*one" \
    || { echo "$out"; fail "guard error message doesn't mention single-version case"; }
echo "$out" | grep -qF "self uninstall" \
    || { echo "$out"; fail "guard error message doesn't redirect to 'self uninstall'"; }

# Critical: nothing was actually removed.
[[ "$(xlings_versions)" == "1.0.0" ]] \
    || fail "single-version remove must NOT mutate version DB; got '$(xlings_versions)'"

log "  ok: refused with rc=2, version DB intact"

# ── Scenario 2: install a second version, multi-version remove succeeds ─
log "scenario 2: multi-version remove must auto-switch (existing semantics)"
RUN install "xim:xlings@2.0.0" -y >/dev/null 2>&1 \
    || fail "install xim:xlings@2.0.0 failed"
RUN use xlings 2.0.0 >/dev/null 2>&1 \
    || fail "use xlings 2.0.0 failed"

versions_now="$(xlings_versions)"
[[ "$versions_now" == "1.0.0,2.0.0" ]] \
    || fail "post-install: expected versions=1.0.0,2.0.0, got '$versions_now'"
[[ "$(xlings_active)" == "2.0.0" ]] \
    || fail "post-install: expected active=2.0.0, got '$(xlings_active)'"

# Remove the active version (2.0.0). Guard must NOT fire — there's another
# version. Installer auto-switches active → 1.0.0.
RUN remove "xim:xlings@2.0.0" -y >/dev/null 2>&1 \
    || fail "multi-version remove of active version failed"

[[ "$(xlings_versions)" == "1.0.0" ]] \
    || fail "after removing 2.0.0: expected versions=1.0.0, got '$(xlings_versions)'"
[[ "$(xlings_active)" == "1.0.0" ]] \
    || fail "after removing 2.0.0: expected active=1.0.0 (auto-switched), got '$(xlings_active)'"

log "  ok: multi-version remove succeeded, active auto-switched to 1.0.0"

# ── Scenario 3: now back to single-version, guard must fire again ───────
log "scenario 3: after multi→single, guard fires again"
set +e
out="$(RUN remove xim:xlings -y 2>&1)"
rc=$?
set -e

[[ "$rc" -eq 2 ]] \
    || { echo "$out"; fail "expected rc=2 again after returning to single-version state, got $rc"; }
[[ "$(xlings_versions)" == "1.0.0" ]] \
    || fail "guard re-fired but somehow mutated DB: '$(xlings_versions)'"

log "  ok: guard re-fires correctly after returning to single-version state"

log "PASS: remove_self_guard_test"
