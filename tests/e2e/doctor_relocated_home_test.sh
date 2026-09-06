#!/usr/bin/env bash
# E2E: a home that was moved is diagnosed and re-pointed, never destroyed.
#
# Issue #583. Moving a home with `mv` is not a supported operation and this
# does not make it one -- payload contents (PT_INTERP, linker scripts) still
# name the old root afterwards. What it tests is the other half: that
# `self doctor --fix` stops destroying the bookkeeping it could repair.
#
# Measured on 2026.9.4.1, on a slice of a real 152 GB home moved with `mv`:
# one `--fix` run deleted 1173 sysroot links and dropped 367 registrations in
# 42 seconds, with all 234 package directories present under the new root, and
# said `its payload is gone and nothing can restore it` about each of them.
#
#   R1  a moved home is NAMED as moved. Before this, the words "moved" and
#       "relocated" appeared zero times in a moved home's report.
#   R2  `--fix` re-points records and links; it deletes and prunes nothing.
#   R3  a payload that is genuinely gone is STILL pruned. The gate is "the
#       payload is present under the current root", not "this home moved" --
#       getting that backwards would turn a data-loss bug into a
#       never-converges bug.
#   R4  `--fix --dry-run` plans what `--fix` does, and changes nothing. It
#       used to skip the local repairs entirely: 411 actions planned, 1173
#       deletions performed.
#   R5  a declared sysroot link that is MISSING is detected and placed back --
#       the only way a home damaged by an older client can be repaired.
#   R6  a moved home reached through a compensating symlink at the old path is
#       repaired too. That workaround is what users reach for, and it left
#       every record and 866 links naming a path that only exists because of
#       the symlink.
#
# No package installs and no network: every fixture is written by hand.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/doctor_relocated_home"
cleanup() { chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true; rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

XLINGS_BIN="$(find_xlings_bin)"

if [[ "${OSTYPE:-}" == "msys" || "${OSTYPE:-}" == "cygwin" || -n "${WINDIR:-}" ]]; then
  # Not a silent pass. On Windows the sysroot is hard links and copies, which
  # survive a move of the tree they live in, so the link half of this has no
  # meaning there. The record half does, but a partial fixture would assert
  # less than it appears to.
  log "SKIP: the sysroot is not symlinks on this platform"
  log "PASS: doctor_relocated_home (skipped)"
  exit 0
fi

# Pinned for the same reason the sibling test pins it: the default lock
# timeout is ten minutes, and a fixture that cannot take the lock would look
# hung rather than failed.
RUN() {
  local home="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$home" XLINGS_LOCK_TIMEOUT=30 "$XLINGS_BIN" "$@" )
}

# A home with one library (farm link + header link) and one program.
make_home() {
  local home="$1"
  mkdir -p "$home/bin" \
           "$home/subos/default/bin" "$home/subos/default/lib" \
           "$home/subos/default/usr/include" \
           "$home/data/xpkgs/xim-x-demolib/1.0.0/lib" \
           "$home/data/xpkgs/xim-x-demolib/1.0.0/include" \
           "$home/data/xpkgs/xim-x-demoprog/2.0.0/bin"
  cp "$XLINGS_BIN" "$home/bin/xlings"
  : > "$home/data/xpkgs/xim-x-demolib/1.0.0/lib/libdemo.so.1"
  echo '#define DEMO 1' > "$home/data/xpkgs/xim-x-demolib/1.0.0/include/demo.h"
  printf '#!/bin/sh\necho demo\n' > "$home/data/xpkgs/xim-x-demoprog/2.0.0/bin/demoprog"
  chmod +x "$home/data/xpkgs/xim-x-demoprog/2.0.0/bin/demoprog"
  ln -sfn "$home/data/xpkgs/xim-x-demolib/1.0.0/lib/libdemo.so.1" \
          "$home/subos/default/lib/libdemo.so.1"
  ln -sfn "$home/data/xpkgs/xim-x-demolib/1.0.0/include/demo.h" \
          "$home/subos/default/usr/include/demo.h"
  python3 - "$home" <<'PY'
import json, os, sys
home = sys.argv[1]
store = os.path.join(home, "data/xpkgs")
json.dump({
    "activeSubos": "default", "version": "0.0.0", "mirror": "CN",
    "versions": {
        "libdemo.so.1": {
            "filename": "libdemo.so.1", "type": "lib",
            "versions": {"1.0.0": {
                "kind": "lib",
                "path": os.path.join(store, "xim-x-demolib/1.0.0/lib"),
                "sourceName": "libdemo.so.1",
                "destinationName": "libdemo.so.1"}}},
        "demo.h": {
            "filename": "demo.h", "type": "files",
            "versions": {"1.0.0": {
                "kind": "files",
                "path": os.path.join(store, "xim-x-demolib/1.0.0"),
                "fileSrc": "include/demo.h",
                "fileDst": "usr/include/demo.h"}}},
        "demoprog": {
            "filename": "demoprog", "type": "program",
            "versions": {"2.0.0": {
                "path": os.path.join(store, "xim-x-demoprog/2.0.0/bin")}}},
    }}, open(os.path.join(home, ".xlings.json"), "w"), indent=2)
json.dump({
    "subos_info": {"schema_version": 1, "created_at": "2026-09-06T00:00:00Z",
                   "created_by": "e2e", "envs": {}},
    "workspace": {
        "libdemo.so.1": {"active": "1.0.0", "installed": ["1.0.0"]},
        "demo.h":       {"active": "1.0.0", "installed": ["1.0.0"]},
        "demoprog":     {"active": "2.0.0", "installed": ["2.0.0"]}}},
    open(os.path.join(home, "subos/default/.xlings.json"), "w"), indent=2)
PY
}

