#!/usr/bin/env bash
# non_interactive_contract_test.sh — no xlings command may block waiting for a
# keypress, and none may report success without having done anything.
#
# `xlings use <name>` (no version) decided what to do by asking whether a
# terminal was attached. With one it opened an arrow-key picker and blocked
# until somebody pressed a key; without one it printed the version list and
# returned 0 having changed nothing. Both halves are unusable to anything
# driving xlings, and the TTY gate does not even separate the two populations
# it was meant to -- agents and terminal-automation tools routinely allocate a
# pty, so they took the *blocking* branch.
#
# The contract this locks down:
#
#   * whether a human is at the keyboard is not detectable, so it must not
#     decide semantics -- only presentation;
#   * a command with a single correct outcome performs it (exit 0);
#   * an ambiguous one changes nothing, lists what it could have done, and
#     says so in words -- exit 0, because a query that answered itself did
#     not fail (2026.7.31.3; it was exit 2 before, from applying "did nothing
#     => non-zero" to a command that is not an action);
#   * nothing waits for input that was never promised a way to arrive.
#
# Every case runs under `timeout`, so a regression that reintroduces a blocking
# prompt fails here instead of hanging CI for its full budget.
#
# Refs: .agents/docs/2026-07-30-cli-determinism-and-followup-plan.md §2
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/non_interactive_contract"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() {
  chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

# stdin is closed for every run: a command that reads it here is a command
# that would have hung.
RUN() {
  ( cd /tmp && timeout 30 env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
      "$XLINGS_BIN" "$@" </dev/null )
}

# The same, wrapped in a pseudo-terminal. This is the case the old TTY gate
# got wrong: `stdin_is_terminal()` is true in here, so this is exactly what an
# agent that allocates a pty was hitting.
HAVE_PTY=0
if command -v script >/dev/null 2>&1; then HAVE_PTY=1; fi

RUN_PTY() {
  local cmd
  cmd="$(printf '%q ' env -i HOME="$HOME" PATH=/usr/bin:/bin \
           XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
           "$XLINGS_BIN" "$@")"
  case "$(uname -s)" in
    Darwin|FreeBSD) ( cd /tmp && timeout 30 script -q /dev/null /bin/sh -c "$cmd" </dev/null ) ;;
    *)              ( cd /tmp && timeout 30 script -qec "$cmd" /dev/null </dev/null ) ;;
  esac
}

# `timeout` exits 124 on expiry; that is the failure this whole file exists to
# catch, so it gets its own message rather than being lumped in with "wrong
# exit code".
rc_of() {
  local rc=0
  "$@" >/dev/null 2>&1 || rc=$?
  if [[ $rc -eq 124 ]]; then
    fail "command blocked until the 30s timeout: $*"
  fi
  printf '%s\n' "$rc"
}

mkdir -p "$HOME_DIR/subos/default/bin" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/n"

# Two fixtures: one that ships a single version (unambiguous) and one that
# ships two (ambiguous). Both print their own version so a switch is provable
# by running the shim, not only by reading state.
write_probe() {
  local name="$1"; shift
  local versions=("$@")
  {
    printf 'package = {\n'
    printf '    spec = "1", name = "%s",\n' "$name"
    printf '    description = "non-interactive contract fixture",\n'
    printf '    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",\n'
    printf '    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},\n'
    printf '    xpm = {\n'
    for os in linux macosx windows; do
      printf '        %s = { ' "$os"
      for v in "${versions[@]}"; do printf '["%s"] = {}, ' "$v"; done
      printf '},\n'
    done
    printf '    },\n}\n'
    cat <<'LUA'
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, pkginfo.name()),
                 "#!/bin/sh\necho \"probe " .. pkginfo.version() .. "\"\n")
    return true
end
function config()
    xvm.add(pkginfo.name(), { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall() xvm.remove(pkginfo.name()) return true end
LUA
  } > "$LOCAL_INDEX_DIR/pkgs/n/$name.lua"
}

write_probe ni-one 1.0.0
write_probe ni-two 1.0.0 2.0.0

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN self init >/dev/null 2>&1 || fail "self init failed"
RUN install ni-one >/dev/null 2>&1 || fail "install ni-one failed"
RUN install ni-two@1.0.0 >/dev/null 2>&1 || fail "install ni-two@1.0.0 failed"
RUN install ni-two@2.0.0 >/dev/null 2>&1 || fail "install ni-two@2.0.0 failed"

probe_says() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      XLINGS_ACTIVE_SUBOS=default "$HOME_DIR/subos/default/bin/$1" 2>&1 || true )
}
# io.writefile leaves the payload non-executable; the recipe is a fixture, not
# a real package, so the bit is set here rather than pretending in Lua.
find "$HOME_DIR/data/xpkgs" -type f -name 'ni-*' -exec chmod +x {} +

