#!/usr/bin/env bash
# E2E: the diagnostic contract — one problem, one marker, and nothing silent.
#
# Each scenario below is a defect that shipped, written as the observation that
# would have caught it:
#
#   S1  three [error] markers for one problem ("not installed in this subos")
#   S2  a pinned version failing without naming the file that pinned it
#   S3  candidate lists in std::map order, uncapped (877 chars / 94 versions)
#   (S4 -- the confirmation refusal -- is covered by unit tests; see below)
#   S5  `xlings config --ui-mode/--theme/--interactive` not reaching anything
#   S6  a theme file with a typo in it behaving exactly like no theme at all
#   S7  a degraded frontend not saying it degraded
#
# Every assertion is on OBSERVABLE output or exit code, never on internals:
# these have to keep working when the implementation behind them moves.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/diagnostics_contract"
HOME_DIR="$(runtime_home_dir diagnostics_contract)"
PROJ_DIR="$RUNTIME_DIR/proj"

cleanup() { rm -rf "$RUNTIME_DIR" "$HOME_DIR"; }
trap cleanup EXIT
cleanup

assert_home_is_isolated "$HOME_DIR"
mkdir -p "$HOME_DIR" "$PROJ_DIR"

XLINGS_BIN="$(find_xlings_bin)"

# Always from a neutral cwd unless a scenario needs project context, and never
# with the developer's XLINGS_PROJECT_DIR leaking in.
RUN() {
  ( cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" \
      XLINGS_TERM_WIDTH=100 "$XLINGS_BIN" "$@" )
}
RUN_IN_PROJ() {
  ( cd "$PROJ_DIR" && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" \
      XLINGS_TERM_WIDTH=100 "$XLINGS_BIN" "$@" )
}

count_markers() { grep -c '^\[error\]' <<<"$1" || true; }

RUN self init >/dev/null 2>&1 || fail "self init failed"

# A target with three versions installed in this subos, so `use` has something
# to be ambiguous about and the candidate list has something to sort.
python3 - "$HOME_DIR" <<'PY'
import json, sys, pathlib
h = pathlib.Path(sys.argv[1])
cfg = json.loads((h / '.xlings.json').read_text())
# Deliberately NOT in sorted order, and deliberately including a version whose
# lexicographic position differs from its real one: 0.0.100 sorts before
# 0.0.24 as a string, which is the bug this fixture exists to expose.
cfg.setdefault('versions', {})['demo'] = {'versions': {
    v: {'path': str(h / 'data/xpkgs/demo' / v), 'type': 'program'}
    for v in ('0.0.24', '0.0.100', '0.0.9')
}}
(h / '.xlings.json').write_text(json.dumps(cfg, indent=2))
sub = h / 'subos/default/.xlings.json'
s = json.loads(sub.read_text())
s.setdefault('workspace', {})['demo'] = {
    'active': '', 'installed': ['0.0.24', '0.0.100', '0.0.9']}
sub.write_text(json.dumps(s, indent=2))
PY

log "S1: one problem, one severity marker"
# `absent` exists in no subos at all -> the not-in-subos diagnostic.
out="$(RUN use absent 2>&1 || true)"
markers="$(count_markers "$out")"
[[ "$markers" == "1" ]] \
  || fail "S1: expected exactly 1 [error] marker, got $markers:
$out"
grep -q "is not a name this home knows" <<<"$out" \
  || fail "S1: unexpected summary: $out"
# The detail lines must be continuations, not their own errors.
grep -qE '^\s+install it +xlings install absent' <<<"$out" \
  || fail "S1: action row missing or re-tagged: $out"

log "S2: a pin names the file that set it"
cat > "$PROJ_DIR/.xlings.json" <<'JSON'
{ "workspace": { "demo": "9.9.9" } }
JSON
out="$(RUN_IN_PROJ use demo 2>&1 || true)"
grep -q '\.xlings\.json' <<<"$out" \
  || fail "S2: the diagnostic never names the file that pinned it:
$out"
grep -q 'workspace\.demo' <<<"$out" \
  || fail "S2: the diagnostic names the file but not the field:
$out"
rm -f "$PROJ_DIR/.xlings.json"

log "S2b: a project pin asks you to set the project up, not to retype it"
cat > "$PROJ_DIR/.xlings.json" <<'JSON'
{ "workspace": { "demo": "9.9.9" } }
JSON
out="$(RUN_IN_PROJ use demo 2>&1 || true)"
# WARN, not error: the command failed (exit stays non-zero) but the project is
# one documented command away from working, and xlings knows which one. A red
# [error] for "your project needs installing" reads as a fault in the tool.
grep -q '^\[warn\]' <<<"$out" \
  || fail "S2b: a project pin was reported as an error, not a warning:
$out"
grep -q "this project asks for a version of" <<<"$out" \
  || fail "S2b: the wording does not say it came from the project:
$out"
# The action must be the one the user should actually run. `xlings install`
# with no args reads the manifest; naming the coordinate makes the user redo
# what the file already says, and gets it wrong with more than one dependency.
grep -qE '^\s+set this project up\s+xlings install\s*$' <<<"$out" \
  || fail "S2b: the action is not the no-argument project install:
$out"
rm -f "$PROJ_DIR/.xlings.json"

log "S2c: standing in a project does not make a GLOBAL pin the project's fault"
# The gate for S2b's friendlier wording is which LAYER pinned the version, not
# whether a project config happens to exist. Those are different questions, and
# answering the second one instead sends the user to `xlings install` -- which
# reads only the project manifest, so it would install the OTHER package below,
# exit 0, and leave `demo` exactly as broken as it was. Succeeded-having-done-
# nothing, produced by the fix for succeeded-having-done-nothing.
python3 - "$HOME_DIR" <<'PY'
import json, sys, pathlib
sub = pathlib.Path(sys.argv[1]) / 'subos/default/.xlings.json'
s = json.loads(sub.read_text())
s['workspace']['demo']['active'] = '9.9.9'   # pinned HERE, in the subos file
sub.write_text(json.dumps(s, indent=2))
PY
cat > "$PROJ_DIR/.xlings.json" <<'JSON'
{ "workspace": { "somethingelse": "1.0.0" } }
JSON
out="$(RUN_IN_PROJ use demo 2>&1 || true)"
grep -q '^\[error\]' <<<"$out" \
  || fail "S2c: a global pin was softened to a warning because a project file
happened to be in the directory:
$out"
# Spelled `if grep; then fail` and NOT `grep -qv ... || fail`: `grep -qv` is
# true when ANY line fails to match, so that form passes no matter what the
# output says. This file has shipped that mistake before.
if grep -q "this project asks for" <<<"$out"; then
  fail "S2c: a global pin was attributed to the project:
$out"
fi
if grep -q "set this project up" <<<"$out"; then
  fail "S2c: offered \`xlings install\`, which does not read the file that
pinned this and would exit 0 having installed something else:
$out"
fi
rm -f "$PROJ_DIR/.xlings.json"
python3 - "$HOME_DIR" <<'PY'
import json, sys, pathlib
sub = pathlib.Path(sys.argv[1]) / 'subos/default/.xlings.json'
s = json.loads(sub.read_text())
s['workspace']['demo']['active'] = ''
sub.write_text(json.dumps(s, indent=2))
PY

log "S3: candidate lists are newest-first and capped"
out="$(RUN use demo 2>&1 || true)"
# Newest first, whether the candidates come out as one list or as panel rows.
# Flattened to the order the versions APPEAR, because that is what the reader
# sees; asserting on a particular layout would break the next time the panel
# changes.
order="$(grep -oE '0\.0\.(100|24|9)' <<<"$out" | awk '!seen[$0]++' | paste -sd, -)"
[[ -n "$order" ]] || fail "S3: no candidates in output:
$out"
# Lexicographic order is 0.0.100, 0.0.24, 0.0.9 -- which happens to LOOK right
# for the first pair, so the assertion has to pin the whole sequence.
[[ "$order" == "0.0.100,0.0.24,0.0.9" ]] \
  || fail "S3: candidates not newest-first, got [$order]:
$out"
# No line may run away. The measured regression was 877 characters.
long="$(awk 'length > 200 {print length; exit}' <<<"$out" || true)"
[[ -z "$long" ]] || fail "S3: a line of $long characters escaped the width contract:
$out"

# S4 (confirmation refusal) is NOT here, deliberately.
#
# Reaching a confirmation end-to-end needs a package that installs from the
# fixture index, and `remove` only gets that far after the catalog loads. The
# fixture index is a git checkout whose local origin gets cloned into the test
# home, so an injected untracked recipe does not survive the first resync --
# the scenario would run, pass, and assert nothing.
#
# The behaviour itself is covered where it can be observed exactly:
# tests/unit/test_prompt_refusal.cpp, five cases including BOTH default
# directions (install defaults "y", remove defaults "n" -- the asymmetry that
# made the old auto-answer dangerous), that no question is emitted when nobody
# can answer, and that a registered auto-responder still outranks it.
#
# Wiring a durable installable fixture is worth doing; pretending this file
# covers it would be worse than saying so.

log "S5: the new settings persist and are read back"
RUN config --ui-mode cli >/dev/null 2>&1 || fail "S5: --ui-mode rejected"
RUN config --interactive false >/dev/null 2>&1 || fail "S5: --interactive rejected"
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
d = json.loads((pathlib.Path(sys.argv[1]) / '.xlings.json').read_text())
assert d.get('uiMode') == 'cli', f"uiMode not stored: {d.get('uiMode')!r}"
assert d.get('tui', {}).get('interactive') is False, \
    f"tui.interactive not stored: {d.get('tui')!r}"
PY
RUN config 2>&1 | grep -q "ui mode" \
  || fail "S5: `xlings config` does not report the resolved frontend"

log "S5b: an invalid mode is refused, not silently ignored"
set +e
RUN config --ui-mode fancy >"$RUNTIME_DIR/s5b.out" 2>&1
rc=$?
set -e
[[ "$rc" -ne 0 ]] || fail "S5b: an unknown ui mode was accepted"
grep -q "is not a UI mode" "$RUNTIME_DIR/s5b.out" \
  || fail "S5b: no explanation: $(cat "$RUNTIME_DIR/s5b.out")"

log "S6: shipped themes exist, and a broken one is reported"
[[ -f "$HOME_DIR/config/themes/mono.json" ]] \
  || fail "S6: mono.json was not provisioned"
[[ -f "$HOME_DIR/config/themes/high-contrast.json" ]] \
  || fail "S6: high-contrast.json was not provisioned"
RUN config --theme list 2>&1 | grep -q "built in" \
  || fail "S6: --theme list does not mention the built-in default"

cat > "$HOME_DIR/config/themes/typo.json" <<'JSON'
{ "name": "typo", "dark": { "acent": "#010203" } }
JSON
RUN config --theme typo >/dev/null 2>&1 || fail "S6: --theme typo rejected"
out="$(RUN config 2>&1 || true)"
grep -q "is not a colour slot" <<<"$out" \
  || fail "S6: a mistyped slot was ignored silently:
$out"
grep -q "accent" <<<"$out" \
  || fail "S6: no suggestion for the mistyped slot:
$out"

# A theme that does not exist must also say so rather than quietly defaulting.
RUN config --theme ./nowhere.json >/dev/null 2>&1 || true
out="$(RUN config 2>&1 || true)"
grep -qi "theme file is not there" <<<"$out" \
  || fail "S6: a missing theme file was ignored silently:
$out"
RUN config --theme default >/dev/null 2>&1 || true

log "S7: a degraded frontend says so"
out="$(RUN --ui-mode tui list 2>&1 || true)"
# stdout is a pipe here, so tui cannot be honoured. Silently doing something
# else is how a user concludes their config does nothing.
grep -q "frontend instead of tui" <<<"$out" \
  || fail "S7: degraded to cli without saying so:
$out"
grep -q "not a terminal" <<<"$out" \
  || fail "S7: degraded without saying why:
$out"

log "PASS: diagnostics contract"
