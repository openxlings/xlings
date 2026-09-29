#!/usr/bin/env bash
# E2E: the commands an install hook runs write to the hook's log, not to the
# terminal the install's own output owns (libxpkg ExecutionContext::hook_log).
#
# Measured on 2026.9.29.1: `xlings install chatgpt` reinstalled qt-base, whose
# hook runs 7-Zip five times, and 7-Zip's banners, listings and an ERROR that
# the recipe had already repaired were printed between `[74/88]` and
# `[75/88]`. The hook's own print() was already captured (and shown on
# failure); the processes it started were not.
#
#   H1  a hook's commands (stdout and stderr) and its print() land in
#       <home>/logs/hooks/<ns>-<name>@<version>.install.log, in order, and not
#       in the install's output; `system.exec(cmd, {tty = true})` still
#       reaches the terminal and not the log
#   H2  a hook that fails prints its last lines, not all of them, and where
#       the full log is
#   H3  XLINGS_HOOK_OUTPUT=inherit restores the terminal for the commands
#   H4  a hook that runs long says so (a heartbeat line naming the package, the
#       hook, the elapsed time and the log's last line)
#   H5  the interface: stdout stays NDJSON with a `hook` progress event, and the
#       commands' output reaches neither stdout nor stderr (it used to be
#       forwarded to stderr as `[stray stdout]` lines)
#
# Offline: hook-only fixtures.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/hook_output_log"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() {
  chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_NON_INTERACTIVE=1 \
      ${HOOK_ENV:-} \
      "$XLINGS_BIN" "$@" )
}

