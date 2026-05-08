#!/usr/bin/env bash
# subos_sandbox_test.sh — verifies the sandbox subos type:
#
#   * `subos new --sandbox-shell <xpkg>` creates the right layout, /etc/*
#     templates land where expected, .xlings.json carries the sandbox-shell
#     fields, AND eagerly installs the configured shell xpkg + symlinks the
#     binary into <sandbox>/bin/<basename> so `subos use` can run it
#     (added in 0.4.22).
#   * `subos use <sandbox>` correctly enters the proot-based sandbox shell.
#   * If the symlink dangles (e.g. user removed the underlying xpkg), the
#     next `subos use` self-heals by re-running hydration.
#
# Sandbox is Linux-only by design (proot uses ptrace + Linux syscall
# semantics); this test is gated on Linux. macOS / Windows variants only
# verify that --sandbox-shell at create time is rejected with a clear
# error.
#
# Design ref: docs/plans/2026-05-09-subos-sandbox-design.md
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_sandbox"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/s/sh.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/runtimedir"

# Private copy of the shared fixture index, neutralise sub-index repos.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

# Fixture xpkg `sh`: drops a tiny static-ish shell at
# <install_dir>/bin/sh and registers via xvm.add. The install
# hook prefers the system busybox (statically linked on most distros),
# falling back to /bin/sh otherwise — in either case the binary
# delivered through the xim install path is what we want the eager
# hydrate flow to symlink into the sandbox bin.
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "sh",
    description = "Local fixture for tests/e2e/subos_sandbox_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    -- Prefer system busybox (static, dispatches by argv[0] so the
    -- "sh" applet is available when invoked as bin/sh). Otherwise
    -- copy /bin/sh. The hydrate flow symlinks <install_dir>/bin/sh
    -- into <sandbox>/bin/sh for the sandbox shell.
    local src = "/bin/busybox"
    if not os.isfile(src) then src = "/bin/sh" end
    if os.isfile(src) then
        os.cp(src, path.join(bindir, "sh"))
    else
        io.writefile(path.join(bindir, "sh"),
                     "#!/bin/sh\nexec /bin/sh \"$@\"\n")
    end
    os.exec("chmod +x " .. path.join(bindir, "sh"))
    return true
end

function config()
    xvm.add("sh", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("sh")
    return true
end
LUA

cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
JSON

run_x() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      "$XLINGS_BIN" "$@" )
}

# ─────────────────────────────────────────────────────────────────────
# Linux path: full sandbox creation + entry
# ─────────────────────────────────────────────────────────────────────
if [[ "$(uname -s)" == "Linux" ]]; then

# ── S1: subos new --sandbox-shell creates layout + auto-hydrates shell
log "S1: subos new --sandbox-shell mybox xim:sh"
run_x subos new mybox --sandbox-shell xim:sh >/dev/null 2>&1

[[ -d "$HOME_DIR/subos/mybox" ]]              || fail "S1: subos dir not created"
[[ -d "$HOME_DIR/subos/mybox/bin" ]]          || fail "S1: bin/ missing"
[[ -d "$HOME_DIR/subos/mybox/etc" ]]          || fail "S1: etc/ missing"
[[ -d "$HOME_DIR/subos/mybox/root" ]]         || fail "S1: root/ missing (expected for sandbox subos)"
[[ -d "$HOME_DIR/subos/mybox/tmp" ]]          || fail "S1: tmp/ missing (expected for sandbox subos)"
[[ -f "$HOME_DIR/subos/mybox/.xlings.json" ]] || fail "S1: .xlings.json missing"

# Eager hydration: shell binary must be present at <sandbox>/bin/sh
# (created as a symlink to the xim payload's binary).
shell_link="$HOME_DIR/subos/mybox/bin/sh"
[[ -L "$shell_link" ]] || fail "S1: shell symlink missing at $shell_link"
[[ -f "$shell_link" ]] || fail "S1: shell symlink dangling at $shell_link"
target="$(readlink "$shell_link")"
[[ "$target" == *"data/xpkgs/xim-x-sh"* ]] \
  || fail "S1: shell symlink target unexpected: $target"
log "  ✓ layout + auto-installed shell (symlink: $target)"

# ── S2: /etc/* templates have correct content
log "S2: /etc/{passwd,group,hosts,nsswitch.conf} populated correctly"
grep -q "^root:x:0:0:root:/root:/bin/sh" "$HOME_DIR/subos/mybox/etc/passwd" \
  || fail "S2: /etc/passwd missing root entry"
grep -q "^root:x:0:" "$HOME_DIR/subos/mybox/etc/group" \
  || fail "S2: /etc/group missing root entry"
grep -q "127.0.0.1.*localhost" "$HOME_DIR/subos/mybox/etc/hosts" \
  || fail "S2: /etc/hosts missing localhost"
grep -q "hosts:.*files" "$HOME_DIR/subos/mybox/etc/nsswitch.conf" \
  || fail "S2: /etc/nsswitch.conf missing hosts: files"
log "  ✓ /etc templates present and well-formed"

