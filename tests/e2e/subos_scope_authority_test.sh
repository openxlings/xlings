#!/usr/bin/env bash
# subos_scope_authority_test.sh — install, use and remove must act on the SAME
# subos.
#
# There used to be two implementations of "which subos is this process acting
# on", and they disagreed on exactly one input:
#
#   Config::paths().subosDir        computed once at construction,
#                                   IGNORED forceGlobalScope_
#   Config::xvm_artifact_subos_dir()  recomputed per call, HONORED it
#
# `xlings install -g` sets forceGlobalScope_. install resolved its artifact
# directories with the second; `use` and the removal path read `paths()`, i.e.
# the first. So inside a project directory, `-g` installed into the global
# subos and remove went looking in the project's — the package could not be
# uninstalled by the command that installed it, and nothing said why.
#
# This is not a defect a call site can fix. It is what having two authorities
# means, so the test asserts the property rather than any one symptom:
# whatever `-g` installs, `-g` removes.
#
# Refs: .agents/docs/2026-07-31-xvm-subos-architecture-review.md §3
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_scope_authority"
HOME_DIR="$RUNTIME_DIR/home"
PROJ_DIR="$RUNTIME_DIR/proj"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() {
  chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

# Every command runs from inside the project, which is the whole point: the
# project declares a subos, and `-g` says "act on the home anyway".
RUN_PROJ() {
  ( cd "$PROJ_DIR" && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" </dev/null )
}

mkdir -p "$HOME_DIR/subos/default/bin" "$PROJ_DIR" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/s"

cat > "$LOCAL_INDEX_DIR/pkgs/s/sc-probe.lua" <<'LUA'
package = {
    spec = "1", name = "sc-probe",
    description = "subos scope authority fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "sc-probe"), "#!/bin/sh\necho scope-ok\n")
    return true
end
function config()
    xvm.add("sc-probe", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall() xvm.remove("sc-probe") return true end
LUA

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

# The project declares its own subos. Without this the two resolvers agree and
# the test proves nothing.
cat > "$PROJ_DIR/.xlings.json" <<'JSON'
{ "subos": "pa" }
JSON

log "init sandbox"
RUN_PROJ self init >/dev/null 2>&1 || fail "self init failed"

GLOBAL_BIN="$HOME_DIR/subos/default/bin"
PROJ_BIN="$PROJ_DIR/.xlings/subos/pa/bin"

# ── S1: -g installs into the HOME's subos, not the project's ─────────
log "S1: -g targets the home's subos from inside a project"
RUN_PROJ install sc-probe -g -y >/dev/null 2>&1 || fail "S1: install -g failed"

[[ -x "$GLOBAL_BIN/sc-probe" ]] \
  || fail "S1: -g did not create the shim in the home's subos"
if [[ -e "$PROJ_BIN/sc-probe" ]]; then
  fail "S1: -g leaked a shim into the project subos"
fi

# ── S2: THE ASSERTION — what -g installed, -g removes ────────────────
#
# With two resolvers this failed: remove resolved the project subos, found
# nothing registered there, and left the home's shim in place while reporting
# whatever it reported.
log "S2: -g removes what -g installed"
out="$(RUN_PROJ remove sc-probe -g -y 2>&1 || true)"
if [[ -e "$GLOBAL_BIN/sc-probe" ]]; then
  fail "S2: remove -g left the shim the matching install created; got:\n$out"
fi

# ── S3: without -g the project subos is the target, symmetrically ────
# The authoritative half is the project subos. A copy also appears in the
# home's bin ON PURPOSE -- `common::mirror_shim_to_global_bin`: a project
# subos's bin is not on PATH and the home's is, so the mirror is what makes
# the tool reachable at all. Asserting its absence would be asserting against
# a designed behaviour, so what is asserted is the authoritative location.
log "S3: without -g both halves act on the project subos"
RUN_PROJ install sc-probe -y >/dev/null 2>&1 || fail "S3: project install failed"
[[ -x "$PROJ_BIN/sc-probe" ]] \
  || fail "S3: project install did not create the shim in the project subos"

out="$(RUN_PROJ remove sc-probe -y 2>&1 || true)"
# NEGATIVE assertion, so the pattern has to track the message or it stops
# testing without failing. 2026.8.22.1 collapsed six wordings for this state
# into one and dropped "current" for "this"; the old literal would now never
# match and this check would pass vacuously forever.
if grep -qE "is not installed in (this|current) subos" <<<"$out"; then
  fail "S3: remove looked in a different subos than install wrote to; got:\n$out"
fi
if [[ -e "$PROJ_BIN/sc-probe" ]]; then
  fail "S3: project remove left the shim behind; got:\n$out"
fi

log "PASS: subos_scope_authority"
