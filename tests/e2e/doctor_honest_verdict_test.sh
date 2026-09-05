#!/usr/bin/env bash
# E2E: doctor tells the truth about what it did, and can see the library farm.
#
# Four defects from issue #583, all reachable WITHOUT the unsupported `mv`
# that the issue was filed about. Moving a home is not supported and is not
# what this tests; these are what that report surfaced on the way.
#
#   S1  a read-only home must not abort the process. `self doctor --fix` used
#       to throw from the home-config writer, find no handler -- `self` is
#       dispatched before cli.cpp's top-level try -- and die with SIGABRT
#       *after* printing a full report (exit 134).
#   S2  a run that dropped registrations must not call the home "OK". Prune is
#       legitimate; claiming consistency about a home made consistent by
#       deleting the inconsistent parts is not.
#   S3  a dangling link in <subos>/lib whose source the versions DB knows must
#       be RE-POINTED, not deleted. The farm was outside the scan entirely
#       until 2026.9.4.1, so nothing here was ever detected, let alone fixed.
#   S4  a dangling link nothing can place is still deleted.
#   S5  the summary's counts must match the list they summarise: a subos
#       manifest defect is not a "broken payload".
#
# No package installs and no network: every fixture is written by hand, which
# is also why the assertions can be exact.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/doctor_honest_verdict"
cleanup() { chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true; rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

XLINGS_BIN="$(find_xlings_bin)"

# Pinned, and not for speed. The default lock timeout is ten minutes; a
# fixture that cannot take the lock (S1's home is read-only, so the lock file
# cannot be created) would otherwise make this test look hung rather than
# failed.
RUN() {
  local home="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$home" XLINGS_LOCK_TIMEOUT=30 "$XLINGS_BIN" "$@" )
}

# A home with one registration and nothing else. `payload` decides whether the
# registered package exists on disk.
make_home() {
  local home="$1" payload="$2"
  mkdir -p "$home/bin" "$home/subos/default/bin" "$home/subos/default/lib" \
           "$home/subos/default/usr/include" "$home/data/xpkgs"
  if [[ "$payload" == "present" ]]; then
    mkdir -p "$home/data/xpkgs/xim-x-ghost/1.0.0/bin"
    printf '#!/bin/sh\necho ghost\n' > "$home/data/xpkgs/xim-x-ghost/1.0.0/bin/ghost"
    chmod +x "$home/data/xpkgs/xim-x-ghost/1.0.0/bin/ghost"
  fi
  python3 - "$home" <<'PY'
import json, os, sys
home = sys.argv[1]
json.dump({"activeSubos": "default", "version": "0.0.0", "mirror": "CN",
           "versions": {"ghost": {"filename": "ghost", "type": "program",
               "versions": {"1.0.0": {"path": os.path.join(
                   home, "data/xpkgs/xim-x-ghost/1.0.0")}}}}},
          open(os.path.join(home, ".xlings.json"), "w"), indent=2)
json.dump({"subos_info": {"created_by": "e2e"},
           "workspace": {"ghost": {"active": "1.0.0", "installed": ["1.0.0"]}}},
          open(os.path.join(home, "subos/default/.xlings.json"), "w"), indent=2)
PY
}

# ── S1 · a read-only home is reported, not aborted ──────────────────────────
log "S1: read-only home must not abort"
S1="$RUNTIME_DIR/s1"
# The payload must be PRESENT. The stamp that used to abort is written only
# when the run has nothing outstanding, and a missing payload leaves a prune
# the read-only home cannot perform -- so the fixture never reaches the crash
# and the test passes against the very binary it is meant to catch. Verified
# by running this file against 2026.9.3.2: with `missing` it passed, with
# `present` it exits 134.
make_home "$S1" present
chmod -R a-w "$S1"
set +e
S1_OUT="$(RUN "$S1" self doctor --fix 2>&1)"; S1_RC=$?
set -e
chmod -R u+w "$S1"
# The invariant is "did not die", not a particular code: what the run finds in
# a home it cannot write is allowed to change.
case "$S1_RC" in
  0|1|2) : ;;
  *) printf '%s\n' "$S1_OUT" >&2
     fail "S1: read-only home exited $S1_RC (134 = the SIGABRT this fixes)" ;;
esac
grep -q 'terminate called' <<<"$S1_OUT" \
  && fail "S1: an exception still escaped to std::terminate"
log "S1 ok (exit $S1_RC, no terminate)"

# ── S2 · a prune is not an "OK" ─────────────────────────────────────────────
log "S2: dropped registrations must not read as a clean home"
S2="$RUNTIME_DIR/s2"
make_home "$S2" missing
cp "$XLINGS_BIN" "$S2/bin/xlings"
S2_OUT="$(RUN "$S2" self doctor --fix 2>&1 || true)"
grep -q 'dropped' <<<"$S2_OUT" || { printf '%s\n' "$S2_OUT" >&2
  fail "S2: the fixture did not reach a prune; the test proves nothing"; }
