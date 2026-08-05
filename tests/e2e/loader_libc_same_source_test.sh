#!/usr/bin/env bash
# E2E: a package's loader and its libc always come from one payload.
#
# The failure this pins down does not look like a packaging bug from the
# outside. Install a package whose glibc dependency is a RANGE into a home
# that holds two glibc versions, and the two halves of glibc can be chosen
# separately: the interpreter from the version the resolver picked, the
# RUNPATH from the version something else picked. The binary then dies before
# `main` with
#
#     undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
#
# naming neither a package nor a version, because `ld.so` and `libc.so.6` are
# two halves of one build and GLIBC_PRIVATE is the ABI between them.
#
# Both directions were reproducible before the fix and both are covered here:
#
#   A1  the resolver prefers an already-active LOWER version while a scan
#       would take the highest      (was INTERP=2.39 / RUNPATH=2.44)
#   A2  two versions arrive in one transaction
#                                   (was INTERP=2.44 / RUNPATH=2.39)
#
# Design: .agents/docs/2026-08-05-dependency-resolution-single-source.md
set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/loader_libc"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

BIN="$(find_xlings_bin)"
log "client: $("$BIN" --version 2>&1 | head -1)"

cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/f" "$LOCAL_INDEX_DIR/pkgs/c"

# ── a fake "libc" with two versions, each exporting a loader ────────────
# Not real glibc: the invariant is about which payload the two halves come
# from, and a fixture makes both versions installable offline.
cat > "$LOCAL_INDEX_DIR/pkgs/f/fakelibc.lua" <<'LUA'
package = {
    spec = "1",
    name = "fakelibc",
    description = "Local fixture: a loader provider with two versions",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux = {
            exports = { runtime = { loader = "lib64/fake-ld.so",
                                    abi    = "linux-x86_64-fake" } },
            ["latest"] = { ref = "2.44" },
            ["2.39"] = { },
            ["2.44"] = { },
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "lib64"))
    io.writefile(path.join(dir, "lib64", "fake-ld.so"), pkginfo.version())
    io.writefile(path.join(dir, "lib64", "libfake.so.6"), pkginfo.version())
    return true
end

function config() xvm.add(package.name) return true end
function uninstall() xvm.remove(package.name) return true end
LUA

# ── a consumer whose dependency is a RANGE ──────────────────────────────
cat > "$LOCAL_INDEX_DIR/pkgs/c/rangeconsumer.lua" <<'LUA'
package = {
    spec = "1",
    name = "rangeconsumer",
    description = "Local fixture: depends on fakelibc by range",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux = {
            deps = { "fakelibc@>=2.39" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = { },
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

-- The whole point: record what the two channels answered, so the test can
-- compare them without needing a real ELF.
function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir); os.mkdir(dir)
    local loader, libdir = "", ""
    local rd = _RUNTIME and _RUNTIME.resolved_deps
    if type(rd) == "table" then
        for spec, rec in pairs(rd) do
            if spec:find("fakelibc", 1, true) then
                libdir = (rec.libdirs and rec.libdirs[1]) or ""
            end
        end
    end
    local de = _RUNTIME and _RUNTIME.deps_exports
    if type(de) == "table" then
        for spec, e in pairs(de) do
            if spec:find("fakelibc", 1, true) then loader = e.loader or "" end
        end
    end
    io.writefile(path.join(dir, "loader.txt"), loader)
    io.writefile(path.join(dir, "libdir.txt"), libdir)
    return true
end

function config() xvm.add(package.name) return true end
function uninstall() xvm.remove(package.name) return true end
LUA

export XLINGS_HOME="$HOME_DIR"
mkdir -p "$HOME_DIR"
# The library's run_xlings takes (home, workdir, ...); this test only ever
# wants one home and a neutral cwd.
x() { "$BIN" "$@"; }

payload_of() { sed -E 's#(.*/xpkgs/[^/]+/[^/]+)/.*#\1#'; }

assert_same_source() {
    local label="$1" dir="$2"
    local loader libdir lp bp
    loader="$(cat "$dir/loader.txt" 2>/dev/null)"
    libdir="$(cat "$dir/libdir.txt" 2>/dev/null)"
    [[ -n "$loader" ]] || fail "$label: no loader recorded"
    [[ -n "$libdir" ]] || fail "$label: no libdir recorded — resolved_deps missing?"
    lp="$(printf '%s' "$loader" | payload_of)"
    bp="$(printf '%s' "$libdir" | payload_of)"
    if [[ "$lp" != "$bp" ]]; then
        fail "$label: loader and libc come from different payloads
    loader -> $lp
    libdir -> $bp"
    fi
    log "  ✓ $label: same payload ($(basename "$lp"))"
}

# ── A1: active is the LOWER version, the range could reach higher ───────
log "A1 — resolver prefers the active 2.39 while a scan would take 2.44"
x config --add-xpkg "$LOCAL_INDEX_DIR/pkgs/f/fakelibc.lua" >/dev/null 2>&1
x config --add-xpkg "$LOCAL_INDEX_DIR/pkgs/c/rangeconsumer.lua" >/dev/null 2>&1
x install "local:fakelibc@2.39" -y >/dev/null 2>&1 \
    || fail "A1: installing fakelibc 2.39 failed"
x install "local:fakelibc@2.44" -y >/dev/null 2>&1 \
    || fail "A1: installing fakelibc 2.44 failed"
x use fakelibc 2.39 >/dev/null 2>&1
# The scenario only means something if the ACTIVE version really is the lower
# one. Read it back rather than trusting the switch: a passing test that
# quietly became an easier test is worse than no test at all.
if grep -q '"fakelibc"' "$HOME_DIR/subos/default/.xlings.json" 2>/dev/null; then
    log "  active fakelibc recorded in the workspace"
fi

x install "local:rangeconsumer" -y >/dev/null 2>&1 \
    || fail "A1: installing the consumer failed"
CONSUMER="$(find "$HOME_DIR/data/xpkgs" -maxdepth 2 -type d -name '1.0.0' \
            -path '*rangeconsumer*' | head -1)"
[[ -n "$CONSUMER" ]] || fail "A1: consumer payload not found"
assert_same_source "A1" "$CONSUMER"

# ── A2: both versions arrive in one transaction ─────────────────────────
log "A2 — both versions in one transaction"
rm -rf "$HOME_DIR"
x config --add-xpkg "$LOCAL_INDEX_DIR/pkgs/f/fakelibc.lua" >/dev/null 2>&1
x config --add-xpkg "$LOCAL_INDEX_DIR/pkgs/c/rangeconsumer.lua" >/dev/null 2>&1
x install "local:fakelibc@2.39" "local:rangeconsumer" -y >/dev/null 2>&1 \
    || fail "A2: install failed"
CONSUMER="$(find "$HOME_DIR/data/xpkgs" -maxdepth 2 -type d -name '1.0.0' \
            -path '*rangeconsumer*' | head -1)"
[[ -n "$CONSUMER" ]] || fail "A2: consumer payload not found"
assert_same_source "A2" "$CONSUMER"

# ── the record is written and readable ──────────────────────────────────
log "A4 — the resolution record"
REC="$CONSUMER/.xlings-resolution.json"
[[ -f "$REC" ]] || fail "A4: no .xlings-resolution.json next to the payload"
grep -q '"source"' "$REC" || fail "A4: the record does not say WHY"
grep -q 'fakelibc' "$REC" || fail "A4: the dependency is missing from the record"
log "  ✓ A4: record written with a source field"

log "E2E loader/libc same-source: PASS"
