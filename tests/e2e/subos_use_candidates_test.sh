#!/usr/bin/env bash
# E2E: `subos use` discovers and resolves one shared, bounded candidate view.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_use_candidates"
HOME_DIR="$RUNTIME_DIR/home"
LEGACY_HOME_DIR="$RUNTIME_DIR/legacy-home"
EMPTY_HOME_DIR="$RUNTIME_DIR/empty-home"
COLLISION_HOME_DIR="$RUNTIME_DIR/collision-home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"

RUN() {
  ( cd "$RUNTIME_DIR" && env -i HOME="$HOME_DIR" USER=xlings-ci \
      SHELL=/bin/sh PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      "$XLINGS_BIN" "$@" )
}

RUN_LEGACY() {
  ( cd "$RUNTIME_DIR" && env -i HOME="$LEGACY_HOME_DIR" USER=xlings-ci \
      SHELL=/bin/sh PATH=/usr/bin:/bin XLINGS_HOME="$LEGACY_HOME_DIR" \
      "$XLINGS_BIN" "$@" )
}

RUN_EMPTY() {
  ( cd "$RUNTIME_DIR" && env -i HOME="$EMPTY_HOME_DIR" USER=xlings-ci \
      SHELL=/bin/sh PATH=/usr/bin:/bin XLINGS_HOME="$EMPTY_HOME_DIR" \
      "$XLINGS_BIN" "$@" )
}

RUN_COLLISION() {
  ( cd "$RUNTIME_DIR" && env -i HOME="$COLLISION_HOME_DIR" USER=xlings-ci \
      SHELL=/bin/sh PATH=/usr/bin:/bin XLINGS_HOME="$COLLISION_HOME_DIR" \
      "$XLINGS_BIN" "$@" )
}

strip_ansi() { sed -E 's/\x1b\[[0-9;]*m//g'; }

mkdir -p "$HOME_DIR/subos"
for name in alpha alpine beta; do
  mkdir -p "$HOME_DIR/subos/$name/bin"
  printf '{"workspace":{}}\n' > "$HOME_DIR/subos/$name/.xlings.json"
done
cat > "$HOME_DIR/.xlings.json" <<'JSON'
{
  "activeSubos": "alpha",
  "subos": {
    "beta": {"dir": ""},
    "alpha": {"dir": ""},
    "alpine": {"dir": ""}
  }
}
JSON
ln -s alpha "$HOME_DIR/subos/current"
cp "$HOME_DIR/.xlings.json" "$RUNTIME_DIR/home-before.json"
CURRENT_BEFORE="$(readlink "$HOME_DIR/subos/current")"

log "no-name use is a read-only discovery operation"
set +e
NO_NAME_OUT="$(RUN subos use 2>&1 | strip_ansi)"
NO_NAME_RC=$?
set -e
[[ "$NO_NAME_RC" -eq 0 ]] \
  || fail "no-name subos use exited $NO_NAME_RC: $NO_NAME_OUT"
for name in alpha alpine beta; do
  grep -Eq "(^|[[:space:]])${name}([[:space:]]|$)" <<< "$NO_NAME_OUT" \
    || fail "no-name output omitted $name: $NO_NAME_OUT"
done
ALPHA_LINE="$(grep -nE '(^|[[:space:]])alpha([[:space:]]|$)' <<< "$NO_NAME_OUT" | head -1 | cut -d: -f1)"
ALPINE_LINE="$(grep -nE '(^|[[:space:]])alpine([[:space:]]|$)' <<< "$NO_NAME_OUT" | head -1 | cut -d: -f1)"
BETA_LINE="$(grep -nE '(^|[[:space:]])beta([[:space:]]|$)' <<< "$NO_NAME_OUT" | head -1 | cut -d: -f1)"
(( ALPHA_LINE < ALPINE_LINE && ALPINE_LINE < BETA_LINE )) \
  || fail "candidate output is not sorted: $NO_NAME_OUT"
