#!/usr/bin/env bash
# mcpp-community/mcpp#636, 2.5: `xlings self doctor` reports a payload whose top level
# still carries the fingerprint of the sweep defect (a foreign archive, or
# a download-cache `.lock`/`.meta` sidecar, left inside a package that
# never asked for a download of its own) -- and `--fix` repairs it by
# removing and reinstalling, which stages the package's own archive
# privately (installer.cpp) and ends up with exactly a fresh install's
# entries.
#
# The scenario a real swept payload looks like: install a hookless package
# normally (this binary already installs it clean -- that is what the rest
# of this PR is for), then hand-seed the two shapes of the fingerprint
# doctor's swept_payload_marker checks: a zero-length "<name>.lock" file
# paired with a sibling "<name>", and a file whose name ends in a
# download/archive extension. Both are exactly what the OLD
# stage_extracted_payload_ used to drag in from runtimedir; this test
# reproduces only their on-disk SHAPE, not the defect that used to
# produce them, so it needs no unpatched binary to demonstate the FIX --
# the fixture package installs cleanly here however it is built. It is
# doctor's REPAIR (repair_swept_'s remove-then-reinstall, RepairKind::
# SweptPayload's skip of R2 in repair.cpp) that this file actually puts
# to the test, and that code path did not exist before this PR at all.
#
# Refs: .agents/docs/2026-09-14-636-build-database-and-the-latest-xlings.md §2.5
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/doctor_swept_payload"
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

# swept-fixture: hookless, a loose top-level "bin/" (no wrapping
# directory, so install_dir/bin/tool lands directly and the assertions
# below stay simple) -- pre-placed in runtimedir (see
# hookless_install_stages_own_archive_test.sh for why: downloader.cpp's
# sha256 cache-hit path means the url is never fetched).
mkdir -p "$STAGE_DIR/bin"
printf 'tool\n' > "$STAGE_DIR/bin/tool"
tar -czf "$RUNTIMEDIR/swept-fixture.tar.gz" -C "$STAGE_DIR" bin
rm -rf "$STAGE_DIR/bin"
FIXTURE_SHA="$(sha256_of "$RUNTIMEDIR/swept-fixture.tar.gz")"

cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/s"

cat > "$LOCAL_INDEX_DIR/pkgs/s/swept-fixture.lua" <<LUA
package = {
    spec = "1", name = "swept-fixture", type = "package",
    description = "local fixture: a swept payload, for doctor_swept_payload_test.sh",
    authors = {"xlings-ci"}, licenses = {"MIT"}, status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = { url = "https://example.invalid/swept-fixture.tar.gz", sha256 = "$FIXTURE_SHA" } },
        macosx  = { ["1.0.0"] = { url = "https://example.invalid/swept-fixture.tar.gz", sha256 = "$FIXTURE_SHA" } },
        windows = { ["1.0.0"] = { url = "https://example.invalid/swept-fixture.tar.gz", sha256 = "$FIXTURE_SHA" } },
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

log "install swept-fixture@1.0.0 (this binary's own install is not under test here -- it always stages clean)"
out="$(RUN install swept-fixture@1.0.0 -y 2>&1)" \
  || { echo "$out"; fail "swept-fixture install failed"; }

PAYLOAD_DIR="$HOME_DIR/data/xpkgs/xim-x-swept-fixture/1.0.0"
[[ -f "$PAYLOAD_DIR/bin/tool" ]] || fail "setup: swept-fixture did not install its own payload"

log "D1: a clean install is not reported"
rc=0; clean_out="$(RUN self doctor 2>&1)" || rc=$?
[[ $rc -eq 0 ]] || { echo "$clean_out"; fail "D1: a clean install must not fail plain doctor; rc=$rc"; }
echo "$clean_out" | strip_ansi | grep -qi "swept" \
  && { echo "$clean_out"; fail "D1: a clean install must not be reported as swept"; }

log "D1.5: a BYSTANDER file -- looks like a download, has no .lock --"
log "  must NOT be flagged (the false positive the anchor rules out)"
# A package can legitimately carry a top-level file that merely LOOKS
# like a download (an installer asset, a bundled sub-archive) as part of
# its own, genuine archive. Without a paired .lock, xim::
# swept_payload_marker's anchor must never fire on a name alone.
printf 'legitimate payload content\n' > "$PAYLOAD_DIR/bundled-asset.tar.gz"
rc=0; bystander_out="$(RUN self doctor 2>&1)" || rc=$?
[[ $rc -eq 0 ]] \
  || { echo "$bystander_out"; fail "D1.5: a bystander download-shaped filename with no .lock must not fail doctor; rc=$rc"; }
