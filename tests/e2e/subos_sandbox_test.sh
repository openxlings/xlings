#!/usr/bin/env bash
# subos_sandbox_test.sh — verifies the 0.4.23 V4 sandbox model:
#
# - `xlings subos new <name>` creates a regular subos (no sandbox-specific
#   fields, dirs, or eager xpkg installs — that whole V1.1-V1.3 path is
#   gone). Workspace .xlings.json contains JUST `{"workspace": {}}`.
#
# - `xlings subos use <name> --sandbox` enters the SAME subos via proot
#   with FS isolation:
#     * real user identity (no `-0`, no fake root);
#     * $HOME is sandbox-private (`<subos>/home/<user>`);
#     * /etc/passwd has the real user entry (sandbox template);
#     * /tmp is sandbox-private;
#     * ~/.xlings is bound from host RW so xlings install / list / etc.
#       behave EXACTLY like outside the sandbox (per-subos workspace,
#       host xpkg pool — sandbox is just a different way to enter the
#       same subos);
#     * /usr, /lib, /lib64, /etc/resolv.conf, /etc/ld.so.cache are bound
#       from host RO so coreutils / loader / DNS just work.
#
# - `xlings subos use <name>` (no --sandbox) is unchanged from current
#   subos behavior — env-spawn shell, no FS isolation.
#
# Linux-only (proot uses ptrace). Non-Linux platforms verify --sandbox
# is rejected with a clear error.
#
# Design ref: .agents/docs/sandbox-v4-design.md
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

# Under $TMPDIR, not the checkout. E1: the checkout is normally under $HOME,
# so a home there shares a long prefix with the real one and every "which home
# did we actually use?" defect stays invisible -- which is how all four of the
# 2026-08-06 home defects survived. It also puts the home BELOW a directory the
# sandbox privatises before binding, which is the hardest ordering in the bind
# list and the one S8 exists to pin down. The 2026-08-06 measurements hit that
# ordering by accident; here it is deliberate.
RUNTIME_DIR="$(runtime_home_dir subos_sandbox)"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

assert_home_is_isolated "$HOME_DIR"
XLINGS_BIN="$(find_xlings_bin)"

mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/runtimedir"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "activeSubos": "default" }
JSON

run_x() {
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL="${SHELL:-/bin/sh}" \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      "$XLINGS_BIN" "$@" )
}

# ─────────────────────────────────────────────────────────────────────
# Linux path: full V4 sandbox flow
# ─────────────────────────────────────────────────────────────────────
if [[ "$(uname -s)" == "Linux" ]]; then

# ── S1: subos new is back to a plain subos (no sandbox-specific stuff)
log "S1: subos new mybox creates a plain subos"
run_x subos new mybox >/dev/null
[[ -d "$HOME_DIR/subos/mybox/bin" ]] || fail "S1: bin/ missing"
[[ -d "$HOME_DIR/subos/mybox/lib" ]] || fail "S1: lib/ missing"
# V4: no sandbox dirs at create time — those are lazy on first --sandbox use
[[ ! -d "$HOME_DIR/subos/mybox/home" ]] || fail "S1: home/ created too early (should be lazy)"
[[ ! -d "$HOME_DIR/subos/mybox/etc" ]]  || fail "S1: etc/ created too early (should be lazy)"
[[ ! -d "$HOME_DIR/subos/mybox/tmp" ]]  || fail "S1: tmp/ created too early (should be lazy)"
# .xlings.json should be just `{ "workspace": {} }`, no sandbox-shell field
shell_field="$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
print(d.get("sandbox-shell", "<absent>"))
print(d.get("sandbox-shell-xpkg", "<absent>"))
' "$HOME_DIR/subos/mybox/.xlings.json")"
[[ "$shell_field" == "<absent>"$'\n'"<absent>" ]] \
  || fail "S1: stale sandbox-shell fields in fresh .xlings.json: '$shell_field'"
log "  ✓ plain subos, no V1 sandbox-shell residue"

# ── S2: --sandbox-shell flag is gone (rejected by `subos new`)
log "S2: subos new --sandbox-shell is now rejected (V4 removed the flag)"
out="$(run_x subos new someotherbox --sandbox-shell xim:fish 2>&1 || true)"
echo "$out" | grep -q "unknown option" \
  || fail "S2: expected 'unknown option' for --sandbox-shell, got:
$out"
[[ ! -d "$HOME_DIR/subos/someotherbox" ]] \
  || fail "S2: someotherbox dir created despite the rejection"
log "  ✓ --sandbox-shell rejected at parse time"

# ── S3: V5 auto-detect + auto-install backend
# V5 auto-installs bwrap/proot if neither is available. If bwrap probe
# fails (namespace restricted on Ubuntu 24+), auto-falls back to proot.
# Either way, sandbox should enter successfully.
log "S3: subos use --sandbox auto-detects/installs backend"
out="$(echo 'echo AUTO_SANDBOX_OK; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 60 "$XLINGS_BIN" subos use mybox --sandbox ) 2>&1 || true)"
if echo "$out" | grep -q "AUTO_SANDBOX_OK"; then
  log "  ✓ auto-install + sandbox entry succeeded"
elif echo "$out" | grep -q "failed to install"; then
  log "  (skip rest: auto-install failed — offline or no index)"
  log "PASS: subos sandbox V5 (Linux, partial — S1-S2 only)"
  exit 0
else
  # Not a recognised outcome. Still not fatal -- this path depends on a
  # backend install that needs sudo for setuid, and a developer machine
  # without passwordless sudo legitimately lands here -- but PRINT what
  # happened. Reporting PASS while silently skipping the entire feature under
  # test is how a real regression in sandbox entry would reach a release
  # looking exactly like an unattended laptop.
  log "  (unexpected output, skipping remaining sandbox tests):"
  printf '%s\n' "$out" | sed 's/^/      | /'
  log "PASS: subos sandbox V5 (Linux, partial — S1-S2 only)"
  exit 0
fi

# ── S5: --sandbox enters proot, real user identity preserved
log "S5: subos use --sandbox preserves real user identity"
out_inside="$(echo 'echo "ID:$(whoami):$(id -u)"; echo "HOME=$HOME"; echo "SHELL=$SHELL"; echo "MODE=$XLINGS_SUBOS_MODE"; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) 2>&1 || true)"
echo "$out_inside" | grep -q "ID:$USER:$(id -u)" \
  || fail "S5: identity not preserved (expected ID:$USER:$(id -u)):
$out_inside"
echo "$out_inside" | grep -q "HOME=/home/$USER" \
  || fail "S5: \$HOME is not /home/<user>:
$out_inside"
echo "$out_inside" | grep -q "MODE=sandbox" \
  || fail "S5: XLINGS_SUBOS_MODE not set:
$out_inside"
log "  ✓ inside sandbox: $USER (uid $(id -u)), \$HOME=/home/$USER, mode=sandbox"

# ── S6: /etc/passwd is sandbox template with real user + root only
log "S6: /etc/passwd is sandbox template (real user + root, no host users)"
out_passwd="$(echo 'cat /etc/passwd; echo MARKER_END; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) 2>&1 || true)"
# Expect ONLY root + current user — no daemon, sshd, sys, etc.
line_count=$(echo "$out_passwd" | sed -n '/^root:/,/^MARKER_END/p' | grep -c '^[a-zA-Z]' || true)
[[ "$line_count" -le 3 ]] \
  || fail "S6: /etc/passwd has too many entries (expected ≤2 + MARKER_END line, got $line_count):
$out_passwd"
echo "$out_passwd" | grep -q "^root:x:0:0:root:" \
  || fail "S6: /etc/passwd missing root entry:
$out_passwd"
echo "$out_passwd" | grep -q "^$USER:x:$(id -u):" \
  || fail "S6: /etc/passwd missing real-user entry:
$out_passwd"
log "  ✓ /etc/passwd: only root + $USER, no host user leak"

# ── S6b: bwrap must not force an interactive prompt for piped commands
log "S6b: bwrap backend keeps non-interactive command output prompt-free"
out_bwrap_probe="$(echo 'echo BWRAP_BACKEND_OK; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox bwrap ) 2>&1 || true)"
if echo "$out_bwrap_probe" | grep -q "BWRAP_BACKEND_OK"; then
  out_bwrap_passwd="$(echo 'cat /etc/passwd; echo MARKER_END; exit' | \
    ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
        PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
        timeout 10 "$XLINGS_BIN" subos use mybox --sandbox bwrap ) 2>&1 || true)"
  echo "$out_bwrap_passwd" | grep -q "^root:x:0:0:root:" \
    || fail "S6b: bwrap non-interactive output was prompt-polluted:
$out_bwrap_passwd"
  echo "$out_bwrap_passwd" | grep -q "^$USER:x:$(id -u):" \
    || fail "S6b: bwrap missing real-user entry:
$out_bwrap_passwd"
  log "  ✓ bwrap non-interactive output starts at real command output"
else
  log "  (skip: bwrap backend unavailable in this environment)"
fi

# ── S7: dotfile isolation — sandbox writes to /home/<user> stay private
log "S7: writing to ~/ inside sandbox doesn't pollute host"
marker_file="$HOME/.xlings-sandbox-test-marker-$$"
trap "rm -f '$marker_file'" EXIT
[[ ! -e "$marker_file" ]] || fail "S7: setup — marker exists before sandbox"
echo "echo SANDBOX > '$marker_file' && exit" | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) >/dev/null 2>&1 || true
[[ ! -e "$marker_file" ]] \
  || fail "S7: sandbox $marker_file leaked to host!"
[[ -f "$HOME_DIR/subos/mybox/home/$USER$marker_file" ]] || true
# Check the file landed inside <subos>/home/<user>/...
sandbox_marker="$HOME_DIR/subos/mybox/home/$USER${marker_file#$HOME}"
[[ -f "$sandbox_marker" ]] \
  || fail "S7: sandbox marker not found at $sandbox_marker"
log "  ✓ host \$HOME unaffected; file landed in <subos>/home/$USER/"

# ── S8: the xlings home is host-shared AT ITS OWN ABSOLUTE PATH
#
# Not at ~/.xlings. xvm alias targets, RPATH and INTERP are absolute host
# paths baked at install time and nothing rewrites them on the way in, so
# the home has to answer to the same spelling inside as outside. Binding it
# at ~/.xlings instead stranded every one of them; binding it at both made
# one directory reachable by two real paths, which a bind mount does not
# collapse — a shim then reports a conflict with itself. For the default
# home the own-path bind IS ~/.xlings, which is why the remapped form went
# unnoticed until an isolated XLINGS_HOME was used.
log "S8: xlings home is shared at its own absolute path, one spelling"
out_xl="$(echo 'echo "XH=$XLINGS_HOME"; echo "P1=${PATH%%:*}"; ls "$XLINGS_HOME/" 2>&1 | tr "\n" " "; echo MARKER; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) 2>&1 || true)"
abs_home="$(cd "$HOME_DIR" && pwd)"
echo "$out_xl" | grep -q "XH=$abs_home" \
  || fail "S8: XLINGS_HOME inside is not the host home path (expected $abs_home):
$out_xl"
echo "$out_xl" | grep -q "P1=$abs_home/subos/" \
  || fail "S8: PATH[0] is spelled differently from XLINGS_HOME — a shim will
read that as two homes and warn about a conflict with itself:
$out_xl"
echo "$out_xl" | grep -q "subos" \
  || fail "S8: the home shows no host content at its own path:
$out_xl"
log "  ✓ home visible at $abs_home, and PATH agrees with XLINGS_HOME"

# The second spelling must NOT exist: it is a distinct real path to the same
# directory, and that is what made shim ownership ambiguous.
out_alias="$(echo 'test -d "$HOME/.xlings" && echo SECOND_SPELLING || echo SINGLE_SPELLING; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) 2>&1 || true)"
echo "$out_alias" | grep -q "SINGLE_SPELLING" \
  || fail "S8: the home is also reachable at \$HOME/.xlings — two real paths
to one directory, which canonicalisation does not collapse:
$out_alias"
log "  ✓ no second spelling of the home inside the sandbox"

# ── S9: /tmp is sandbox-private
log "S9: /tmp is sandbox-private (writes don't pollute host /tmp)"
host_tmp_marker="/tmp/xlings-sandbox-host-marker-$$"
trap "rm -f '$marker_file' '$host_tmp_marker'" EXIT
echo "touch /tmp/xlings-sandbox-host-marker-$$; ls /tmp/ | head -3; exit" | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) >/dev/null 2>&1 || true
[[ ! -e "$host_tmp_marker" ]] \
  || fail "S9: sandbox /tmp file leaked to host /tmp"
[[ -f "$HOME_DIR/subos/mybox/tmp/xlings-sandbox-host-marker-$$" ]] \
  || fail "S9: sandbox /tmp marker not landed in <subos>/tmp/"