grep -Fq "xlings subos use <name>" <<< "$NO_NAME_OUT" \
  || fail "no-name output omitted the usage hint: $NO_NAME_OUT"
if grep -Eq 'export (XLINGS_ACTIVE_SUBOS|PATH)=' <<< "$NO_NAME_OUT"; then
  fail "no-name discovery emitted shell activation code: $NO_NAME_OUT"
fi
cmp -s "$RUNTIME_DIR/home-before.json" "$HOME_DIR/.xlings.json" \
  || fail "no-name discovery rewrote the home manifest"
[[ "$(readlink "$HOME_DIR/subos/current")" == "$CURRENT_BEFORE" ]] \
  || fail "no-name discovery changed the current link"

log "exact, case-insensitive exact and unique prefix resolve once"
EXACT_OUT="$(RUN subos use beta --cmd 'printf EXACT_MATCHED' 2>&1 | strip_ansi)"
grep -Fq "EXACT_MATCHED" <<< "$EXACT_OUT" \
  || fail "case-sensitive exact match did not execute beta: $EXACT_OUT"
CASE_OUT="$(RUN subos use BETA --cmd 'printf CASE_MATCHED' 2>&1 | strip_ansi)"
grep -Fq "CASE_MATCHED" <<< "$CASE_OUT" \
  || fail "case-insensitive exact match did not execute beta: $CASE_OUT"
PREFIX_OUT="$(RUN subos use bet --cmd 'printf PREFIX_MATCHED' 2>&1 | strip_ansi)"
grep -Fq "PREFIX_MATCHED" <<< "$PREFIX_OUT" \
  || fail "unique prefix did not execute beta: $PREFIX_OUT"
grep -Fq "bet" <<< "$PREFIX_OUT" && grep -Fq "beta" <<< "$PREFIX_OUT" \
  || fail "unique prefix did not report its canonical resolution: $PREFIX_OUT"

log "ambiguous prefix and unknown names never execute"
set +e
AMBIGUOUS_OUT="$(RUN subos use alp --cmd 'printf SHOULD_NOT_RUN' 2>&1 | strip_ansi)"
AMBIGUOUS_RC=$?
set -e
[[ "$AMBIGUOUS_RC" -eq 2 ]] \
  || fail "ambiguous prefix exited $AMBIGUOUS_RC instead of 2: $AMBIGUOUS_OUT"
grep -Fq "alpha" <<< "$AMBIGUOUS_OUT" \
  || fail "ambiguous output omitted alpha: $AMBIGUOUS_OUT"
grep -Fq "alpine" <<< "$AMBIGUOUS_OUT" \
  || fail "ambiguous output omitted alpine: $AMBIGUOUS_OUT"
if grep -Fq "SHOULD_NOT_RUN" <<< "$AMBIGUOUS_OUT"; then
  fail "ambiguous prefix executed the command"
fi

set +e
UNKNOWN_OUT="$(RUN subos use alpx --cmd 'printf SHOULD_NOT_RUN' 2>&1 | strip_ansi)"
UNKNOWN_RC=$?
set -e
[[ "$UNKNOWN_RC" -eq 1 ]] \
  || fail "unknown name exited $UNKNOWN_RC instead of 1: $UNKNOWN_OUT"
grep -Fqi "not found" <<< "$UNKNOWN_OUT" \
  || fail "unknown name omitted NotFound: $UNKNOWN_OUT"
grep -Eq 'alpha|alpine' <<< "$UNKNOWN_OUT" \
  || fail "unknown name omitted nearest suggestions: $UNKNOWN_OUT"
if grep -Fq "SHOULD_NOT_RUN" <<< "$UNKNOWN_OUT"; then
  fail "unknown name executed the command"
fi

