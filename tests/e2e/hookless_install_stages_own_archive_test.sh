#!/usr/bin/env bash
# mcpp-community/mcpp#636 / xpkg-manifest-v1 §6: a package without an install() hook
# must receive exactly the entries of its own archive, laid out as the
# archive lays them out, and nothing else -- and the archive stays in the
# download cache (runtimedir).
#
# Before the fix, every archive was extracted into the shared runtime
# directory (installer.cpp's runtime_dir_), and a hookless install's
# staging fallback (stage_extracted_payload_) was handed that WHOLE shared
# directory -- moving every entry in it, including other packages'
# archives and download sidecars, into this package's install_dir. The
# single-top-level-directory stripping branch never ran for a real
# download (the archive file itself was always a second entry), so the
# layout every hookless install produced was un-stripped
# (<version>/<top-level-dir>/...) -- which is what this file's LAYOUT
# scenario asserts stays true after the fix, and what 55 mcpp-index
# `mcpp = "*/…/mcpp.toml"` descriptors depend on.
#
# No network needed: each fixture archive is pre-placed at the exact path
# the downloader would have saved it to, with a matching sha256 declared
# in the xpm entry (downloader.cpp's "cache hit path 1"), so the recipe's
# `url` is never actually fetched -- only its filename (the last path
# segment) has to match the pre-placed file.
#
# Measured against the released 2026.9.12.1 binary (this repo's own
# main-checkout build; see the header of the PR this file ships with):
# the POSITIVE scenario fails -- hooked-fixture's archive is swept out of
# runtimedir and into hookless-multi-fixture's install_dir.
#
# Refs: .agents/docs/2026-09-14-636-build-database-and-the-latest-xlings.md §2
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/hookless_install_stages_own_archive"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
STAGE_DIR="$RUNTIME_DIR/stage"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
      "$XLINGS_BIN" "$@" </dev/null )
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

mkdir -p "$HOME_DIR/subos/default/bin" "$RUNTIME_DIR" "$STAGE_DIR"
RUNTIMEDIR="$HOME_DIR/data/runtimedir"
mkdir -p "$RUNTIMEDIR"

# ── Build three fixture archives, pre-placed in runtimedir as though
# already downloaded ────────────────────────────────────────────────────

# hooked.tar.gz: one top-level directory named exactly like the archive's
# own basename, matching the xpkg-manifest-v1 §8.2 example's convention
# (install_file():replace(".tar.gz", "") names the extracted directory).
mkdir -p "$STAGE_DIR/hooked/bin"
printf 'hooked tool\n' > "$STAGE_DIR/hooked/bin/hooked-tool"
tar -czf "$RUNTIMEDIR/hooked.tar.gz" -C "$STAGE_DIR" hooked
rm -rf "$STAGE_DIR/hooked"
HOOKED_SHA="$(sha256_of "$RUNTIMEDIR/hooked.tar.gz")"

# hookless-multi.tar.gz: several LOOSE top-level entries, no wrapping
# directory -- the archive's own top-level entry set is {bin, lib,
# README.txt}, and that is exactly what install_dir must equal.
mkdir -p "$STAGE_DIR/multi/bin" "$STAGE_DIR/multi/lib"
printf 'tool\n' > "$STAGE_DIR/multi/bin/tool"
printf 'lib\n'  > "$STAGE_DIR/multi/lib/libfoo.so"
printf 'docs\n' > "$STAGE_DIR/multi/README.txt"
tar -czf "$RUNTIMEDIR/hookless-multi.tar.gz" -C "$STAGE_DIR/multi" bin lib README.txt
rm -rf "$STAGE_DIR/multi"
MULTI_SHA="$(sha256_of "$RUNTIMEDIR/hookless-multi.tar.gz")"

# hookless-single.tar.gz: ONE top-level directory, the shape 55 mcpp-index
# descriptors depend on staying un-stripped.
mkdir -p "$STAGE_DIR/single/hookless-single-3.0.0/bin" "$STAGE_DIR/single/hookless-single-3.0.0/share"
printf 'tool3\n' > "$STAGE_DIR/single/hookless-single-3.0.0/bin/tool3"
printf 'doc\n'   > "$STAGE_DIR/single/hookless-single-3.0.0/share/doc.txt"
tar -czf "$RUNTIMEDIR/hookless-single.tar.gz" -C "$STAGE_DIR/single" hookless-single-3.0.0
rm -rf "$STAGE_DIR/single"
SINGLE_SHA="$(sha256_of "$RUNTIMEDIR/hookless-single.tar.gz")"

