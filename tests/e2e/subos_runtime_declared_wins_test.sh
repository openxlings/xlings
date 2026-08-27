#!/usr/bin/env bash
# The subos runtime ABI contract: what a subos DECLARES outranks what the
# index calls newest.
#
# The defect this pins down: a subos records `glibc@2.44`, nothing is active
# yet on a fresh home, and the first dependency written `glibc@>=2.39`
# resolves through select_best -- which takes the MAXIMUM satisfying version
# and does not look at `latest`. The subos declared one libc and ran another.
#
# `latest` and "the highest entry in the table" are two different questions
# asked of the same table. They only look alike while nobody holds a version
# back, and the published index holds musl at `latest = 1.2.5` with 1.2.6 in
# the table -- so this is the shape of a real index, not an invented one.
#
# requires: python3
set -eu
set -o pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

# find_xlings_bin, not a hand-written path.
#
# The first version of this file guessed `target/x86_64-linux-gnu/bin/xlings`
# and skipped when it was absent -- which it always is, because builds land
# under `target/<triple>/<fingerprint>/bin/`. On CI that skip exited 0 and the
# runner printed `PASS: E2E-95 (5ms)`. Every assertion below had been written,
# reviewed and registered, and none of them ran.
#
# ⚠️ A skip that reads as a pass is worse than no test: it answers a question
# nobody asked while looking like the one that was.
BIN="$(find_xlings_bin)"
[ -n "$BIN" ] && [ -x "$BIN" ] \
  || { echo "[project-e2e] FAIL: no xlings binary found under target/" >&2; exit 1; }

RUNTIME_DIR="$(mktemp -d)"
trap 'rm -rf "$RUNTIME_DIR"' EXIT

log()  { echo "[project-e2e] $*"; }
fail() { echo "[project-e2e] FAIL: $*" >&2; exit 1; }

read_block() {  # <subos-dir> <python-expr over blk>
  python3 - "$1" "$2" <<'PY'
import json, sys, pathlib
d = json.loads(pathlib.Path(sys.argv[1], ".xlings.json").read_text())
blk = d.get("subos_info", {})
print(eval(sys.argv[2], {}, {"blk": blk}))
PY
}

log "client: $("$BIN" --version 2>&1 | head -1)"

# ── the fixture index ────────────────────────────────────────────────────
#
# A copy of the real fixture index with two runtime recipes written into it.
# Both are shaped the same way and neither version string appears anywhere
# else in the tree, so a manifest or a plan carrying one can only have
# resolved it.
#
#   glibc  latest = 7.7.7   table also holds 7.7.8   <- higher, held back
#   musl   latest = 6.6.6   table also holds 6.6.7   <- higher, held back
#
# 7.7.7/7.7.8 and 6.6.6/6.6.7 are chosen so that neither is a SUBSTRING of the
# other: `9.9.9` inside `9.9.99` would make the negative assertions below pass
# on the wrong reading.
INDEX="$RUNTIME_DIR/held-back-index"
cp -a "$ROOT_DIR/tests/fixtures/xim-pkgindex" "$INDEX"
python3 "$ROOT_DIR/tests/e2e/support/make_runtime_recipe.py" \
    "$INDEX/pkgs/g/glibc.lua" glibc linux-x86_64-glibc 7.7.7 7.7.8 \
  || fail "harness: could not write the glibc recipe"
python3 "$ROOT_DIR/tests/e2e/support/make_runtime_recipe.py" \
    "$INDEX/pkgs/m/musl.lua" musl linux-x86_64-musl 6.6.6 6.6.7 \
  || fail "harness: could not write the musl recipe"

new_home() {  # <dir>
  mkdir -p "$1/data/xim-index-repos"
  printf '{}\n' > "$1/data/xim-index-repos/xim-indexrepos.json"
  cat > "$1/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$INDEX" }
  ]
}
EOF
}

A1_HOME="$RUNTIME_DIR/home-a1"
new_home "$A1_HOME"
x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$A1_HOME" "$BIN" "$@" ) }

x self init >/dev/null 2>&1 || true
x update >/dev/null 2>&1 || true

# The harness has to actually answer, or every assertion below is measuring
# the fallback and calling it a pass.
INFO="$(x info xim:glibc 2>&1)" || true
printf '%s\n' "$INFO" | grep -q '7\.7\.7' \
  || fail "harness: the test index does not answer for glibc at all:
$INFO"
printf '%s\n' "$INFO" | grep -q '7\.7\.8' \
  || fail "harness: 7.7.8 is not in the table, so nothing here can pick the
    WRONG version and A1 would pass with the mechanism removed:
$INFO"
log "  ✓ harness: index answers latest=7.7.7 with 7.7.8 held back above it"

# ── A1: the declared runtime outranks the index's maximum ────────────────
#
# The window that matters is AFTER `self init` has written the binding and
# BEFORE anything is active -- tier 2 (pin-to-active) has nothing to say here,
# which is exactly why the declaration had to take part in resolution.
#
# `>=2.0` is satisfied by both 7.7.7 and 7.7.8, so the constraint does not
# decide; the subos's declaration does. Install cannot complete (the fixture
# URLs are unreachable by design) but the PLAN is printed before any download,
# and the plan is the decision this asserts on.
DEFAULT_DIR="$A1_HOME/subos/default"
[ -f "$DEFAULT_DIR/.xlings.json" ] || fail "A1: self init produced no default subos"

