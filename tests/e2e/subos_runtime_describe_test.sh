#!/usr/bin/env bash
# E2E: describing an existing subos must never invent what it runs.
#
# openxlings/xlings#547. Six places wrote the `subos_info` block; five asked
# one helper and the sixth wrote the DEFAULT_RUNTIME constant directly. On a
# measured real home that produced two subos declaring `glibc@2.44` while
# `lib/libc.so.6` pointed into the 2.39 payload -- and nothing anywhere
# noticed, because every check compared one record we wrote against another
# record we wrote.
#
# The distinction the fix rests on:
#
#   Create    `subos new`, where a human could have said `--runtime`.
#             The default means "they didn't say", so it is legitimate.
#   Describe  everything else. `xlings install` has no `--runtime` flag
#             anywhere in the tree, so the constant would record "the user
#             took the default" about a question nobody was asked.
#
# Scenarios (each falsifiable, and S1/S3 are red before the fix):
#   S1  a workspace that names the runtime is what gets declared, not 2.44
#   S2  no evidence at all -> the `runtime` key is ABSENT, doctor says so,
#       and the exit code stays 0 (an honest unknown is not a defect)
#   S3  the sysroot symlink answers when the workspace does not
#   S4  a declaration that contradicts the sysroot is REPORTED and `--fix`
#       does not silently rewrite it
#   S5  `--fix` converges: the manifest stops changing
#   S6  describing does not fabricate a creation date
#
# S2 is the one that could not exist before: I6 required a well-formed
# binding, so "unknown" was inexpressible and a guess got written instead.
#
# The CREATE half is E2E-67 (`subos_runtime_binding_test.sh`) and is not
# duplicated here. It is the other side of the same rule and it earns its
# keep: making `self init` unconditionally Describe -- which looks right,
# since it runs on install and update -- leaves a brand-new home's `default`
# with no runtime at all. E2E-67/S1 caught exactly that while this file was
# green, which is the argument for both existing.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$(runtime_home_dir subos_runtime_describe)"
cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"
log "using $XLINGS_BIN"

FAILURES=()
note_fail() { FAILURES+=("$1"); }

# A home with one subos, whose `.xlings.json` is whatever the caller says and
# whose sysroot optionally carries a libc symlink into a store payload.
#
# Deliberately NOT seeded from the real home: a seeded home makes the index a
# symlink and turns differentials into unfalsifiable passes.
make_home() {
  local home="$1" subos_json="$2" libc_version="${3:-}"
  rm -rf "$home"
  mkdir -p "$home/subos/default"/{bin,lib,usr,generations} "$home/bin" \
           "$home/data/xpkgs"
  assert_home_is_isolated "$home"
  printf '{"activeSubos":"default","subos":{"default":{"dir":""}}}' \
      > "$home/.xlings.json"
  printf '%s' "$subos_json" > "$home/subos/default/.xlings.json"
  if [[ -n "$libc_version" ]]; then
    local payload="$home/data/xpkgs/xim-x-glibc/$libc_version/lib64"
    mkdir -p "$payload"
    : > "$payload/libc.so.6"
    ln -s "$payload/libc.so.6" "$home/subos/default/lib/libc.so.6"
  fi
}