# ── Fixture index: one hooked package, two hookless ────────────────────
#
# The `url` below is never fetched -- downloader.cpp's cache-hit path 1
# matches the declared sha256 against the file already sitting at
# runtimedir/<last path segment of url>, so it never even attempts a
# connection. Only the filename has to match what was pre-placed above.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/h"

cat > "$LOCAL_INDEX_DIR/pkgs/h/hooked-fixture.lua" <<LUA
package = {
    spec = "1", name = "hooked-fixture", type = "package",
    description = "local fixture: an install() hook, for hookless_install_stages_own_archive_test.sh",
    authors = {"xlings-ci"}, licenses = {"MIT"}, status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = { url = "https://example.invalid/hooked.tar.gz", sha256 = "$HOOKED_SHA" } },
        macosx  = { ["1.0.0"] = { url = "https://example.invalid/hooked.tar.gz", sha256 = "$HOOKED_SHA" } },
        windows = { ["1.0.0"] = { url = "https://example.invalid/hooked.tar.gz", sha256 = "$HOOKED_SHA" } },
    },
}
import("xim.libxpkg.pkginfo")
function install()
    os.tryrm(pkginfo.install_dir())
    os.mv(pkginfo.install_file():replace(".tar.gz", ""), pkginfo.install_dir())
    return true
end
function config() return true end
LUA

cat > "$LOCAL_INDEX_DIR/pkgs/h/hookless-multi-fixture.lua" <<LUA
package = {
    spec = "1", name = "hookless-multi-fixture", type = "package",
    description = "local fixture: no hooks at all, multi-entry archive, for hookless_install_stages_own_archive_test.sh",
    authors = {"xlings-ci"}, licenses = {"MIT"}, status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = { url = "https://example.invalid/hookless-multi.tar.gz", sha256 = "$MULTI_SHA" } },
        macosx  = { ["1.0.0"] = { url = "https://example.invalid/hookless-multi.tar.gz", sha256 = "$MULTI_SHA" } },
        windows = { ["1.0.0"] = { url = "https://example.invalid/hookless-multi.tar.gz", sha256 = "$MULTI_SHA" } },
    },
}
LUA

cat > "$LOCAL_INDEX_DIR/pkgs/h/hookless-single-fixture.lua" <<LUA
package = {
    spec = "1", name = "hookless-single-fixture", type = "package",
    description = "local fixture: no hooks at all, single top-level directory, for hookless_install_stages_own_archive_test.sh",
    authors = {"xlings-ci"}, licenses = {"MIT"}, status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["3.0.0"] = { url = "https://example.invalid/hookless-single.tar.gz", sha256 = "$SINGLE_SHA" } },
        macosx  = { ["3.0.0"] = { url = "https://example.invalid/hookless-single.tar.gz", sha256 = "$SINGLE_SHA" } },
        windows = { ["3.0.0"] = { url = "https://example.invalid/hookless-single.tar.gz", sha256 = "$SINGLE_SHA" } },
    },
}
LUA

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
EOF

log "init sandbox"
RUN self init >/dev/null 2>&1 || fail "self init failed"

# ── POSITIVE: a hooked install first, then a hookless one ─────────────
#
# The hooked package's archive must stay in runtimedir through and past
# the hookless install that follows it -- that is exactly the shared
# state the sweep used to reach into.
log "POSITIVE: install hooked-fixture (leaves its archive in runtimedir)"
out1="$(RUN install hooked-fixture@1.0.0 -y 2>&1)" \
  || { echo "$out1"; fail "hooked-fixture install failed"; }
HOOKED_DIR="$HOME_DIR/data/xpkgs/xim-x-hooked-fixture/1.0.0"
[[ -f "$HOOKED_DIR/bin/hooked-tool" ]] \
  || { echo "$out1"; fail "hooked-fixture did not install its own payload"; }
[[ -f "$RUNTIMEDIR/hooked.tar.gz" ]] \
  || fail "setup: hooked.tar.gz should already sit in runtimedir after the hooked install"

log "POSITIVE: install hookless-multi-fixture (no install() hook at all)"
out2="$(RUN install hookless-multi-fixture@1.0.0 -y 2>&1)" \
  || { echo "$out2"; fail "hookless-multi-fixture install failed"; }