# How many registrations does this home still hold?
entries_of() {
  python3 - "$1" <<'PY'
import json, os, sys
db = json.load(open(os.path.join(sys.argv[1], ".xlings.json")))["versions"]
print(sum(len(i.get("versions", {})) for i in db.values()))
PY
}

# Does any record still name the old root?
records_naming() {
  python3 - "$1" "$2" <<'PY'
import json, os, sys
text = open(os.path.join(sys.argv[1], ".xlings.json")).read()
print(text.count(sys.argv[2]))
PY
}

# ── R1 / R2 · named, and repaired rather than destroyed ─────────────────────
log "R1/R2: a moved home is named as moved, and nothing is destroyed"
OLD="$RUNTIME_DIR/r2_old"
NEW="$RUNTIME_DIR/r2_new"
make_home "$OLD"
BEFORE_ENTRIES="$(entries_of "$OLD")"
mv "$OLD" "$NEW"

R1_OUT="$(RUN "$NEW" self doctor 2>&1 || true)"
grep -qi 'home relocated' <<<"$R1_OUT" \
  || { printf '%s\n' "$R1_OUT" >&2
       fail "R1: a moved home is not named as moved"; }
grep -q "$OLD" <<<"$R1_OUT" \
  || { printf '%s\n' "$R1_OUT" >&2
       fail "R1: the report does not say WHERE the home was"; }
# The remedy must not leave a false impression about payload contents.
grep -q 'PT_INTERP' <<<"$R1_OUT" \
  || { printf '%s\n' "$R1_OUT" >&2
       fail "R1: the remedy does not say what it cannot repair"; }
# The symptoms it stands in for must not be reported as separate defects.
grep -q 'broken payload' <<<"$R1_OUT" \
  && { printf '%s\n' "$R1_OUT" >&2
       fail "R1: a present payload is still reported as broken"; }

R2_OUT="$(RUN "$NEW" self doctor --fix 2>&1 || true)"
grep -q 'dangling link removed' <<<"$R2_OUT" \
  && { printf '%s\n' "$R2_OUT" >&2
       fail "R2: --fix deleted a link whose target is present under the new root"; }
grep -q 'nothing can restore it' <<<"$R2_OUT" \
  && { printf '%s\n' "$R2_OUT" >&2
       fail "R2: --fix dropped a registration whose payload is on disk"; }
grep -q 'records re-pointed' <<<"$R2_OUT" \
  || { printf '%s\n' "$R2_OUT" >&2
       fail "R2: the records were not re-pointed at the current root"; }
[[ "$(entries_of "$NEW")" == "$BEFORE_ENTRIES" ]] \
  || fail "R2: registrations went from $BEFORE_ENTRIES to $(entries_of "$NEW")"
[[ "$(records_naming "$NEW" "$OLD")" == "0" ]] \
  || fail "R2: $(records_naming "$NEW" "$OLD") record(s) still name the old root"