log "interface exposes the same fuzzy resolution event"
INTERFACE_OUT="$(RUN interface switch_subos --args '{"name":"bet"}' 2>&1)"
grep -Fq '"dataKind":"subos_candidates"' <<< "$INTERFACE_OUT" \
  || fail "interface omitted subos_candidates: $INTERFACE_OUT"
grep -Fq '"auto_selected":true' <<< "$INTERFACE_OUT" \
  || fail "interface did not identify fuzzy auto-selection: $INTERFACE_OUT"
grep -Fq '"selected":"beta"' <<< "$INTERFACE_OUT" \
  || fail "interface did not expose canonical beta: $INTERFACE_OUT"
grep -Fq '"exitCode":0' <<< "$INTERFACE_OUT" \
  || fail "interface fuzzy switch did not succeed: $INTERFACE_OUT"

log "case-insensitive exact collisions are ambiguous"
for name in Beta beta; do
  mkdir -p "$COLLISION_HOME_DIR/subos/$name/bin"
  printf '{"workspace":{}}\n' > "$COLLISION_HOME_DIR/subos/$name/.xlings.json"
done
cat > "$COLLISION_HOME_DIR/.xlings.json" <<'JSON'
{"activeSubos":"Beta","subos":{"Beta":{"dir":""},"beta":{"dir":""}}}
JSON
set +e
COLLISION_OUT="$(RUN_COLLISION subos use BETA --cmd 'printf SHOULD_NOT_RUN' 2>&1 | strip_ansi)"
COLLISION_RC=$?
set -e
[[ "$COLLISION_RC" -eq 2 ]] \
  || fail "case-insensitive collision exited $COLLISION_RC instead of 2: $COLLISION_OUT"
grep -Fq "Beta" <<< "$COLLISION_OUT" && grep -Fq "beta" <<< "$COLLISION_OUT" \
  || fail "case-insensitive collision omitted candidates: $COLLISION_OUT"
if grep -Fq "SHOULD_NOT_RUN" <<< "$COLLISION_OUT"; then
  fail "case-insensitive collision executed the command"
fi

log "legacy default is synthesized from its manifest without a write"
mkdir -p "$LEGACY_HOME_DIR/subos/default/bin"
printf '{"workspace":{}}\n' > "$LEGACY_HOME_DIR/subos/default/.xlings.json"
cat > "$LEGACY_HOME_DIR/.xlings.json" <<'JSON'
{"activeSubos":"default","subos":{}}
JSON
cp "$LEGACY_HOME_DIR/.xlings.json" "$RUNTIME_DIR/legacy-before.json"
LEGACY_LIST="$(RUN_LEGACY subos use 2>&1 | strip_ansi)"
[[ "$(grep -Ec '(^|[[:space:]])default([[:space:]]|$)' <<< "$LEGACY_LIST")" -eq 1 ]] \
  || fail "legacy default was not synthesized exactly once: $LEGACY_LIST"
LEGACY_USE="$(RUN_LEGACY subos use def --cmd 'printf LEGACY_MATCHED' 2>&1 | strip_ansi)"
grep -Fq "LEGACY_MATCHED" <<< "$LEGACY_USE" \
  || fail "synthesized legacy default was not usable: $LEGACY_USE"
cmp -s "$RUNTIME_DIR/legacy-before.json" "$LEGACY_HOME_DIR/.xlings.json" \
  || fail "legacy candidate discovery wrote the registry"

log "an empty candidate view points to subos new"
mkdir -p "$EMPTY_HOME_DIR/subos"
printf '{"activeSubos":"default","subos":{}}\n' > "$EMPTY_HOME_DIR/.xlings.json"
EMPTY_OUT="$(RUN_EMPTY subos use 2>&1 | strip_ansi)"
grep -Fq "xlings subos new <name>" <<< "$EMPTY_OUT" \
  || fail "empty discovery omitted the creation hint: $EMPTY_OUT"

log "PASS: subos use discovery and safe fuzzy resolution"
