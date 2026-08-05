#!/usr/bin/env bash
# E2E: a package may not put a libc on a process-global search path.
#
# LD_LIBRARY_PATH is inherited by every child of the subos shell, and most of
# those children are HOST binaries running under the HOST loader. ld.so and
# libc.so.6 are two halves of one build that talk over GLIBC_PRIVATE; handing a
# host binary our half of the pair does not fail to load, it segfaults before
# main and names nothing.
#
# This is how it reached a release: nvidia-gl-host-link gathered its runtime
# dependencies -- including glibc's -- into one directory and declared that
# directory on LD_LIBRARY_PATH, so that the NVIDIA vendor library, which is the
# host's file and so cannot carry an RPATH of ours, could find them. The libc
# in there was never usable for that purpose (the vendor is dlopen'd into a
# process whose libc is long since bound, and an already-loaded SONAME is not
# searched for), but it was inherited by /bin/bash. `xlings subos use` returned
# a shell that died of SIGSEGV before printing a character -- on a host whose
# glibc was the same upstream VERSION as ours, merely a different build.
#
# The recipe was fixed. This test covers the class, not that recipe: xlings
# drops such an entry itself, names it, and keeps the rest of the variable.
#
# What has to hold:
#   1. a declared directory holding libc.so.6 does not reach the environment
#   2. the other directories on the same variable survive -- they are usually
#      the point of the declaration
#   3. the drop is reported with the directory AND the package that declared
#      it, because a silent drop is indistinguishable from a recipe that never
#      declared anything
#   4. a directory with no libc in it is untouched (the guard is not a ban on
#      LD_LIBRARY_PATH)

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_env_libc_guard"
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
mkdir -p "$LOCAL_INDEX_DIR/pkgs/l"

# The fixture declares one variable naming two directories: one that holds a
# libc, one that does not. Both halves of the contract are then observable in
# a single value.
cat > "$LOCAL_INDEX_DIR/pkgs/l/libcguardfixture.lua" <<'LUA'
package = {
    spec = "1",
    name = "libcguardfixture",
    description = "Local fixture for tests/e2e/subos_env_libc_guard_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.subos")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    -- Contents are never loaded; the guard reads the directory listing, so a
    -- file of the right NAME is the whole fixture. Building a real glibc to
    -- test a filename check would test the build, not the check.
    os.mkdir(path.join(dir, "poisoned"))
    io.writefile(path.join(dir, "poisoned", "libc.so.6"), "not a real libc\n")
    io.writefile(path.join(dir, "poisoned", "libfixture.so.1"), "\n")
    os.mkdir(path.join(dir, "clean"))
    io.writefile(path.join(dir, "clean", "libfixture.so.1"), "\n")
    return true
end

function config()
    if type(subos.env) == "function" then
        local binding = package.name .. "@" .. pkginfo.version()
        subos.env{ var = "LD_LIBRARY_PATH", op = "prepend",
                   value = "${pkgdir}/poisoned:${pkgdir}/clean",
                   binding = binding }
        -- Same directory, a variable the loader does not read. Nothing may be
        -- dropped here: the hazard is the variable, not the directory.
        subos.env{ var = "E2E_GUARD_CONTROL", op = "set",
                   value = "${pkgdir}/poisoned", binding = binding }
    end
    return true
end

function uninstall() return true end
LUA

mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/data/xim-index-repos"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$BIN" "$@" ) }

x self init >/dev/null 2>&1 || true

OUT="$(x install libcguardfixture@1.0.0 -y 2>&1)" \
  || { echo "$OUT" >&2; fail "install failed"; }

PKGDIR="$HOME_DIR/data/xpkgs/xim-x-libcguardfixture/1.0.0"
[[ -f "$PKGDIR/poisoned/libc.so.6" ]] \
  || fail "fixture did not install: no $PKGDIR/poisoned/libc.so.6"

# The declaration itself must be recorded unchanged. The guard belongs at the
# point the environment is built, not at the point it is declared: a manifest
# that silently differs from what the recipe said is a second source of truth.
MANIFEST="$HOME_DIR/subos/default/.xlings.json"
grep -q "poisoned" "$MANIFEST" \
  || fail "the manifest did not record the declaration as written:
$(cat "$MANIFEST")"
log "  ✓ declaration recorded verbatim in the subos manifest"

RUN="$(x subos use default --cmd 'echo "LDLP=[$LD_LIBRARY_PATH]"; echo "CTRL=[$E2E_GUARD_CONTROL]"' 2>&1)"

# 1. the libc directory is gone
echo "$RUN" | grep -q "LDLP=.*$PKGDIR/poisoned" \
  && fail "a directory holding libc.so.6 reached LD_LIBRARY_PATH:
$RUN"
log "  ✓ the directory holding libc.so.6 was dropped"

# 2. the rest of the variable survives
echo "$RUN" | grep -q "LDLP=.*$PKGDIR/clean" \
  || fail "the guard took the whole variable instead of the offending entry:
$RUN"
log "  ✓ the other directory on the same variable survived"

# 3. the drop is reported, with directory and provider
echo "$RUN" | grep -q "dropped $PKGDIR/poisoned" \
  || fail "the drop was silent -- indistinguishable from a recipe that never
declared anything:
$RUN"
echo "$RUN" | grep -q "libcguardfixture@1.0.0" \
  || fail "the report does not name the package that declared it:
$RUN"
log "  ✓ reported, naming both the directory and the declaring package"

# 4. a variable the loader does not read is untouched
echo "$RUN" | grep -q "CTRL=\[$PKGDIR/poisoned\]" \
  || fail "the guard reached a variable the dynamic loader never reads:
$RUN"
log "  ✓ non-loader variables naming the same directory are untouched"

log "PASS: subos env libc guard"
