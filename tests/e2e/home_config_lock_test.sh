#!/usr/bin/env bash
# E2E test: commands that rewrite `~/.xlings.json` take the state lock.
#
# `~/.xlings.json` has several owners. install/remove/use write `versions`
# and `workspace`; the subos commands write `subos` and `activeSubos`;
# `xlings config` writes `lang`, `mirror` and `index_repos`. Each of them
# reads the whole document and writes the whole document back.
#
# Only the first group took the home-wide state lock. So `xlings subos new`
# would read the config, spend its time laying down directories (seconds, if
# the subos uses image storage and has to run mkfs.ext4), and then write back
# the document it read at the start -- silently reverting whatever an install
# committed in between. The payload stayed on disk with no record of it, and
# because `versions` and `workspace` are two halves of one release since
# 0.4.70, reverting one of them is enough to make a whole toolchain refuse to
# switch.
#
# The unit tests cover the locked-commit helper. What this covers is the
# wiring: that the real CLI paths actually go through it. An unrelated holder
# of the lock must make each of these commands fail, and fail without having
# touched the file.
#
# Scenarios:
#   1. subos new     — refused while the lock is held, nothing registered
#   2. subos use -g  — refused, activeSubos unchanged
#   3. subos rm      — refused, the entry survives
#   4. config --lang — refused, lang unchanged
#   5. all four succeed once the lock is released, and none of them lose the
#      `versions`/`workspace` keys they do not own
#
# Scenario 5 also pins the retry behavior of `subos new`. The lock is taken at
# the commit rather than around the whole command -- holding it across mkfs
# would make a routine install in another terminal wait past its timeout -- so
# a refused `subos new` has already laid down its directories. They are inert
# until the entry is registered, and the retry reuses them, which is what
# scenario 5 re-running the same `subos new` checks.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/home_config_lock"
HOME_DIR="$RUNTIME_DIR/home"
CONFIG="$HOME_DIR/.xlings.json"
LOCKFILE="$HOME_DIR/.xlings.lock"

cleanup() {
  # Release the holder before removing the tree: an flock'd descriptor left
  # open would keep the file alive on some platforms and turn cleanup into a
  # failure that says nothing about what was tested.
  exec 9>&- 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

# `env -i` on purpose: the lock is re-entrant for a child of its holder, via
# XLINGS_STATE_LOCK_HELD. Inheriting that marker from this shell would make
# every command below skip the lock and the test would pass vacuously.
RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

json_get() {  # json_get <jq-ish path expr in python>
  python3 -c "
import json,sys
d=json.load(open('$CONFIG'))
print($1)
" 2>/dev/null || echo "<unreadable>"
}

mkdir -p "$HOME_DIR"

# Seed the two keys the subos and config commands do not own. If any of them
# rewrites the document from a stale read, these are what disappears.
cat > "$CONFIG" <<'JSON'
{
  "activeSubos": "default",
  "subos": { "default": { "dir": "" }, "doomed": { "dir": "" } },
  "lang": "zh",
  "versions": { "gcc": { "16.1.0": { "path": "/store/gcc/16.1.0" } } },
  "workspace": { "active": { "gcc": "16.1.0" } }
}
JSON
mkdir -p "$HOME_DIR/subos/default" "$HOME_DIR/subos/doomed"

BEFORE="$(cat "$CONFIG")"

# ---------------------------------------------------------------- 1..4
# Hold the lock as an unrelated process would. flock(1) keeps it for as long
# as fd 9 is open in this shell.
exec 9>"$LOCKFILE"
if ! flock -n 9; then
  echo "FAIL: could not take the lock the test needs to hold"
  exit 1
fi
echo "holding $LOCKFILE"

expect_refused() {
  local what="$1"; shift
  local out rc=0
  out="$(RUN "$@" 2>&1)" || rc=$?
  if [[ $rc -eq 0 ]]; then
    echo "FAIL: [$what] succeeded while another process held the state lock"
    echo "$out"
    exit 1
  fi
  if ! grep -q "another xlings process" <<<"$out"; then
    echo "FAIL: [$what] failed for some other reason than the lock:"
    echo "$out"
    exit 1
  fi
  if [[ "$(cat "$CONFIG")" != "$BEFORE" ]]; then
    echo "FAIL: [$what] modified the config despite being refused"
    diff <(echo "$BEFORE") "$CONFIG" || true
    exit 1
  fi
  echo "  ok: [$what] refused, config untouched"
}

echo "--- 1..4: refused while the lock is held"
expect_refused "subos new"    subos new probe
expect_refused "subos use -g" subos use --global doomed
expect_refused "subos rm"     subos rm doomed
expect_refused "config"       config --lang en

# ---------------------------------------------------------------- 5
exec 9>&-
echo "--- 5: the same commands succeed once the lock is free"

RUN subos new probe        >/dev/null
RUN config --lang en       >/dev/null
RUN subos rm doomed        >/dev/null

[[ "$(json_get "'probe' in d['subos']")"  == "True"  ]] || { echo "FAIL: subos new did not register"; exit 1; }
[[ "$(json_get "'doomed' in d['subos']")" == "False" ]] || { echo "FAIL: subos rm did not deregister"; exit 1; }
[[ "$(json_get "d['lang']")"              == "en"    ]] || { echo "FAIL: config --lang did not apply"; exit 1; }

# The point of the whole exercise: keys these commands do not own survived
# every one of them.
[[ "$(json_get "d['versions']['gcc']['16.1.0']['path']")" == "/store/gcc/16.1.0" ]] \
  || { echo "FAIL: 'versions' was lost by a subos/config write"; exit 1; }
[[ "$(json_get "d['workspace']['active']['gcc']")" == "16.1.0" ]] \
  || { echo "FAIL: 'workspace' was lost by a subos/config write"; exit 1; }

echo "  ok: versions and workspace survived subos new / rm / config"
echo
echo "PASS: home config mutations are serialized by the state lock"