DECLARED="$(read_block "$DEFAULT_DIR" 'blk.get("runtime")')"
[ "$DECLARED" = "glibc@7.7.7" ] \
  || fail "A1: the subos declares '$DECLARED', expected glibc@7.7.7 -- the
    binding comes from the index's `latest`, so if this is wrong the rest of
    the test is measuring something else."

ACTIVE_BEFORE="$(read_block "$DEFAULT_DIR" 'blk.get("runtime")' 2>/dev/null || true)"
[ -n "$ACTIVE_BEFORE" ] || fail "A1: could not read the block back"

PLAN="$(x install 'glibc@>=2.0' 2>&1)" || true
case "$PLAN" in
  *7.7.8*) fail "A1: resolution chose 7.7.8 -- the index's MAXIMUM -- while
    this subos declares glibc@7.7.7. That is the defect: a subos running a
    libc it did not declare. Output was:
$PLAN" ;;
esac
case "$PLAN" in
  *7.7.7*) ;;
  *) fail "A1: resolution named neither version, so this asserts nothing.
    Output was:
$PLAN" ;;
esac
log "  ✓ A1: the declared runtime won over the index's maximum (7.7.7, not 7.7.8)"

# A6 -- the denominator. "7.7.7 appears" would also pass if BOTH appeared.
PLAN_VERSIONS="$(printf '%s\n' "$PLAN" | grep -oE '7\.7\.[78]' | sort -u | tr '\n' ' ')"
[ "$PLAN_VERSIONS" = "7.7.7 " ] \
  || fail "A6: the plan named versions '$PLAN_VERSIONS' -- exactly one is
    allowed. A subos holds exactly one runtime; two in one plan is the
    duplicate-binding shape that once had a GPU enumerated twice."
log "  ✓ A6: exactly one runtime version in the plan"

# ── A2: the rule does not know the word "glibc" ──────────────────────────
#
# The same assertion on a runtime that is not glibc. A fix that special-cases
# glibc passes A1 and fails here, which is the only reason this exists: the
# subos runtime has never been glibc-only -- RUNTIME_PACKAGES lists five, and
# musl is published today.
A2_HOME="$RUNTIME_DIR/home-a2"
new_home "$A2_HOME"
x2() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
         XLINGS_HOME="$A2_HOME" "$BIN" "$@" ) }
x2 self init >/dev/null 2>&1 || true
x2 update >/dev/null 2>&1 || true

OUT2="$(x2 subos new t-musl --runtime musl@6.6.6 2>&1)" || true
[ -f "$A2_HOME/subos/t-musl/.xlings.json" ] \
  || { echo "$OUT2" >&2; fail "A2: subos new left no manifest at all"; }

RUNTIME2="$(read_block "$A2_HOME/subos/t-musl" 'blk.get("runtime")')"
[ "$RUNTIME2" = "musl@6.6.6" ] \
  || fail "A2: the subos declares '$RUNTIME2', expected musl@6.6.6"

PLAN2="$( ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
            XLINGS_HOME="$A2_HOME" XLINGS_ACTIVE_SUBOS=t-musl \
            "$BIN" install 'musl@>=1.0' ) 2>&1 )" || true
case "$PLAN2" in
  *6.6.7*) fail "A2: resolution chose musl 6.6.7 -- the index's MAXIMUM --
    while that subos declares musl@6.6.6. A1 passing and this failing means
    the fix knows the string \"glibc\" rather than the rule. Output was:
$PLAN2" ;;
esac
case "$PLAN2" in
  *6.6.6*) ;;
  *) fail "A2: resolution named neither musl version, so this asserts
    nothing. Output was:
$PLAN2" ;;
esac
log "  ✓ A2: same rule holds for a runtime that is not glibc (musl 6.6.6)"

# ── A2b: the ABI came from the package, not from a table in this repo ────
#
# `family_of` in subos/manifest maps musl -> linux-<arch>-musl by a table
# here, while the recipe states it in the index. Recording what the package
# SAID is what lets a new runtime need no engine change; deriving it from the
# name means a new runtime that updates only the package becomes "unknown".
ABI2="$(read_block "$A2_HOME/subos/t-musl" 'blk.get("runtime_abi", "")')"
[ "$ABI2" = "linux-x86_64-musl" ] \
  || fail "A2b: recorded runtime_abi='$ABI2', expected the linux-x86_64-musl
    the recipe declares. Empty means the subos fell back to deriving it, and
    a derived ABI is a different fact from one the package stated."
log "  ✓ A2b: runtime_abi recorded from the package's own declaration"