for link in "$NEW/subos/default/lib/libdemo.so.1" \
            "$NEW/subos/default/usr/include/demo.h"; do
  [[ -L "$link" ]] || fail "R2: $link was deleted"
  [[ -e "$link" ]] || fail "R2: $link is still dangling after --fix"
  case "$(readlink "$link")" in
    "$NEW"/*) : ;;
    *) fail "R2: $link still points at $(readlink "$link")" ;;
  esac
done
log "R1/R2 ok"

# ── R3 · a payload that is really gone is still pruned ──────────────────────
log "R3: a genuinely missing payload is still pruned"
R3="$RUNTIME_DIR/r3"
make_home "$R3"
rm -rf "$R3/data/xpkgs/xim-x-demoprog"
R3_OUT="$(RUN "$R3" self doctor --fix 2>&1 || true)"
grep -q 'dropped' <<<"$R3_OUT" \
  || { printf '%s\n' "$R3_OUT" >&2
       fail "R3: the gate also turned off pruning for a payload that is gone"; }
log "R3 ok"

# ── R4 · dry run plans what the real run does, and changes nothing ──────────
log "R4: --dry-run previews the local repairs and mutates nothing"
R4_OLD="$RUNTIME_DIR/r4_old"
R4_NEW="$RUNTIME_DIR/r4_new"
make_home "$R4_OLD"
mv "$R4_OLD" "$R4_NEW"
R4_BEFORE="$(find "$R4_NEW/subos" -type l | wc -l)"
R4_OUT="$(RUN "$R4_NEW" self doctor --fix --dry-run 2>&1 || true)"
grep -q 'would run' <<<"$R4_OUT" \
  || { printf '%s\n' "$R4_OUT" >&2; fail "R4: the dry run planned nothing"; }
grep -q 're-point' <<<"$R4_OUT" \
  || { printf '%s\n' "$R4_OUT" >&2
       fail "R4: the dry run does not mention the re-pointing --fix performs"; }
grep -q 'registration was dropped' <<<"$R4_OUT" \
  && { printf '%s\n' "$R4_OUT" >&2
       fail "R4: the dry run reports a drop in the past tense"; }
[[ "$(find "$R4_NEW/subos" -type l | wc -l)" == "$R4_BEFORE" ]] \
  || fail "R4: --dry-run changed the sysroot"
[[ "$(records_naming "$R4_NEW" "$R4_OLD")" != "0" ]] \
  || fail "R4: --dry-run rewrote the records"
log "R4 ok"

# ── R5 · a declared link that is missing is placed back ─────────────────────
log "R5: a missing declared sysroot link is detected and placed"
R5="$RUNTIME_DIR/r5"
make_home "$R5"
rm -f "$R5/subos/default/lib/libdemo.so.1" "$R5/subos/default/usr/include/demo.h"
R5_SEEN="$(RUN "$R5" self doctor 2>&1 || true)"
grep -q 'missing sysroot link' <<<"$R5_SEEN" \
  || { printf '%s\n' "$R5_SEEN" >&2
       fail "R5: a deleted farm link is invisible to doctor"; }
RUN "$R5" self doctor --fix >/dev/null 2>&1 || true
[[ -e "$R5/subos/default/lib/libdemo.so.1" ]] \
  || fail "R5: the library link was not placed back"
[[ -e "$R5/subos/default/usr/include/demo.h" ]] \
  || fail "R5: the header link was not placed back"
log "R5 ok"

# ── R6 · the compensating-symlink workaround is repaired too ────────────────
log "R6: a moved home reached through a symlink at the old path"
R6_OLD="$RUNTIME_DIR/r6_old"
R6_NEW="$RUNTIME_DIR/r6_new"
make_home "$R6_OLD"
mv "$R6_OLD" "$R6_NEW"
ln -sfn "$R6_NEW" "$R6_OLD"
R6_OUT="$(RUN "$R6_NEW" self doctor --fix 2>&1 || true)"
grep -q 'dangling link removed' <<<"$R6_OUT" \
  && { printf '%s\n' "$R6_OUT" >&2; fail "R6: --fix deleted a resolvable link"; }
[[ "$(records_naming "$R6_NEW" "$R6_OLD")" == "0" ]] \
  || fail "R6: records still depend on the compensating symlink"
for link in "$R6_NEW/subos/default/lib/libdemo.so.1" \
            "$R6_NEW/subos/default/usr/include/demo.h"; do
  case "$(readlink "$link")" in
    "$R6_NEW"/*) : ;;
    *) fail "R6: $link still points through the old path" ;;
  esac
done
log "R6 ok"

log "PASS: doctor_relocated_home"
