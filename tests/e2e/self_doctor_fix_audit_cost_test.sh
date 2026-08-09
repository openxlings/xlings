#!/usr/bin/env bash
# E2E: `--fix` does not pay for an audit it cannot act on, and `--fix --deep`
# pays for it only when a payload could have changed.
#
# Measured before this test existed: on a real 71 GB home one payload audit is
# ~196s, `--fix` ran SEVEN of them (one detection plus six `refresh()` calls),
# and no repair selects on what they produce -- LoaderLibcSplit and
# NssResolution reach `count_` and `render_` and nothing else. ~23 minutes,
# entirely unused.
#
# The assertion is a COUNT, not a duration. Timings vary by machine and disk;
# how many times the ELF walk ran does not. Each audited ELF costs exactly two
# patchelf calls (interpreter, then rpath), so with one ELF in the store the
# recorder's line count is `2 x audits`.
#
#   xlings self doctor              0 audits   (quick, unchanged)
#   xlings self doctor --deep       1 audit
#   xlings self doctor --fix        0 audits   <- the fix
#   xlings self doctor --fix --deep <= 2       <- one initial + one after the
#                                                 payload ladder, which is the
#                                                 only repair that can change
#                                                 what an audit would see
#
# And a report that did not audit has to SAY it did not audit: a clean `--fix`
# and a clean `--fix --deep` printing the same thing is "did not check" and
# "checked, nothing wrong" sharing one output.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
require_fixture_index

RUNTIME_DIR="$(runtime_home_dir self_doctor_fix_audit_cost)"
HOME_DIR="$RUNTIME_DIR/home"
RECORDER_DIR="$RUNTIME_DIR/recorders"
TRACE="$RUNTIME_DIR/patchelf.trace"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$HOME_DIR/subos/default/bin" "$RECORDER_DIR"

XLINGS_BIN="$(find_xlings_bin)"
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"

# Exactly one ELF in the store, so the count arithmetic stays readable.
PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-fixture/1.0.0"
mkdir -p "$PAYLOAD/bin"
printf '\177ELF fixture\n' > "$PAYLOAD/bin/tool"
chmod +x "$PAYLOAD/bin/tool"
: > "$PAYLOAD/.xpkg-install.json"

cat > "$HOME_DIR/.xlings.json" <<'JSON'
{ "mirror": "GLOBAL", "activeSubos": "default", "subos": {"default": {"dir": ""}} }
JSON
printf '{"workspace":{}}\n' > "$HOME_DIR/subos/default/.xlings.json"
ln -sfn "$HOME_DIR/subos/default" "$HOME_DIR/subos/current"

cat > "$RECORDER_DIR/patchelf" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >> "$DOCTOR_PATCHELF_TRACE"
case "$1" in
  --print-interpreter) printf '/lib64/ld-linux-x86-64.so.2\n' ;;
  --print-rpath)       printf '\n' ;;
esac
SH
chmod +x "$RECORDER_DIR/patchelf"

RUN() {
  ( cd /tmp && exec env -i HOME="$HOME" \
      PATH="$RECORDER_DIR:/usr/bin:/bin" \
      XLINGS_HOME="$HOME_DIR" \
      DOCTOR_PATCHELF_TRACE="$TRACE" \
      "$XLINGS_BIN" "$@" ) || true
}

audits() {
  local lines=0
  [[ -f "$TRACE" ]] && lines="$(wc -l < "$TRACE" | tr -d ' ')"
  # two patchelf calls per audited ELF, one ELF in the store
  echo $(( lines / 2 ))
}

run_and_count() {   # $1 = label, rest = argv
  local label="$1"; shift
  : > "$TRACE"
  RUN "$@" > "$RUNTIME_DIR/${label}.out" 2>&1
  audits
}

log "quick doctor audits nothing"
n="$(run_and_count quick self doctor)"
[[ "$n" -eq 0 ]] || fail "quick doctor ran $n payload audit(s)"