# ── A3: a hosted runtime is not a missing payload ────────────────────────
#
# ucrt and macos_sdk are OS components -- "there is no ucrt payload to bind
# to". A checker that assumes every runtime is a package reports a healthy
# Windows subos as broken. Asserted here on the predicate's own terms, since
# this suite runs on Linux: a manifest declaring a hosted runtime with no
# payload on disk must NOT be reported as a mismatch.
A3_HOME="$RUNTIME_DIR/home-a3"
new_home "$A3_HOME"
x3() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
         XLINGS_HOME="$A3_HOME" "$BIN" "$@" ) }
x3 self init >/dev/null 2>&1 || true

HOSTED_DIR="$A3_HOME/subos/default"
python3 - "$HOSTED_DIR/.xlings.json" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text())
blk = d.setdefault("subos_info", {})
blk["runtime"] = "ucrt@10.0.26100.0"
blk["runtime_source"] = "explicit"
p.write_text(json.dumps(d, indent=2))
PY

DOCTOR3="$(x3 self doctor 2>&1)" || true
case "$DOCTOR3" in
  *ucrt*payload*|*payload*ucrt*) fail "A3: doctor reported the hosted runtime
    ucrt as a payload problem. There is no ucrt payload to find -- the OS
    supplies it -- so this is a defect invented by a checker that assumed
    every runtime is a package. Output was:
$DOCTOR3" ;;
esac
log "  ✓ A3: a hosted runtime is not reported as a missing payload"

# ── A4: moving the index cannot move an existing subos ───────────────────
#
# The operational promise: a runtime with a fix in it reaches an existing
# subos only when someone asks for it. An index update must never do it --
# swapping the libc under a subos is exactly the hazard of an INTERP and a
# RUNPATH coming from two different runtimes.
#
# Reuses the A1 home, which by now declares glibc@7.7.7. The index then moves
# `latest` to 7.7.8, which is the strongest form of the question: not merely
# "is a higher version present" but "is it the one the index now recommends".
# 7.7.7 STAYS in the table. Dropping it would test A5 (a declaration the
# index can no longer satisfy) while claiming to test A4 -- the two degrade
# differently and must not be conflated. Entries here are append-only in the
# real index for exactly this reason.
python3 "$ROOT_DIR/tests/e2e/support/make_runtime_recipe.py" \
    "$INDEX/pkgs/g/glibc.lua" glibc linux-x86_64-glibc 7.7.8 7.7.7 \
  || fail "A4: could not move the fixture index to 7.7.8"
x update >/dev/null 2>&1 || true

INFO4="$(x info xim:glibc 2>&1)" || true
printf '%s\n' "$INFO4" | grep -q '7\.7\.8' \
  || fail "A4: the index did not actually move, so this asserts nothing:
$INFO4"

STILL="$(read_block "$DEFAULT_DIR" 'blk.get("runtime")')"
[ "$STILL" = "glibc@7.7.7" ] \
  || fail "A4: the index moved to 7.7.8 and the subos now declares '$STILL'.
    An index update rewrote what a subos IS."

PLAN4="$(x install 'glibc@>=2.0' 2>&1)" || true
case "$PLAN4" in
  *7.7.8*) fail "A4: after the index moved, resolution chose 7.7.8 for a
    subos that still declares glibc@7.7.7. The declaration is what this
    subos's binaries are linked against; the index does not get a vote.
    Output was:
$PLAN4" ;;
esac
log "  ✓ A4: index moved to 7.7.8 and the subos stayed on its declared 7.7.7"

# ── A5: an unsatisfiable declaration degrades, it does not brick ─────────
#
# A subos can legitimately declare a version the index no longer offers -- it
# was withdrawn, or the home is offline. That must fall back to the lower
# tiers and say so, never hard-fail: the alternative bricks every subos whose
# runtime was withdrawn, which is a far larger blast radius than the defect
# this whole change addresses.
A5_HOME="$RUNTIME_DIR/home-a5"
new_home "$A5_HOME"
x5() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
         XLINGS_HOME="$A5_HOME" "$BIN" "$@" ) }
x5 self init >/dev/null 2>&1 || true
x5 update >/dev/null 2>&1 || true

python3 - "$A5_HOME/subos/default/.xlings.json" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text())
# A version this index has never offered and never will.
d.setdefault("subos_info", {})["runtime"] = "glibc@5.5.5"
p.write_text(json.dumps(d, indent=2))
PY

OUT5="$(x5 install 'glibc@>=2.0' 2>&1)" || true
case "$OUT5" in
  *"5.5.5"*"not found"*|*"not found"*"5.5.5"*)
    fail "A5: a declaration the index cannot satisfy turned into a hard
    'not found'. It has to degrade to the lower tiers instead -- otherwise
    withdrawing any runtime version bricks every subos that declared it.
    Output was:
$OUT5" ;;
esac
# It has to have RESOLVED something, or "no hard failure" is just silence.
case "$OUT5" in
  *7.7.8*) ;;
  *) fail "A5: resolution produced no version at all. Degrading means
    falling through to the index, not giving up. Output was:
$OUT5" ;;
esac
log "  ✓ A5: an unsatisfiable declaration fell through to the index (7.7.8)"

log "E2E subos-runtime-declared-wins: PASS"
