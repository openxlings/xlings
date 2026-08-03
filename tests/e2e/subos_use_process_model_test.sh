#!/usr/bin/env bash
# `xlings subos use <name>` (interactive, no --cmd) must REPLACE the xlings
# process with the shell on POSIX, and `--cmd` must spawn-and-wait so the
# child's exit code survives. Those are two different process models and the
# difference is observable, so it gets asserted rather than assumed.
#
# Why it matters: exec(2) hands the terminal to the shell outright -- the shell
# becomes the foreground process group leader, Ctrl-C reaches only it, job
# control works, and `exit` returns to the parent shell. A forked child that
# xlings waits on leaves xlings in the same foreground process group, so SIGINT
# is delivered to xlings too and it can die on its default disposition while
# the child still owns the tty. Every existing SubOS test drives `--cmd`, so
# the interactive model had no coverage at all.
#
# The probe: a launcher that prints its own PID and then execs xlings. If
# xlings execs the shell in turn, `$$` inside the shell is that same PID.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

bin=$(find_xlings_bin)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
home="$root/.xlings"

# /bin/sh, not the developer's login shell: `echo $$` is POSIX, while fish
# spells it `$fish_pid`. Setting it here also exercises XLINGS_SHELL on POSIX,
# which used to be honoured on Windows only.
export XLINGS_SHELL=/bin/sh

run() { HOME="$root/user" XLINGS_HOME="$home" NO_COLOR=1 "$@"; }

mkdir -p "$root/user"
run "$bin" self init >/dev/null
run "$bin" subos new probe >/dev/null

launcher="$root/launcher.sh"
cat > "$launcher" <<'LAUNCH'
#!/usr/bin/env bash
echo "launcher=$$"
exec "$@"
LAUNCH
chmod +x "$launcher"

# --- interactive entry: the shell must inherit xlings's process ------------
output=$(printf 'echo "shell=$$"\nexit 0\n' \
    | HOME="$root/user" XLINGS_HOME="$home" NO_COLOR=1 \
      bash "$launcher" "$bin" subos use probe 2>/dev/null || true)

launcher_pid=$(sed -n 's/^launcher=//p' <<<"$output" | head -1)
shell_pid=$(sed -n 's/^shell=//p' <<<"$output" | head -1)
[[ -n "$launcher_pid" ]] || fail "launcher never reported its pid: $output"
[[ -n "$shell_pid" ]] || fail "interactive shell never ran: $output"
[[ "$launcher_pid" == "$shell_pid" ]] || fail \
  "interactive \`subos use\` forked instead of exec-replacing: xlings pid \
$launcher_pid, shell pid $shell_pid -- the shell is a child, so xlings stays \
in the foreground process group and shares its signals"

# --- one-shot: spawn and wait, exit code preserved -------------------------
set +e
run "$bin" subos use probe --cmd 'exit 37'
status=$?
set -e
[[ $status -eq 37 ]] || fail "--cmd lost the child exit code: got $status"

# A signalled child reports 128+n rather than collapsing to a generic failure.
set +e
run "$bin" subos use probe --cmd 'kill -TERM $$'
status=$?
set -e
[[ $status -eq 143 ]] || fail "--cmd did not report SIGTERM as 143: got $status"

echo "subos use process model: ok"