# ── N1: one candidate → LIST, exit 0, nothing changed ────────────────
#
# This case used to assert the opposite: that a single candidate SWITCHED.
# That was 2026.7.30.2's rule -- "whether the command has a single correct
# outcome is detectable, and that is what decides" -- and on its own terms it
# is sound.
#
# What it collided with is `--help`, which has always said of the version
# argument: "omit to list installed versions". So one typed command was a
# QUERY or a MUTATION depending on the candidate count, and the count is
# "versions opted into THIS subos" -- not "versions I installed", and not
# anything the user can see before pressing enter. Measured on a real home:
# `use gcc --all` listed five while `use gcc` wrote state, because one of the
# five was opted into that subos.
#
# So the meaning is fixed instead of the outcome: bare `use <name>` lists, at
# any count. Naming a version switches (N5). The sysroot repair that relied on
# re-running `use` on the active version is now spelled `use <name> <version>`,
# which works at any count rather than only at exactly one.
log "N1: a single installed version lists, exactly as several do"
rc="$(rc_of RUN use ni-one)"
[[ "$rc" == "0" ]] || fail "N1: expected exit 0, got $rc"
out="$(RUN use ni-one 2>&1 || true)"
grep -q "1.0.0" <<<"$out" || fail "N1: the version was not listed; got:\n$out"
# And specifically NOT a switch. `->` is how cmd_use announces one.
if grep -q -- "ni-one -> " <<<"$out"; then
  fail "N1: bare use switched instead of listing; got:\n$out"
fi

# ── N2: several candidates → list them, exit 0, nothing changed ──────
#
# What must hold is "nothing changed, and the user can see why" -- not that it
# reports failure. `use <name>` with no version is a question, and it answers
# it; the switch is requested by naming a version (N5), which is where a
# non-zero exit means something. What is still forbidden is the *original*
# defect: printing a list when a single candidate made the outcome
# unambiguous (N1 locks that side).
log "N2: several installed versions list without switching, exit 0"
before="$(probe_says ni-two)"
rc="$(rc_of RUN use ni-two)"
[[ "$rc" == "0" ]] || fail "N2: expected exit 0 for a listing, got $rc"
out="$(RUN use ni-two 2>&1 || true)"
grep -q "1.0.0" <<<"$out" || fail "N2: the candidates were not listed; got:\n$out"
grep -q "2.0.0" <<<"$out" || fail "N2: the candidates were not listed; got:\n$out"
grep -q "use ni-two" <<<"$out" \
  || fail "N2: no command was named for making the choice; got:\n$out"
# The listing must not be rendered as a failure -- that is what made a normal
# query look like a fault.
if grep -qi "\[error\]" <<<"$out"; then
  fail "N2: a listing was rendered as an error; got:\n$out"
fi
# The version list belongs in the panel, once. It used to be repeated in the
# error hint line.
[[ "$(grep -c "2\.0\.0" <<<"$out")" == "1" ]] \
  || fail "N2: the version list was printed more than once; got:\n$out"
after="$(probe_says ni-two)"
[[ "$before" == "$after" ]] \
  || fail "N2: a listing changed the active version: '$before' -> '$after'"

# ── N3: the same, inside a pseudo-terminal ───────────────────────────
#
# The branch an agent actually reaches. Before this change it opened a picker
# and waited forever; `timeout 30` is what proves it no longer does.
if [[ "$HAVE_PTY" == "1" ]]; then
  log "N3: a pty does not turn the listing back into a blocking prompt"
  rc="$(rc_of RUN_PTY use ni-two)"
  [[ "$rc" == "0" ]] || fail "N3: expected exit 0 under a pty, got $rc"
  before="$(probe_says ni-two)"
  RUN_PTY use ni-two >/dev/null 2>&1 || true
  [[ "$before" == "$(probe_says ni-two)" ]] \
    || fail "N3: a pty listing switched the active version"
else
  log "N3: SKIP (no \`script\` binary to allocate a pty)"
fi

# ── N4: --pick is gone, and its removal is not a silent no-op ────────
#
# `--pick` existed to give the removed picker an explicit door. Once the
# default path is deterministic there is nothing behind that door, and a flag
# that is quietly ignored is worse than one that does not exist -- a script
# passing it would believe it had asked for something.
log "N4: --pick is rejected as an unknown flag, not silently ignored"
rc="$(rc_of RUN use ni-two --pick)"
[[ "$rc" != "0" ]] || fail "N4: --pick was accepted and silently ignored"
[[ "$rc" != "124" ]] || fail "N4: --pick blocked"