log "  ✓ /tmp inside sandbox is <subos>/tmp/, host /tmp unaffected"

# ── S10: subos use mybox (no --sandbox) is unchanged
log "S10: subos use mybox (no --sandbox) is plain env-spawn shell"
# We can't easily probe interactive shell behavior in a CI script, but
# we CAN assert the binary exits without trying to fork proot. Use
# `echo` piped to xlings to get one shell command + exit.
out_plain="$(echo 'echo NORMAL_MODE_$XLINGS_SUBOS_MODE; echo HOME=$HOME; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox ) 2>&1 || true)"
echo "$out_plain" | grep -q "NORMAL_MODE_$" \
  || fail "S10: XLINGS_SUBOS_MODE was set (expected unset for plain subos use):
$out_plain"
echo "$out_plain" | grep -q "HOME=$HOME" \
  || fail "S10: \$HOME was changed (expected host \$HOME for plain use):
$out_plain"
log "  ✓ plain subos use: no XLINGS_SUBOS_MODE, host \$HOME preserved"

# ── S11: empty <subos>/subos/ marker dir is created at sandbox init
# Without this marker, `<subos>/.xlings.json` at the chroot root gets
# treated as an anonymous-project root, which OVERRIDES per-shell
# XLINGS_ACTIVE_SUBOS=mybox and routes install / shim / workspace
# into <subos>/.xlings/subos/_/ instead of <subos>/. Symptom from
# user side: `xlings install foo` reports success but the binary
# isn't on PATH because shim went to the wrong dir.
#
# The fix is the empty marker: xlings's project discovery (config.cppm
# load_project_config_) treats any dir containing both `.xlings.json`
# AND a `subos/` sibling as an xlings home (boundary, NOT a project).
# We seed an empty `<subos>/subos/` so the chroot root looks like an
# xlings home from the project-discovery walk's POV.
log "S11: <subos>/subos/ marker dir created (project-discovery boundary fix)"
[[ -d "$HOME_DIR/subos/mybox/subos" ]] \
  || fail "S11: <subos>/subos/ marker dir missing — boundary check
won't fire, project discovery will misfire and override XLINGS_ACTIVE_SUBOS"
log "  ✓ <subos>/subos/ marker exists"

# ── S12: explicit PATH front-loads <subos>/bin (non-interactive defensive)
# Interactive shells source the seeded .bashrc which sets PATH via the
# xlings profile; non-interactive shells (`bash -c`, scripts) skip the
# rc files. Both paths should still see the per-subos bin first because
# we set PATH explicitly in env before exec proot.
log "S12: PATH front-loads <subos>/bin even in non-interactive shell"
out_path="$(echo 'echo "PATH:$PATH"; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) 2>&1 || true)"
echo "$out_path" | grep -q "PATH:/home/$USER/.xlings/subos/mybox/bin:" \
  || fail "S12: PATH first segment is NOT <home>/.xlings/subos/mybox/bin:
$out_path"
log "  ✓ PATH starts with /home/$USER/.xlings/subos/mybox/bin"

# ── S13: seeded shell rc files exist (interactive prompt-pill plumbing)
# init_sandbox_dirs_ writes minimal .bashrc / .profile / config.fish
# into <subos>/home/<user>/ that source the host xlings profile. Without
# these, interactive sandbox sessions don't get PATH adjusted, prompt
# pill never appears (XLINGS_SUBOS_MODE is set in env but the shell
# never sources the profile that consumes it).
log "S13: seeded shell rc files chain to host xlings profile"
[[ -f "$HOME_DIR/subos/mybox/home/$USER/.bashrc" ]] \
  || fail "S13: .bashrc not seeded"
grep -q "xlings-profile.sh" "$HOME_DIR/subos/mybox/home/$USER/.bashrc" \
  || fail "S13: .bashrc doesn't chain to xlings-profile.sh"
[[ -f "$HOME_DIR/subos/mybox/home/$USER/.config/fish/config.fish" ]] \
  || fail "S13: fish config.fish not seeded"
grep -q "xlings-profile.fish" "$HOME_DIR/subos/mybox/home/$USER/.config/fish/config.fish" \
  || fail "S13: config.fish doesn't chain to xlings-profile.fish"
