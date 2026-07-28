#!/usr/bin/env bash
# Measure `xlings self doctor [--fix]` against a slice and print the acceptance
# table from .agents/docs/2026-07-29-doctor-fix-one-shot-design.md §5.4.
#
# Prints discriminating counts, not a verdict, so a "pass" cannot be asserted
# without the numbers that justify it. A1..A15 are checked at the end.
set -uo pipefail

SLICE="${1:?usage: doctor-acceptance.sh <slice-dir> [outdir]}"
OUT="${2:-/tmp/doctor-acceptance}"
mkdir -p "$OUT"

XL="$SLICE/bin/xlings"
[[ -x "$XL" ]] || { echo "no binary at $XL" >&2; exit 2; }

run() {  # run <logfile> <args...>
    local log="$1"; shift
    env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$SLICE" \
        "$XL" "$@" >"$log" 2>&1
    echo $?
}
# The TUI pads panel lines and terminates them with CR. Both have to go, or a
# captured command carries a trailing \r that is passed as an empty argument
# and every remedy "fails" for a reason that is entirely the harness's.
strip() { sed -r 's/\x1B\[[0-9;]*[a-zA-Z]//g' "$1" | tr -d '\r' | sed -r 's/[[:space:]]+$//'; }

echo "=== 1. plain doctor (before fix)"
rc_before=$(run "$OUT/before.txt" self doctor)
echo "=== 2. doctor --fix"
rc_fix=$(run "$OUT/fix.txt" self doctor --fix)
echo "=== 3. plain doctor (after fix)"
rc_after=$(run "$OUT/after.txt" self doctor)
echo "=== 4. doctor --fix again (idempotence)"
rc_fix2=$(run "$OUT/fix2.txt" self doctor --fix)

# `grep -a` is load-bearing, not defensive. The rendered panel contains a NUL
# byte, and without -a GNU grep treats the stream as binary and prints NOTHING
# -- so every count reads as empty/0 and every assertion passes whether or not
# it should. Caught by running the A9 grep against the OLD report, which must
# be non-zero: it came back empty too.
count() { strip "$1" | grep -acF "$2" || true; }
field() { strip "$1" | sed -n -r "s/^ *$2 +([0-9]+).*/\1/p" | head -1; }

echo
echo "---------------- measurements ----------------"
printf 'A1  doctor --fix exit code              : %s   (want 0)\n' "$rc_fix"
printf 'A2  doctor exit code after fix          : %s   (want 0)\n' "$rc_after"
printf 'A3  second --fix healed/pruned          : %s / %s   (want 0 / 0)\n' \
    "$(field "$OUT/fix2.txt" 'healed')" "$(field "$OUT/fix2.txt" 'pruned')"
printf 'A4  "repair failed" lines  before/after : %s / %s   (want ?/0)\n' \
    "$(count "$OUT/fix.txt" '✗ repair failed')" \
    "$(count "$OUT/fix2.txt" '✗ repair failed')"
printf 'A5  "repair skipped" lines before/after : %s / %s   (want ?/0)\n' \
    "$(count "$OUT/fix.txt" '✗ repair skipped')" \
    "$(count "$OUT/fix2.txt" '✗ repair skipped')"
printf 'A6  broken payloads   before/after      : %s / %s   (want ?/0)\n' \
    "$(field "$OUT/before.txt" 'broken payloads')" \
    "$(field "$OUT/after.txt" 'broken payloads')"
printf 'A7  binding state     before/after      : %s / %s   (want ?/0)\n' \
    "$(field "$OUT/before.txt" 'binding state')" \
    "$(field "$OUT/after.txt" 'binding state')"
printf 'A8  other subos       before/after      : %s / %s   (want ?/0)\n' \
    "$(field "$OUT/before.txt" 'other subos findings')" \
    "$(field "$OUT/after.txt" 'other subos findings')"

# A9: a coordinate must never render as target@ns:version.
bad_coord_before=$(strip "$OUT/before.txt" \
    | grep -acE '[A-Za-z0-9_.+-]+@[a-z][a-z0-9-]*:' || true)
bad_coord_after=$(strip "$OUT/after.txt" \
    | grep -acE '[A-Za-z0-9_.+-]+@[a-z][a-z0-9-]*:' || true)
printf 'A9  "name@ns:ver" occurrences bef/aft   : %s / %s   (want ?/0)\n' \
    "$bad_coord_before" "$bad_coord_after"

printf 'A12 claude alias warnings  before/after : %s / %s   (want ?/0)\n' \
    "$(strip "$OUT/before.txt" | grep -ac 'alias unresolved.*claude' || true)" \
    "$(strip "$OUT/after.txt"  | grep -ac 'alias unresolved.*claude' || true)"
printf 'A13 report lines      before/after      : %s / %s\n' \
    "$(wc -l < "$OUT/before.txt")" "$(wc -l < "$OUT/after.txt")"
printf 'A11 recorded client version             : %s\n' \
    "$(python3 -c "import json;print(json.load(open('$SLICE/.xlings.json')).get('version'))")"
printf '    running version                     : %s\n' \
    "$(env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$SLICE" "$XL" --version 2>&1 | head -1)"

# A10: every printed remedy must actually work. This is the assertion that
# turns "the command is correct" from eyeballing into a measurement.
#
# Read off the BEFORE report, not the after: a successful --fix leaves nothing
# to print, so grading the after report would be grading an empty list and
# calling it a pass.
echo
echo "---------------- A10: printed remedies (from before.txt) ----------------"
mapfile -t cmds < <(strip "$OUT/before.txt" \
    | grep -a . | sed -n -r 's/^ *→ run +(xlings .*)$/\1/p' | sort -u)
if [[ ${#cmds[@]} -eq 0 ]]; then
    echo "  (no remedy commands printed)"
else
    a10_fail=0
    for c in "${cmds[@]}"; do
        # shellcheck disable=SC2086
        args=(${c#xlings })
        env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$SLICE" \
            "$XL" "${args[@]}" >/dev/null 2>&1
        rc=$?
        [[ $rc -eq 0 ]] || { a10_fail=$((a10_fail+1)); echo "  FAIL rc=$rc : $c"; }
    done
    echo "  ${#cmds[@]} command(s), $a10_fail failing   (want 0)"
fi
echo
echo "logs in $OUT/{before,fix,after,fix2}.txt"