MULTI_DIR="$HOME_DIR/data/xpkgs/xim-x-hookless-multi-fixture/1.0.0"
[[ -d "$MULTI_DIR" ]] || fail "hookless-multi-fixture install_dir does not exist"

# Entry set equals the archive's own top-level entry set: {bin, lib,
# README.txt}, nothing else -- except xlings's own bookkeeping
# (.xpkg-install.json, the platform stamp; .xlings-resolution.json, the
# resolved dependency closure; .xpkg.lua, the recipe
# snapshot), which every install writes regardless of this fix and is not
# part of "the archive's entries". Checked as a count plus membership
# (not string equality against a fixed ordering) because `sort`'s
# collation of "README.txt" against "bin"/"lib" is locale-dependent.
entry_count="$(ls -A "$MULTI_DIR" | grep -v -E '^(\.xpkg-install\.json|\.xpkg\.lua|\.xlings-resolution\.json)$' | wc -l | tr -d ' ')"
[[ "$entry_count" == "3" ]] \
  || fail "hookless-multi-fixture install_dir has $entry_count non-bookkeeping entries, expected exactly 3 (bin, lib, README.txt); got: $(ls -A "$MULTI_DIR")"
[[ -f "$MULTI_DIR/bin/tool" ]] || fail "hookless-multi-fixture: bin/tool missing"
[[ -f "$MULTI_DIR/lib/libfoo.so" ]] || fail "hookless-multi-fixture: lib/libfoo.so missing"
[[ -f "$MULTI_DIR/README.txt" ]] || fail "hookless-multi-fixture: README.txt missing"

# The defect this guards: hooked.tar.gz (or its .lock/.meta sidecars)
# swept into a completely unrelated package's install_dir.
[[ ! -e "$MULTI_DIR/hooked.tar.gz" ]] \
  || fail "REGRESSION: hooked-fixture's archive was swept into hookless-multi-fixture's install_dir"
if compgen -G "$MULTI_DIR/*.lock" >/dev/null || compgen -G "$MULTI_DIR/*.meta" >/dev/null; then
  fail "REGRESSION: a download-cache sidecar (.lock/.meta) was swept into hookless-multi-fixture's install_dir"
fi

log "POSITIVE: runtimedir still holds both archives"
[[ -f "$RUNTIMEDIR/hooked.tar.gz" ]] \
  || fail "REGRESSION: hooked.tar.gz is gone from runtimedir after the hookless install"
[[ -f "$RUNTIMEDIR/hookless-multi.tar.gz" ]] \
  || fail "hookless-multi.tar.gz is not in runtimedir (the hookless package's own archive should stay in the download cache too)"

log "PASS: positive"

# ── LAYOUT: a single-top-level-directory archive is not stripped ──────
log "LAYOUT: install hookless-single-fixture (one top-level directory)"
out3="$(RUN install hookless-single-fixture@3.0.0 -y 2>&1)" \
  || { echo "$out3"; fail "hookless-single-fixture install failed"; }

SINGLE_DIR="$HOME_DIR/data/xpkgs/xim-x-hookless-single-fixture/3.0.0"
[[ -f "$SINGLE_DIR/hookless-single-3.0.0/bin/tool3" ]] \
  || fail "LAYOUT REGRESSION: expected <version>/hookless-single-3.0.0/bin/tool3, top-level directory was stripped"
[[ -f "$SINGLE_DIR/hookless-single-3.0.0/share/doc.txt" ]] \
  || fail "LAYOUT REGRESSION: expected <version>/hookless-single-3.0.0/share/doc.txt"
# The stripped layout the old code never actually reached but must still
# not appear: tool3 directly under install_dir/bin rather than nested one
# level deeper.
[[ ! -e "$SINGLE_DIR/bin" ]] \
  || fail "LAYOUT REGRESSION: install_dir/bin exists directly -- the top-level directory was stripped"

single_entry_count="$(ls -A "$SINGLE_DIR" | grep -v -E '^(\.xpkg-install\.json|\.xpkg\.lua|\.xlings-resolution\.json)$' | wc -l | tr -d ' ')"
[[ "$single_entry_count" == "1" ]] \
  || fail "hookless-single-fixture install_dir has $single_entry_count non-bookkeeping entries, expected exactly 1 (hookless-single-3.0.0); got: $(ls -A "$SINGLE_DIR")"

log "PASS: layout"

log "PASS: hookless_install_stages_own_archive"