# ── S3: .xlings.json has sandbox-shell + sandbox-shell-xpkg fields
log "S3: .xlings.json carries sandbox-shell metadata"
shell_value="$(python3 -c '
import json, sys
data = json.loads(open(sys.argv[1]).read())
print(data.get("sandbox-shell", "<missing>"))
print(data.get("sandbox-shell-xpkg", "<missing>"))
' "$HOME_DIR/subos/mybox/.xlings.json")"
[[ "$shell_value" == "/bin/sh"$'\n'"xim:sh" ]] \
  || fail "S3: sandbox-shell fields wrong: '$shell_value'"
log "  ✓ sandbox-shell=/bin/sh, sandbox-shell-xpkg=xim:sh"

# ── S4: regular subos (no --sandbox-shell) does NOT have sandbox fields
log "S4: regular subos retains its existing schema (no sandbox leak)"
run_x subos new normalbox >/dev/null
if grep -q "sandbox-shell" "$HOME_DIR/subos/normalbox/.xlings.json"; then
  fail "S4: regular subos accidentally got sandbox-shell field"
fi
[[ ! -d "$HOME_DIR/subos/normalbox/etc" ]] \
  || fail "S4: regular subos accidentally got etc/ directory"
[[ ! -d "$HOME_DIR/subos/normalbox/root" ]] \
  || fail "S4: regular subos accidentally got root/ directory"
log "  ✓ regular subos layout unchanged"

# ── S5: subos use sandbox without proot installed → clear error
log "S5: subos use requires proot — clear error if missing"
out="$(run_x subos use mybox 2>&1 || true)"
echo "$out" | grep -q "proot not found" \
  || fail "S5: expected 'proot not found' error, got:
$out"
log "  ✓ proot probe error fires before exec"

# ── S6: subos use after proot is staged → enters sandbox successfully
#       (needs network for proot fetch; gracefully skip if offline)
log "S6: subos use enters proot session (requires network for proot fetch)"
if curl -fsLo "$HOME_DIR/runtimedir/proot" \
     https://proot.gitlab.io/proot/bin/proot 2>/dev/null; then
  chmod +x "$HOME_DIR/runtimedir/proot"
  if [[ -x /bin/busybox ]] && file /bin/busybox 2>/dev/null | grep -q "statically linked"; then
    out_inside="$(echo 'echo "I_AM_$(whoami)_UID_$(id -u)"; echo "PASSWD_FIRST=$(head -1 /etc/passwd)"; ls /xlings 2>/dev/null | head -3 | tr "\n" " "; echo "<-- /xlings"; exit' | \
      ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
        timeout 10 "$XLINGS_BIN" subos use mybox ) 2>&1 || true)"
    echo "$out_inside" | grep -q "I_AM_root_UID_0" \
      || fail "S6: sandbox didn't fake root (expected I_AM_root_UID_0):
$out_inside"
    echo "$out_inside" | grep -q "PASSWD_FIRST=root:x:0:0:root:/root:/bin/sh" \
      || fail "S6: /etc/passwd inside sandbox is not our template:
$out_inside"
    log "  ✓ inside sandbox: whoami=root, /etc/passwd matches sandbox template"
  else
    log "  (skip: no static /bin/busybox to invoke as sandbox shell)"
  fi
else
  log "  (skip: cannot fetch proot from gitlab.io; offline or restricted)"
fi

# ── S7: self-heal — manually break the symlink, next `subos use` rehydrates
log "S7: subos use self-heals when shell symlink is missing"
rm -f "$shell_link"
[[ ! -e "$shell_link" ]] || fail "S7: setup failed — shell still present"
# Trigger use; it'll fail on proot probe (or shell exec), but the
# rehydrate path runs BEFORE proot exec and should restore the symlink.
run_x subos use mybox >/dev/null 2>&1 || true
[[ -L "$shell_link" ]] || fail "S7: symlink not restored after self-heal"
[[ -f "$shell_link" ]] || fail "S7: symlink still dangling after self-heal"
log "  ✓ rehydrate restored shell symlink without recreating subos"

# ── S8: subos remove sandbox cleans up everything
log "S8: subos remove sandbox removes the directory tree"
run_x subos remove mybox >/dev/null 2>&1 || true
[[ ! -d "$HOME_DIR/subos/mybox" ]] || fail "S8: subos dir not removed"
log "  ✓ sandbox subos removed cleanly"

log "PASS: subos sandbox (Linux) — 8 scenarios"

else
# ─────────────────────────────────────────────────────────────────────
# macOS / Windows: --sandbox-shell must be rejected
# ─────────────────────────────────────────────────────────────────────
log "non-Linux: --sandbox-shell rejected with clear hint"
out="$(run_x subos new mybox --sandbox-shell xim:sh 2>&1 || true)"
if ! echo "$out" | grep -q "only supported on Linux"; then
  fail "expected 'only supported on Linux' error on $(uname -s), got:
$out"
fi
[[ ! -d "$HOME_DIR/subos/mybox" ]] \
  || fail "subos dir created despite the rejection"
log "  ✓ rejected, no subos created"
log "PASS: subos sandbox (non-Linux) — rejection path"
fi