log "--deep audits once"
n="$(run_and_count deep self doctor --deep)"
[[ "$n" -eq 1 ]] || fail "--deep ran $n payload audit(s), expected 1"

log "--fix does not pay for an audit no repair consumes"
n="$(run_and_count fix self doctor --fix)"
[[ "$n" -eq 0 ]] \
  || fail "--fix ran $n payload audit(s); it consumes none of their findings"

log "--fix --deep audits at most twice"
n="$(run_and_count fixdeep self doctor --fix --deep)"
[[ "$n" -ge 1 && "$n" -le 2 ]] \
  || fail "--fix --deep ran $n payload audit(s), expected 1..2"

log "a report that skipped the audit says so"
grep -Fq "payload/runtime audit did not run" "$RUNTIME_DIR/fix.out" \
  || fail "--fix did not disclose the skipped audit: $(cat "$RUNTIME_DIR/fix.out")"
grep -Fq "payload/runtime audit did not run" "$RUNTIME_DIR/quick.out" \
  || fail "quick doctor did not disclose the skipped audit"
if grep -Fq "payload/runtime audit did not run" "$RUNTIME_DIR/deep.out"; then
  fail "--deep claims it skipped an audit it ran"
fi
if grep -Fq "payload/runtime audit did not run" "$RUNTIME_DIR/fixdeep.out"; then
  fail "--fix --deep claims it skipped an audit it ran"
fi

log "--scope still requires --deep, and no longer claims --fix implies it"
set +e
SCOPE_OUT="$(RUN self doctor --fix --scope fixture 2>&1)"
set -e
grep -Fq -- "--scope\` requires \`--deep\`" <<< "$SCOPE_OUT" \
  || fail "--fix --scope was accepted or misreported: $SCOPE_OUT"

# The counter-assertion, and the reason it is here rather than implied.
#
# "--fix walks no payloads" is satisfiable by making --fix do nothing. What it
# must KEEP is the catalog half: a BrokenPayload finding's remedy is an install
# command produced by resolving the owning coordinate, and the repair ladder
# uses the same resolve to decide the package is reinstallable. Cheap to keep,
# and the whole point of splitting one flag into two rather than deleting one.
# The counter-assertion, and why it is a differential.
#
# "--fix walks no payloads" is satisfiable by making --fix do nothing, so the
# split needs a guard that it took away only the payload half. Stated as: with
# and without --deep, everything --fix reports about broken payloads and their
# remedies must be identical. Only the payload audit differs between the two
# runs, so any divergence here is the split reaching somewhere it should not.
#
# The POSITIVE path -- a resolvable coordinate actually producing
# `xlings install ...` and driving the repair ladder -- is asserted by
# doctor_fix_convergence_test.sh and foreign_payload_reinstall_test.sh, which
# carry working index fixtures. Rebuilding one here would test their setup
# rather than this change.
log "--deep changes only the payload audit, not what --fix repairs"
: > "$TRACE"
RUN self doctor --fix --dry-run > "$RUNTIME_DIR/rep-fix.out" 2>&1
: > "$TRACE"
RUN self doctor --fix --deep --dry-run > "$RUNTIME_DIR/rep-deep.out" 2>&1

repair_view() {   # everything about repairs and payload registrations
  grep -E "broken payload|xlings install|would run|no remedy|prune" "$1" || true
}
diff <(repair_view "$RUNTIME_DIR/rep-fix.out") \
     <(repair_view "$RUNTIME_DIR/rep-deep.out") \
  || fail "--deep changed what --fix reports about repairs"

# ...and the two runs must genuinely differ somewhere, or the diff above is
# comparing a thing to itself and proves nothing.
if diff -q "$RUNTIME_DIR/rep-fix.out" "$RUNTIME_DIR/rep-deep.out" >/dev/null; then
  fail "--fix and --fix --deep produced byte-identical output; the differential is vacuous"
fi

log "PASS: --fix no longer pays for the audit it cannot use"