doctor() {
  local home="$1"; shift
  # XLINGS_LOCK_TIMEOUT pinned: the product default is minutes, and a test
  # that waits that long on a refusal reads as a hang.
  ( cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$home" \
      XLINGS_LOCK_TIMEOUT=30 "$XLINGS_BIN" self doctor "$@" 2>&1 )
}

block_field() {
  python3 - "$1" "$2" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
block = doc.get("subos_info")
if block is None:
    print("<no-block>")
else:
    print(block.get(sys.argv[2], "<absent>"))
PY
}

# ── S1: an upgraded home declares what it runs, not the current default ──
#
# The #547 regression guard. 17 subos on the measured home have this shape --
# a workspace naming the runtime and a sysroot serving it -- and every one was
# declared `glibc@2.44` by the first install that touched it.
#
# Both evidence sources agree here on purpose: that is what a real upgraded
# home looks like, and a fixture that made them disagree would be testing
# PRECEDENCE, which is a question about ordering rather than about what the
# user sees. Precedence is pinned exactly in the unit tests
# (SubosRuntimeFor.*), where no other repair can move the inputs underneath
# the assertion -- doctor legitimately deactivates a runtime that is not
# registered, and an e2e fixture cannot register one without installing it.
H1="$RUNTIME_DIR/s1"
make_home "$H1" '{"workspace":{"glibc":{"active":"2.39","installed":["2.39"]}}}' 2.39
doctor "$H1" --fix >/dev/null 2>&1 || true
S1_RUNTIME="$(block_field "$H1/subos/default/.xlings.json" runtime)"
[[ "$S1_RUNTIME" == "glibc@2.39" ]] \
  || note_fail "S1: a subos running 2.39 was declared '$S1_RUNTIME'"
# Stated separately, because it is the specific wrong answer this exists to
# stop and it must not be satisfiable by "absent".
#
# The default is READ from the source rather than written here. This line used
# to say "glibc@2.44", and the day that constant moved to 2.44.2 the assertion
# would still have passed -- against a value that is no longer the default, so
# it would no longer have been testing "doctor invented the default". The
# sibling test subos_runtime_binding_test.sh pins the literal on purpose: it
# asserts WHAT the default is. This one asserts the default is not used, so it
# has to follow the default.
CURRENT_DEFAULT="$(sed -n 's/.*DEFAULT_RUNTIME_FALLBACK = "\(.*\)".*/\1/p' \
    "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/src/core/subos/manifest.cppm" \
    | head -1)"
[[ -n "$CURRENT_DEFAULT" ]] \
  || note_fail "S1: could not read DEFAULT_RUNTIME_FALLBACK out of manifest.cppm"
[[ "$S1_RUNTIME" != "$CURRENT_DEFAULT" ]] \
  || note_fail "S1: the subos was re-declared against the current default ($CURRENT_DEFAULT)"

# ── S2: nothing to observe -> an ABSENT runtime key, and exit 0 ──────────
H2="$RUNTIME_DIR/s2"
make_home "$H2" '{"workspace":{}}'
doctor "$H2" --fix >/dev/null 2>&1 || true
S2_RUNTIME="$(block_field "$H2/subos/default/.xlings.json" runtime)"
[[ "$S2_RUNTIME" == "<absent>" ]] \
  || note_fail "S2: with no evidence the runtime should be absent, got '$S2_RUNTIME'"
S2_SCHEMA="$(block_field "$H2/subos/default/.xlings.json" schema_version)"
[[ "$S2_SCHEMA" != "<no-block>" ]] \
  || note_fail "S2: the block itself must exist -- 'old format' and 'known unknown' have to be distinguishable"

S2_OUT="$(doctor "$H2")"
S2_RC=0
( cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$H2" \
    XLINGS_LOCK_TIMEOUT=30 "$XLINGS_BIN" self doctor >/dev/null 2>&1 ) || S2_RC=$?
[[ "$S2_RC" -eq 0 ]] \
  || note_fail "S2: an honest unknown made doctor exit $S2_RC; it is not a defect"
grep -qi "does not record a runtime" <<<"$S2_OUT" \
  || note_fail "S2: doctor said nothing about the unknown runtime: $S2_OUT"

# ── S3: the sysroot answers when the workspace cannot ────────────────────
H3="$RUNTIME_DIR/s3"
make_home "$H3" '{"workspace":{}}' 2.39
doctor "$H3" --fix >/dev/null 2>&1 || true
S3_RUNTIME="$(block_field "$H3/subos/default/.xlings.json" runtime)"
[[ "$S3_RUNTIME" == "glibc@2.39" ]] \
  || note_fail "S3: lib/libc.so.6 points into the 2.39 payload; declared '$S3_RUNTIME'"

# ── S4: a declaration the sysroot contradicts is reported, not rewritten ─
#
# This is the real-home shape (`mcpp-test`, `agent-influence`): a block that
# says 2.44 over a sysroot serving 2.39, with an empty workspace so D5 has
# nothing to compare against and stays silent.
H4="$RUNTIME_DIR/s4"
make_home "$H4" '{"workspace":{},"subos_info":{"schema_version":1,"runtime":"glibc@2.44","envs":{},"created_at":"2026-08-15T14:31:16Z","created_by":"xlings test"}}' 2.39
S4_OUT="$(doctor "$H4")"
grep -qi "drift" <<<"$S4_OUT" \
  || note_fail "S4: a declaration contradicting the sysroot was not reported: $S4_OUT"
doctor "$H4" --fix >/dev/null 2>&1 || true
S4_RUNTIME="$(block_field "$H4/subos/default/.xlings.json" runtime)"
# Which of the two is the accident is not decidable from here, and rewriting
# the declaration changes what the subos claims to BE.
[[ "$S4_RUNTIME" == "glibc@2.44" ]] \
  || note_fail "S4: --fix silently re-declared the subos as '$S4_RUNTIME'"

# ── S5: --fix converges ──────────────────────────────────────────────────
#
# The old invariant made this impossible for an evidence-free subos: the block
# failed validation, --fix rewrote it, and the next run did it again forever.
H5="$RUNTIME_DIR/s5"
make_home "$H5" '{"workspace":{}}'
doctor "$H5" --fix >/dev/null 2>&1 || true
S5_FIRST="$(cat "$H5/subos/default/.xlings.json")"
doctor "$H5" --fix >/dev/null 2>&1 || true
doctor "$H5" --fix >/dev/null 2>&1 || true
S5_LAST="$(cat "$H5/subos/default/.xlings.json")"
[[ "$S5_FIRST" == "$S5_LAST" ]] \
  || note_fail "S5: the manifest is still moving after three --fix runs"

# ── S6: describing does not fabricate a creation date ────────────────────
#
# Measured on a real home: `default` and `gfxbuild` carried a byte-identical
# `created_at` -- the moment they were DESCRIBED, not created. `default`
# predates it by months.
S6_CREATED="$(block_field "$H5/subos/default/.xlings.json" created_at)"
[[ "$S6_CREATED" == "<absent>" ]] \
  || note_fail "S6: a backfill claimed the subos was created at '$S6_CREATED'"
S6_DESCRIBED="$(block_field "$H5/subos/default/.xlings.json" described_at)"
[[ "$S6_DESCRIBED" != "<absent>" ]] \
  || note_fail "S6: nothing recorded WHEN the subos was described"

if [[ ${#FAILURES[@]} -gt 0 ]]; then
  for f in "${FAILURES[@]}"; do echo "[project-e2e] FAIL: $f" >&2; done
  exit 1
fi

log "PASS: describing a subos records what it is, or says it cannot tell"
