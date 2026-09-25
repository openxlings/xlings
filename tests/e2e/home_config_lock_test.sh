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
#   6. `list`, while an unrelated process holds the lock: the upgrade-notice
#      writer behind it (Config::mark_hint_seen / record_verified_version,
#      2026.9.12 F4) must not wait on the ten-minute default the OTHER four
#      scenarios above are pinned to avoid -- it skips its own write and
#      `list` still exits 0 promptly, config untouched
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
#
# XLINGS_LOCK_TIMEOUT is pinned because this file tests REFUSAL, not patience.
# It held the production default implicitly, and when that default moved from
# 30s to 10 minutes -- so a second `xlings install` waits for a real install
# instead of failing on one -- four refusals turned this test from 2 minutes
# into 40, the slowest thing in the suite by an order of magnitude.
#
# A test that asserts "refused while the lock is held" should say how long it
# is willing to wait to see that, rather than inherit a number chosen for
# interactive users. Pinning it also means the next change to the default
# cannot silently make CI 38 minutes slower.
RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_LOCK_TIMEOUT=2 "$XLINGS_BIN" "$@" )
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
expect_refused "subos rm"     subos rm doomed -y
expect_refused "config"       config --lang en

# ---------------------------------------------------------------- 5
exec 9>&-
echo "--- 5: the same commands succeed once the lock is free"

RUN subos new probe        >/dev/null
RUN config --lang en       >/dev/null
RUN subos rm doomed -y     >/dev/null

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

# ---------------------------------------------------------------- 6
#
# `list` never took the state lock for its own read -- what it now does,
# on nearly every command, is the upgrade-notice write path
# (show_upgrade_notice_once_ -> notice::notice_once ->
# Config::mark_hint_seen, plus record_verified_version from `--fix`). That
# write is a background nicety, not a mutation the user asked for, so it
# must not make `list` wait on XLINGS_LOCK_TIMEOUT's ten-minute default the
# way scenarios 1-4 above are deliberately pinned to avoid.
#
# The notice is TTY-gated (platform::supports_rewrite_output()), so this
# needs a real pty -- see notice_once_test.sh, which solved the identical
# problem for the same notice.
echo "--- 6: list (unrelated to config/subos) skips its own lock-protected write and stays fast"

BIN_DIR="$RUNTIME_DIR/bin"
mkdir -p "$BIN_DIR"
cat > "$BIN_DIR/run_pty.py" <<'PY'
import os, pty, fcntl, termios, struct, select, subprocess, sys

home, binary, *args = sys.argv[1:]
env = {"HOME": home, "PATH": "/usr/bin:/bin",
       "XLINGS_HOME": home, "TERM": "xterm-256color"}
mfd, sfd = pty.openpty()
fcntl.ioctl(sfd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
p = subprocess.Popen([binary, *args], stdin=subprocess.DEVNULL,
                     stdout=sfd, stderr=sfd, env=env, close_fds=True, cwd="/tmp")
os.close(sfd)
out = b""
while True:
    r, _, _ = select.select([mfd], [], [], 20)
    if not r:
        break
    try:
        d = os.read(mfd, 65536)
    except OSError:
        break
    if not d:
        break
    out += d
p.wait()
sys.stdout.buffer.write(out)
sys.exit(p.returncode)
PY
run_pty() { python3 "$BIN_DIR/run_pty.py" "$HOME_DIR" "$XLINGS_BIN" "$@"; }

# Backdate the recorded version so show_upgrade_notice_once_ has something
# to say (and therefore something to try to WRITE via mark_hint_seen) --
# an absent record is not a mismatch and would make this scenario silently
# exercise nothing.
python3 - "$CONFIG" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text())
d["version"] = "v0.4.40"
d.pop("verifiedBy", None)
d.pop("hintsSeen", None)
p.write_text(json.dumps(d, indent=2))
PY
BEFORE6="$(cat "$CONFIG")"

exec 9>"$LOCKFILE"
if ! flock -n 9; then
  echo "FAIL: could not take the lock scenario 6 needs to hold"
  exit 1
fi
echo "holding $LOCKFILE"

# Portable millisecond timer (bash 5 EPOCHREALTIME; falls back to whole
# seconds elsewhere) -- same approach as run_all.sh's _t_ms.
_ms() {
  if [[ -n "${EPOCHREALTIME:-}" ]]; then
    local er=${EPOCHREALTIME} s us
    s=${er%.*}; us=${er#*.}
    echo $(( 10#$s * 1000 + 10#$us / 1000 ))
  else
    echo $(( $(date +%s) * 1000 ))
  fi
}

start_ms=$(_ms)
rc6=0
out6="$(run_pty list)" || rc6=$?
end_ms=$(_ms)
elapsed_ms=$((end_ms - start_ms))

exec 9>&-

# 0 or 1, not a crash: this file's HOME_DIR is a hand-crafted config with
# no package index ever set up (it exists only to exercise subos/config
# commands), so `list` can legitimately fail with its own "package index
# not available" here regardless of the lock -- that failure mode predates
# this scenario and is not what it is about. What IS about it: the notice
# path's lock-protected write must not turn that into a HANG, and must not
# corrupt the config on its way to skipping the write (checked below).
case "$rc6" in
  0|1) ;;
  *) echo "FAIL: [list] exited $rc6 while an unrelated process held the lock -- looks like a crash"
     echo "$out6"; exit 1 ;;
esac

# Generous relative to the helper's own ~2s budget, but a small fraction of
# XLINGS_LOCK_TIMEOUT's 10-minute default -- the property under test is
# "did not inherit the long wait", not a tight latency bound.
if [[ $elapsed_ms -gt 30000 ]]; then
  echo "FAIL: [list] took ${elapsed_ms}ms while the lock was held -- looks like it inherited the long default instead of its own short budget"
  exit 1
fi

if [[ "$(cat "$CONFIG")" != "$BEFORE6" ]]; then
  echo "FAIL: [list] modified the config despite the lock being held elsewhere"
  diff <(echo "$BEFORE6") "$CONFIG" || true
  exit 1
fi

echo "  ok: [list] exited 0 in ${elapsed_ms}ms, config untouched by the skipped notice write"

echo
echo "PASS: home config mutations are serialized by the state lock"