# ── N5: naming the version still works ───────────────────────────────
log "N5: an explicit version switches"
rc="$(rc_of RUN use ni-two 1.0.0)"
[[ "$rc" == "0" ]] || fail "N5: expected exit 0, got $rc"
grep -q "probe 1.0.0" <<<"$(probe_says ni-two)" \
  || fail "N5: the explicit switch did not take effect"
rc="$(rc_of RUN use ni-two@2.0.0)"
[[ "$rc" == "0" ]] || fail "N5: expected exit 0 for name@ver, got $rc"
grep -q "probe 2.0.0" <<<"$(probe_says ni-two)" \
  || fail "N5: the name@ver switch did not take effect"

# ── N6: install / remove never wait for a confirmation ───────────────
#
# These already have `-y`, but `-y` is only half the contract: without it they
# must still terminate rather than sit on a prompt nobody can answer.
log "N6: install and remove terminate with stdin closed"
rc="$(rc_of RUN install ni-one)"        # already installed: nothing to confirm
[[ "$rc" == "0" ]] || fail "N6: repeat install exited $rc"
rc="$(rc_of RUN remove ni-one -y)"
[[ "$rc" == "0" ]] || fail "N6: remove -y exited $rc"
if [[ "$HAVE_PTY" == "1" ]]; then
  rc="$(rc_of RUN_PTY install ni-one -y)"
  [[ "$rc" == "0" ]] || fail "N6: install -y under a pty exited $rc"
fi

# ── N8: the question goes to stderr, and EOF is not an answer ────────
#
# Confirmations are asked whenever stdin is a terminal now, which makes two
# things load-bearing that were not before:
#
#   * The question must not go to STDOUT. `xlings remove foo > log` with a
#     terminal on stdin would put it in the file and show the user a process
#     that looks hung -- and really is waiting for them.
#   * EOF must not be answered on the user's behalf. `ui::confirm` returned its
#     default there, which is the guess the whole non-interactive contract
#     exists to prevent, smuggled onto the interactive path.
#
# `RUN_PTY` cannot express this: it uses `script`, which merges everything the
# child writes into one stream. What is needed is a pty on STDIN ONLY, with
# stdout and stderr as separate pipes -- which is exactly the shape that
# breaks.
log "N8: the confirmation goes to stderr, and EOF cancels rather than guesses"
n8="$RUNTIME_DIR/n8"
mkdir -p "$n8"
set +e
XLINGS_BIN="$XLINGS_BIN" XLINGS_HOME="$HOME_DIR" N8_DIR="$n8" python3 - <<'PY8'
import os, pty, subprocess, sys, time

env = dict(os.environ)
env["XLINGS_ACTIVE_SUBOS"] = "default"
d = env["N8_DIR"]

# A pty for stdin, plain pipes for stdout/stderr.
master, slave = pty.openpty()
p = subprocess.Popen([env["XLINGS_BIN"], "remove", "ni-two"],
                     stdin=slave, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, env=env)
os.close(slave)
# Close the master so the child sees EOF on a terminal: somebody was there and
# then was not, which is not the same as answering.
time.sleep(1.0)
os.close(master)
try:
    out, err = p.communicate(timeout=30)
except subprocess.TimeoutExpired:
    p.kill()
    open(os.path.join(d, "timeout"), "w").write("1")
    out, err = p.communicate()
open(os.path.join(d, "stdout"), "wb").write(out)
open(os.path.join(d, "stderr"), "wb").write(err)
open(os.path.join(d, "rc"), "w").write(str(p.returncode))
PY8
set -e
[[ -f "$n8/timeout" ]] && fail "N8: the command hung on an unanswered confirmation"
if grep -qiE '\[y/N\]|\[Y/n\]' "$n8/stdout"; then
  fail "N8: the confirmation prompt was written to stdout:
$(cat "$n8/stdout")"
fi
grep -qiE '\[y/N\]|\[Y/n\]' "$n8/stderr" \
  || fail "N8: no confirmation was asked at all on a terminal stdin:
stdout: $(cat "$n8/stdout")
stderr: $(cat "$n8/stderr")"
# EOF is not a yes. Nothing may have been removed.
after="$(probe_says ni-two)"
grep -q "probe" <<<"$after" \
  || fail "N8: an unanswered confirmation removed the package anyway"

# ── N7: listing is a listing ─────────────────────────────────────────
#
# `list_installed_versions` is served by the same code that `use` used to
# reach. A caller asking to *see* the versions must not come back to a
# different active toolchain.
log "N7: the listing capability does not switch anything"
before="$(probe_says ni-two)"
rc="$(rc_of RUN list ni-two)"
after="$(probe_says ni-two)"
[[ "$before" == "$after" ]] \
  || fail "N7: listing changed the active version: '$before' -> '$after'"

log "PASS: non_interactive_contract"
