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
        # Through a shell, not execv. A remedy is printed for a HUMAN to paste,
        # so some are compound (`xlings subos use X && xlings self doctor
        # --fix`) -- and word-splitting one of those handed `&&` to the binary
        # as an argument, which exits 2 every time. Two perfectly good remedies
        # were reported as failing by the harness, not by the product: a check
        # that cannot run what it grades produces noise that looks exactly like
        # the defect it exists to find.
        env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$SLICE" \
            XLINGS_BIN="$XL" sh -c "${c//xlings /\"\$XLINGS_BIN\" }" \
            >/dev/null 2>&1
        rc=$?
        [[ $rc -eq 0 ]] || { a10_fail=$((a10_fail+1)); echo "  FAIL rc=$rc : $c"; }
    done
    echo "  ${#cmds[@]} command(s), $a10_fail failing   (want 0)"
fi
echo
# A16: does the report agree with the disk about the library farm, and does a
# prune announce itself?
#
# Three numbers, no verdict. The first pair is the one that mattered: until
# 2026.9.4.1 the scan skipped <subos>/lib entirely, so "reported" was 0 on a
# home that had real dangling links and nothing said otherwise.
echo "---------------- A16: sysroot links + prune honesty ----------------"
actual_dangling() {
    local n=0 d
    for d in "$SLICE"/subos/*/lib "$SLICE"/subos/*/lib64; do
        [[ -d "$d" ]] || continue
        while IFS= read -r l; do
            [[ -e "$l" ]] || n=$((n+1))
        done < <(find "$d" -maxdepth 1 -type l 2>/dev/null)
    done
    printf '%s' "$n"
}
printf '    dangling links reported (before)    : %s\n' \
    "$(strip "$OUT/before.txt" | grep -c 'dangling sysroot link' || true)"
printf '    dangling links on disk (after fix)  : %s   (want 0)\n' \
    "$(actual_dangling)"
printf '    links re-pointed by --fix           : %s\n' \
    "$(strip "$OUT/fix.txt" | grep -c 'link repointed' || true)"
printf '    links deleted by --fix              : %s\n' \
    "$(strip "$OUT/fix.txt" | grep -c 'dangling link removed' || true)"
pruned_n="$(strip "$OUT/fix.txt" | sed -n -r 's/^ *. pruned +([0-9]+).*/\1/p' | head -1)"
printf '    registrations pruned                : %s\n' "${pruned_n:-0}"
# A pruned run that still calls itself OK is the #583 verdict bug.
if strip "$OUT/fix.txt" | grep -q 'pruned' \
   && strip "$OUT/fix.txt" | grep -q 'OK — workspace, shims, and payloads'; then
    printf '    verdict after a prune               : FAIL (said OK)\n'
else
    printf '    verdict after a prune               : ok\n'
fi
echo
echo "logs in $OUT/{before,fix,after,fix2}.txt"
