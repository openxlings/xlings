#!/usr/bin/env bash
# foreign_payload_reinstall_test.sh — a payload that belongs to another
# platform is not "already installed".
#
# `already installed` used to mean "the directory exists and is not empty". A
# payload left behind by a run that targeted another platform passes that test
# perfectly, so the install hook -- the only code that unpacks the right
# tarball -- was skipped, while the CONFIG hook ran and registered whatever was
# lying there. The measured case: a May-era Windows llvm@20.1.7 in a Linux
# store, which registered `clang.exe` … `libomp.dll` as programs, warned six
# times that `cc -> clang` could not be found, and reported success.
#
# Refs: .agents/docs/2026-07-30-subos-selection-leak-and-foreign-payload-plan.md
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/foreign_payload"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() {
  chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
      "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR/subos/default/bin" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/f"

cat > "$LOCAL_INDEX_DIR/pkgs/f/fp-probe.lua" <<'LUA'
package = {
    spec = "1", name = "fp-probe",
    description = "foreign payload fixture",
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
    io.writefile(path.join(bindir, "fp-tool"), "#!/bin/sh\necho native\n")
    return true
end
function config()
    xvm.add("fp-probe", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall() xvm.remove("fp-probe") return true end
LUA

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN self init >/dev/null 2>&1 || fail "self init failed"

log "F1: first install stamps the payload"
RUN install fp-probe >/dev/null 2>&1 || fail "install failed"
PAYLOAD="$(dirname "$(find "$HOME_DIR/data/xpkgs" -name fp-tool -type f | head -1)")"
[[ -n "$PAYLOAD" ]] || fail "F1: payload not installed"
PKG_DIR="$(dirname "$PAYLOAD")"
[[ -f "$PKG_DIR/.xpkg-install.json" ]] \
  || fail "F1: no platform stamp written"
grep -q '"os"' "$PKG_DIR/.xpkg-install.json" \
  || fail "F1: stamp has no os field"

# ── F2: a foreign payload is reinstalled ─────────────────────────────
#
# Replace the payload with something from another platform and drop the
# stamp, exactly as a store carried over from another machine would look.
log "F2: a foreign payload is not 'already installed'"
rm -f "$PKG_DIR/.xpkg-install.json"
rm -f "$PAYLOAD/fp-tool"
printf 'MZ\220\0\0\0\0\0\0\0\0\0\0\0\0\0' > "$PAYLOAD/fp-tool.exe"

out="$(RUN install fp-probe 2>&1 || true)"
grep -qi "not for" <<<"$out" \
  || fail "F2: no warning about the foreign payload; got:\n$out"
[[ -f "$PAYLOAD/fp-tool" ]] \
  || fail "F2: install hook did not re-run (native binary still missing)"
[[ ! -f "$PAYLOAD/fp-tool.exe" ]] \
  || fail "F2: the foreign payload survived the reinstall"
[[ -f "$PKG_DIR/.xpkg-install.json" ]] \
  || fail "F2: the reinstall left no stamp"

# ── F3: a stamped native payload takes the fast path ─────────────────
#
# The check must not turn every repeat install into a reinstall: that would
# trade one silent wrong answer for a loud slow one.
log "F3: a stamped payload is still 'already installed'"
printf 'sentinel\n' > "$PAYLOAD/keep-me"
out="$(RUN install fp-probe 2>&1 || true)"
grep -q "already installed" <<<"$out" \
  || fail "F3: a stamped native payload should short-circuit; got:\n$out"
[[ -f "$PAYLOAD/keep-me" ]] \
  || fail "F3: the install hook re-ran and wiped the payload"

log "PASS: foreign_payload_reinstall"