grep -q 'OK — workspace, shims, and payloads are all consistent' <<<"$S2_OUT" \
  && { printf '%s\n' "$S2_OUT" >&2
       fail "S2: a run that dropped a registration still reported OK"; }
grep -q 'registration(s) dropped' <<<"$S2_OUT" \
  || { printf '%s\n' "$S2_OUT" >&2
       fail "S2: the verdict does not name what was lost"; }
log "S2 ok"

# ── S3 / S4 · the library farm is seen, and repaired the right way ──────────
if [[ "${OSTYPE:-}" == "msys" || "${OSTYPE:-}" == "cygwin" || -n "${WINDIR:-}" ]]; then
  # Not a silent pass. On Windows the farm is hardlinks/copies (create_link_),
  # so "a dangling symlink" is not a state this platform reaches, and a test
  # that pretended to check it would be checking nothing.
  log "S3/S4 skipped: the library farm is not symlinks on this platform"
else
  log "S3: a dangling library link the DB can place must be re-pointed"
  S3="$RUNTIME_DIR/s3"
  mkdir -p "$S3/bin" "$S3/subos/default/bin" "$S3/subos/default/lib" \
           "$S3/data/xpkgs/xim-x-demolib/1.0.0/lib"
  cp "$XLINGS_BIN" "$S3/bin/xlings"
  : > "$S3/data/xpkgs/xim-x-demolib/1.0.0/lib/libdemo.so.1"
  python3 - "$S3" <<'PY'
import json, os, sys
home = sys.argv[1]
libdir = os.path.join(home, "data/xpkgs/xim-x-demolib/1.0.0/lib")
json.dump({"activeSubos": "default", "version": "0.0.0", "mirror": "CN",
           "versions": {"libdemo.so.1": {"filename": "libdemo.so.1",
               "type": "lib", "versions": {"1.0.0": {
                   "kind": "lib", "path": libdir,
                   "sourceName": "libdemo.so.1",
                   "destinationName": "libdemo.so.1"}}}}},
          open(os.path.join(home, ".xlings.json"), "w"), indent=2)
json.dump({"subos_info": {"created_by": "e2e"},
           "workspace": {"libdemo.so.1": {"active": "1.0.0",
                                          "installed": ["1.0.0"]}}},
          open(os.path.join(home, "subos/default/.xlings.json"), "w"), indent=2)
PY
  # The measured freetype shape: the link names a directory inside the payload
  # that the payload does not have, while the record is correct.
  ln -s "$S3/data/xpkgs/xim-x-demolib/1.0.0/lib/x86_64-linux-musl/libdemo.so.1" \
        "$S3/subos/default/lib/libdemo.so.1"
  # And one nothing can place.
  ln -s /nowhere/liborphan.so.9 "$S3/subos/default/lib/liborphan.so.9"

  DETECT="$(RUN "$S3" self doctor --all 2>&1 || true)"
  grep -q 'libdemo.so.1 points at' <<<"$DETECT" \
    || { printf '%s\n' "$DETECT" >&2
         fail "S3: the library farm is still outside the dangling scan"; }

  FIX="$(RUN "$S3" self doctor --fix 2>&1 || true)"
  grep -q 'link repointed' <<<"$FIX" \
    || { printf '%s\n' "$FIX" >&2; fail "S3: the link was not re-pointed"; }
  [[ -e "$S3/subos/default/lib/libdemo.so.1" ]] \
    || fail "S3: the re-pointed link still does not resolve"
  [[ "$(readlink "$S3/subos/default/lib/libdemo.so.1")" \
       == "$S3/data/xpkgs/xim-x-demolib/1.0.0/lib/libdemo.so.1" ]] \
    || fail "S3: re-pointed at the wrong source"
  # The payload is the thing a repair must never edit.
  [[ -f "$S3/data/xpkgs/xim-x-demolib/1.0.0/lib/libdemo.so.1" ]] \
    || fail "S3: the payload was disturbed"
  log "S3 ok"

  log "S4: a dangling link nothing can place is still removed"
  [[ ! -e "$S3/subos/default/lib/liborphan.so.9" \
     && ! -L "$S3/subos/default/lib/liborphan.so.9" ]] \
    || fail "S4: an unplaceable dangling link survived --fix"
  log "S4 ok"
fi

# ── S5 · the counts must match the list ─────────────────────────────────────
log "S5: a subos defect is not counted as a broken payload"
S5="$RUNTIME_DIR/s5"
make_home "$S5" present
S5_OUT="$(RUN "$S5" self doctor 2>&1 || true)"
if grep -q 'subos manifest' <<<"$S5_OUT"; then
  grep -qE 'subos issues +[0-9]' <<<"$S5_OUT" \
    || { printf '%s\n' "$S5_OUT" >&2
         fail "S5: a subos finding is not counted under its own label"; }
  log "S5 ok"
else
  log "S5 skipped: this fixture produced no subos finding to classify"
fi

log "PASS: doctor_honest_verdict"