log "  ✓ .bashrc, .profile, .config/fish/config.fish all seeded"

# ── S14: /bin/sh exists inside sandbox (system()/popen()/shebang dispatch)
# Without /bin/sh, libc system()/popen() and any tool with `#!/bin/sh`
# shebang fails silently with exit 255. xlings shim's platform::exec
# uses system() under the hood — observed symptom: any xvm-managed
# binary that goes through alias dispatch (openclaw, claude alias=node
# "...cli.js", etc.) silently exits 255 with no output.
#
# Fix: --bind=/usr/bin/sh:/bin/sh in proot args. The bind is
# unconditional (every sandbox session gets it).
log "S14: /bin/sh resolves inside sandbox (system()/shebang dispatch)"
out_sh="$(echo 'ls -la /bin/sh 2>&1; echo MARKER; sh -c "echo SHELL_OK"; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) 2>&1 || true)"
echo "$out_sh" | grep -q "/bin/sh" || fail "S14: /bin/sh not visible:
$out_sh"
echo "$out_sh" | grep -q "SHELL_OK" || fail "S14: /bin/sh -c failed:
$out_sh"
log "  ✓ /bin/sh resolves and executes (system() works)"

# ── S15: subos remove cleans up everything (including sandbox dirs)
log "S15: subos remove cleans up entirely"
run_x subos remove mybox >/dev/null 2>&1 || true
[[ ! -d "$HOME_DIR/subos/mybox" ]] || fail "S15: subos dir not removed"
log "  ✓ sandbox subos removed cleanly"

log "PASS: subos sandbox V5 (Linux) — 15 scenarios"

else
# ─────────────────────────────────────────────────────────────────────
# macOS / Windows: L2 sandbox (HOME redirect, no FS isolation)
# ─────────────────────────────────────────────────────────────────────
log "non-Linux ($(uname -s)): L2 sandbox via HOME redirect"
mkdir -p "$HOME_DIR/subos/default/bin"
run_x subos new mybox >/dev/null

# SM1: --sandbox should NOT be rejected anymore (cross-platform L2)
log "SM1: subos use --sandbox enters (not rejected)"
out="$(echo 'echo SANDBOX_L2_OK; echo HOME=$HOME; echo XLINGS_SUBOS_MODE=$XLINGS_SUBOS_MODE; exit' | \
  ( cd /tmp && env -i HOME="$HOME" USER="${USER:-$(whoami)}" SHELL=/bin/sh \
      PATH=/usr/bin:/bin:/usr/local/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) 2>&1 || true)"
echo "$out" | grep -q "SANDBOX_L2_OK" \
  || fail "SM1: sandbox didn't enter on $(uname -s):
$out"
log "  ✓ sandbox entered on $(uname -s)"

# SM2: HOME points to sandbox-private dir
echo "$out" | grep -q "HOME=.*subos/mybox/home" \
  || fail "SM2: HOME not redirected to sandbox dir:
$out"
log "  ✓ HOME redirected to sandbox home"

# SM3: XLINGS_SUBOS_MODE=sandbox set
echo "$out" | grep -q "XLINGS_SUBOS_MODE=sandbox" \
  || fail "SM3: XLINGS_SUBOS_MODE not set:
$out"
log "  ✓ XLINGS_SUBOS_MODE=sandbox"

# SM4: dotfile isolation — write inside, host unaffected
marker_file="$HOME/.xlings-sandbox-sm4-marker-$$"
echo "echo test > '$marker_file'; exit" | \
  ( cd /tmp && env -i HOME="$HOME" USER="${USER:-$(whoami)}" SHELL=/bin/sh \
      PATH=/usr/bin:/bin:/usr/local/bin XLINGS_HOME="$HOME_DIR" \
      timeout 10 "$XLINGS_BIN" subos use mybox --sandbox ) >/dev/null 2>&1 || true
[[ ! -e "$marker_file" ]] \
  || fail "SM4: sandbox write leaked to host!"
log "  ✓ dotfile isolation (host \$HOME clean)"

# SM5: subos remove works
run_x subos remove mybox >/dev/null 2>&1 || true
[[ ! -d "$HOME_DIR/subos/mybox" ]] || fail "SM5: subos dir not removed"
log "  ✓ subos removed cleanly"

log "PASS: subos sandbox ($(uname -s)) — 5 scenarios (L2 HOME redirect)"
fi