write_recipe() {  # <name> <install() body>
  local name="$1" body="$2"
  mkdir -p "$LOCAL_INDEX_DIR/pkgs/${name:0:1}"
  cat > "$LOCAL_INDEX_DIR/pkgs/${name:0:1}/$name.lua" <<LUA
package = {
    spec = "1", name = "$name", description = "hook output fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "aarch64"}, status = "stable", categories = {"test-fixture"},
    xpm = { linux = { ["1.0"] = {} }, macosx = { ["1.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.system")
function install()
$body
    io.writefile(path.join(pkginfo.install_dir(), "marker"), "x\\n")
    return true
end
function uninstall() return true end
LUA
}

mkdir -p "$HOME_DIR/subos/default/bin"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"

write_recipe hooklog '
    print("HOOK-PRINT-1")
    system.exec("echo HOOK-STDOUT; echo HOOK-STDERR 1>&2")
    print("HOOK-PRINT-2")
    system.exec("echo HOOK-TTY", { tty = true })'
write_recipe hookfail '
    system.exec("i=1; while [ $i -le 25 ]; do printf \"line-%02d\\n\" $i; i=$((i+1)); done")
    system.exec("exit 3")'
write_recipe hookslow '
    system.exec("echo SLOW-PROGRESS; sleep 4")'
write_recipe hookiface '
    system.exec("echo IFACE-STDOUT; echo IFACE-STDERR 1>&2")'
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"

cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON
RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

LOGS="$HOME_DIR/logs/hooks"

# ── H1 ───────────────────────────────────────────────────────────────
log "H1: a hook's commands write to its log, not to the install's output"
OUT="$(RUN install hooklog@1.0 -y 2>&1)" || fail "H1: install failed:
$OUT"
for word in HOOK-STDOUT HOOK-STDERR HOOK-PRINT-1; do
  if grep -q "$word" <<<"$OUT"; then
    fail "H1: '$word' reached the install's output:
$OUT"
  fi
done
grep -q "HOOK-TTY" <<<"$OUT" || fail "H1: a {tty = true} command did not reach the terminal:
$OUT"
LOG1="$LOGS/xim-hooklog@1.0.install.log"
[[ -f "$LOG1" ]] || fail "H1: no hook log at $LOG1 (have: $(ls "$LOGS" 2>/dev/null))"
ORDER="$(grep -oE 'HOOK-(PRINT-1|STDOUT|STDERR|PRINT-2|TTY)' "$LOG1" | tr '\n' ' ')"
[[ "$ORDER" == "HOOK-PRINT-1 HOOK-STDOUT HOOK-STDERR HOOK-PRINT-2 " ]] \
  || fail "H1: the log is not the hook's output in order (got: '$ORDER'):
$(cat "$LOG1")"
log "  ok"

# ── H2 ───────────────────────────────────────────────────────────────
log "H2: a failing hook prints its last lines and where the log is"
set +e
OUT="$(RUN install hookfail@1.0 -y 2>&1)"
RC=$?
set -e
[[ "$RC" != "0" ]] || fail "H2: a failing hook installed:
$OUT"
grep -q "line-25" <<<"$OUT" || fail "H2: the last line is missing:
$OUT"
if grep -q "line-01" <<<"$OUT"; then
  fail "H2: the whole output was printed instead of its tail:
$OUT"
fi
grep -q "full log: .*xim-hookfail@1.0.install.log" <<<"$OUT" \
  || fail "H2: the log's path is missing:
$OUT"
grep -q "line-01" "$LOGS/xim-hookfail@1.0.install.log" \
  || fail "H2: the log does not hold the whole output"
log "  ok"

# ── H3 ───────────────────────────────────────────────────────────────
log "H3: XLINGS_HOOK_OUTPUT=inherit gives the commands the terminal again"
RUN remove hooklog -y >/dev/null 2>&1 || true
OUT="$(HOOK_ENV="XLINGS_HOOK_OUTPUT=inherit" RUN install hooklog@1.0 -y 2>&1)" \
  || fail "H3: install failed:
$OUT"
grep -q "HOOK-STDOUT" <<<"$OUT" || fail "H3: the command's output did not reach the terminal:
$OUT"
log "  ok"

# ── H4 ───────────────────────────────────────────────────────────────
log "H4: a long hook says it is still running"
OUT="$(HOOK_ENV="XLINGS_HOOK_HEARTBEAT=1:1" RUN install hookslow@1.0 -y 2>&1)" \
  || fail "H4: install failed:
$OUT"
grep -qE "xim:hookslow@1.0 install hook running [0-9]+s: SLOW-PROGRESS" <<<"$OUT" \
  || fail "H4: no heartbeat line:
$OUT"
if grep -qx "SLOW-PROGRESS" <<<"$OUT"; then
  fail "H4: the command's own output reached the terminal:
$OUT"
fi
log "  ok"

# ── H5 ───────────────────────────────────────────────────────────────
log "H5: the interface's stdout stays NDJSON and the commands reach neither stream"
RUN remove hookslow -y >/dev/null 2>&1 || true
ERR_FILE="$RUNTIME_DIR/iface.err"
IFACE="$(HOOK_ENV="XLINGS_HOOK_HEARTBEAT=1:1" RUN interface install_packages \
          --args '{"targets":["hookiface@1.0","hookslow@1.0"],"yes":true}' 2>"$ERR_FILE" || true)"
python3 - "$IFACE" <<'PY' || fail "H5: stdout is not clean NDJSON (see above)"
import json, sys
hook = 0
for n, line in enumerate(sys.argv[1].splitlines(), 1):
    if not line.strip():
        continue
    try:
        ev = json.loads(line)
    except ValueError:
        print(f"H5: line {n} is not JSON: {line!r}")
        sys.exit(1)
    if ev.get("kind") == "progress" and ev.get("phase") == "hook":
        hook += 1
if hook == 0:
    print("H5: no `hook` progress event")
    sys.exit(1)
PY
for word in IFACE-STDOUT IFACE-STDERR; do
  if grep -q "$word" "$ERR_FILE"; then
    fail "H5: '$word' reached stderr:
$(cat "$ERR_FILE")"
  fi
done
grep -q "IFACE-STDOUT" "$LOGS/xim-hookiface@1.0.install.log" \
  || fail "H5: the interface install wrote no hook log"
log "  ok"

log "PASS: hook output log"