echo "$bystander_out" | strip_ansi | grep -qi "swept" \
  && { echo "$bystander_out"; fail "D1.5: FALSE POSITIVE -- a lone bundled-asset.tar.gz with no .lock must not be reported as swept"; }

log "seeding the sweep fingerprint: two shapes, both anchored on a"
log "  zero-length .lock (downloader.cpp writes .lock only in runtimedir)"
# Shape 1: an ORPHAN lock -- the archive side already moved or the
# download never finished, but the lock's own base name ends in a
# download extension, e.g. "glibc-2.44.2-linux-x86_64.tar.gz.lock" with
# no "glibc-2.44.2-linux-x86_64.tar.gz" beside it.
: > "$PAYLOAD_DIR/other-pkg-2.1.0.tar.gz.lock"
# Shape 2: a zero-length "<name>.lock" paired with a sibling "<name>" --
# exactly what the downloader's FileLock leaves in runtimedir, and ONLY
# in runtimedir (downloader.cpp), so one inside a payload is unexplained
# by anything this package's own install produces.
mkdir -p "$PAYLOAD_DIR/another-package-9.9.9"
printf 'leftover\n' > "$PAYLOAD_DIR/another-package-9.9.9/leftover.txt"
: > "$PAYLOAD_DIR/another-package-9.9.9.lock"

log "D2: self doctor reports the swept payload"
rc=0; out="$(RUN self doctor 2>&1)" || rc=$?
[[ $rc -ne 0 ]] \
  || fail "D2: a swept payload must fail plain doctor; got rc=0:\n$out"
echo "$out" | strip_ansi | grep -qi "swept payload" \
  || fail "D2: the report must name the finding 'swept payload'; got:\n$out"
echo "$out" | strip_ansi | grep -q "swept-fixture" \
  || fail "D2: the report must name swept-fixture; got:\n$out"

log "D3: self doctor --fix -y removes and reinstalls the payload"
rc=0; fixout="$(RUN self doctor --fix -y 2>&1)" || rc=$?
echo "$fixout" | tail -30 | sed 's/^/    | /'
[[ $rc -eq 0 ]] \
  || fail "D3: --fix should leave the home clean afterward; got rc=$rc:\n$fixout"

log "D4: the directory now equals a fresh install"
[[ -f "$PAYLOAD_DIR/bin/tool" ]] \
  || fail "D4: swept-fixture's own file (bin/tool) did not survive the repair"
[[ ! -e "$PAYLOAD_DIR/bundled-asset.tar.gz" ]] \
  || fail "D4: the bystander file survived --fix (it was never part of swept-fixture's own archive)"
[[ ! -e "$PAYLOAD_DIR/other-pkg-2.1.0.tar.gz.lock" ]] \
  || fail "D4: the orphan .lock survived --fix"
[[ ! -e "$PAYLOAD_DIR/another-package-9.9.9" ]] \
  || fail "D4: the foreign directory survived --fix"
[[ ! -e "$PAYLOAD_DIR/another-package-9.9.9.lock" ]] \
  || fail "D4: the .lock sidecar survived --fix"

fresh_entries="$(ls -A "$PAYLOAD_DIR" | grep -v -E '^(\.xpkg-install\.json|\.xpkg\.lua|\.xlings-resolution\.json)$' | wc -l | tr -d ' ')"
[[ "$fresh_entries" == "1" ]] \
  || fail "D4: swept-fixture's install_dir has $fresh_entries non-bookkeeping entries after --fix, expected exactly 1 (bin); got: $(ls -A "$PAYLOAD_DIR")"

log "D5: a second doctor run reports nothing left to fix (convergence)"
rc=0; secondout="$(RUN self doctor 2>&1)" || rc=$?
[[ $rc -eq 0 ]] \
  || fail "D5: doctor must be clean after --fix already repaired it; got rc=$rc:\n$secondout"
echo "$secondout" | strip_ansi | grep -qi "swept" \
  && fail "D5: no swept-payload finding should remain; got:\n$secondout"

log "PASS: doctor_swept_payload"
