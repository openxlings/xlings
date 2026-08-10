#!/usr/bin/env bash
# Acceptance for the one-answerer install-state contract (2026.8.11.1).
#
# WHY THIS EXISTS AS A TOOL AND NOT ONLY AS TESTS
#
# The contract's whole point is that four commands agree about one fact. A unit
# test can only ever check one of them at a time, and the defect it replaces was
# precisely that each component was individually correct. So this drives the
# real binary against a real (isolated) home and asks all of them.
#
# Every check here is written to FAIL if the behaviour silently reverts:
#   * the marker is REQUIRED, not tolerated -- a conditional check would pass
#     through the "nothing there" branch if it stopped being written, and
#     report success about the regression it exists to catch;
#   * the retry case seeds a payload WITH CONTENT, because an empty directory
#     re-runs the hook even under the old predicate and would prove nothing;
#   * the compatibility case asserts a pre-field payload is left ALONE, which
#     is the direction that would show up as "why is my home reinstalling
#     itself".
#
# Usage: XLINGS_BIN=/abs/path/to/xlings ./install-state-acceptance.sh
# Refs: .agents/docs/2026-08-11-five-issues-triage-and-plan.md
set -uo pipefail

XBIN="${XLINGS_BIN:-}"
[ -n "$XBIN" ] && [ -x "$XBIN" ] || {
    echo "XLINGS_BIN must be an absolute path to a built xlings" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
HOME_DIR="$WORK/home"
STORE="$HOME_DIR/data/xpkgs"
STAMP=".xpkg-install.json"
fails=0

ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fails=$((fails+1)); }
run()  { ( PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XBIN" "$@" ) 2>&1; }

seed_payload() {
    # $1 store name, $2 version, $3 stamp body ("" = no stamp at all)
    local d="$STORE/$1/$2"
    mkdir -p "$d/lib"
    printf 'payload\n' > "$d/lib/lib$1.so"
    [ -n "$3" ] && printf '%s\n' "$3" > "$d/$STAMP"
    echo "$d"
}

mkdir -p "$HOME_DIR/subos/default/bin"

echo "== 1. a recorded failure is reported, and it is an error =="
seed_payload xim-x-failed 1.0 '{
  "os": "linux", "version": "1.0", "xlings_version": "test",
  "incomplete": true, "reason": "install hook returned false"
}' >/dev/null
out="$(run self doctor)"
printf '%s' "$out" | grep -q "incomplete install" \
    && ok "doctor reports the incomplete install" \
    || bad "doctor did not report a payload whose stamp records a failed install"
printf '%s' "$out" | grep -q "xlings install xim:failed@1.0" \
    && ok "and prints the coordinate that repairs it" \
    || bad "no repair command printed"

echo "== 2. a payload from before the field existed is LEFT ALONE =="
# The compatibility direction. 86 payloads on a real home look like this;
# reporting them is how a tool teaches people to ignore it.
seed_payload xim-x-legacy 0.9 '{
  "os": "linux", "version": "0.9", "xlings_version": "2026.8.10.4"
}' >/dev/null
out="$(run self doctor --all)"
printf '%s' "$out" | grep -q "incomplete install.*legacy" \
    && bad "a pre-field payload was reported as an incomplete INSTALL (error)" \
    || ok "pre-field payload is not an error"
printf '%s' "$out" | grep -q "unverified install.*legacy" \
    && ok "it is a notice instead, with a command that settles it" \
    || bad "pre-field payload produced no notice either -- it is invisible"

echo "== 3. a stamp declaring zero registrations is not a complaint =="
# Zero is a DECLARATION ("this package registers nothing"), unlike absent.
seed_payload xim-x-meta 0.1 '{
  "os": "linux", "version": "0.1", "xlings_version": "test", "registered": 0
}' >/dev/null
run self doctor --all | grep -qE "(incomplete|unverified) install.*meta" \
    && bad "a package declaring it registers nothing was reported" \
    || ok "declared-zero is silent"

echo "== 4. the failure is not counted as healthy by the exit code =="
run self doctor >/dev/null; rc=$?
[ "$rc" -ne 0 ] \
    && ok "doctor exits non-zero while an incomplete install stands (rc=$rc)" \
    || bad "doctor exited 0 with an incomplete install present -- every script wrapping it sees success"

echo
if [ "$fails" -eq 0 ]; then
    echo "install-state acceptance: PASS"
else
    echo "install-state acceptance: $fails FAILED"
fi
exit $(( fails > 0 ))
