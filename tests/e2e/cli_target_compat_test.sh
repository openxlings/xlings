#!/usr/bin/env bash
# E2E: CLI target-spec parsing consistency across remove / update / use /
# info / install.
#
# Goal: every single-target command must accept BOTH equivalent forms:
#   <name>@<version>   (combined)
#   <name> <version>   (separated)
# Plus reject ambiguous (`name@v1 v2`) and too-many (`a b c`).
#
# Critical regression in scope: pre-fix, `xlings remove dpkg 1.0.0` with
# active=2.0.0 silently removed the active version (2.0.0) instead of
# the requested 1.0.0 — second positional was dropped. Post-fix, the
# requested version must be honored.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/cli_target_compat"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/d/dpkg.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

# Helper that runs RUN and captures stdout/stderr as $LAST_OUT.
# Note: the cmdline library's outer run() always returns 0 even when
# action returns non-zero, so we assert on observable side effects
# (state changes / printed error message) rather than the exit code.
run_capture() {
  LAST_OUT=$(RUN "$@" 2>&1) || true
}

mkdir -p "$HOME_DIR"

# Private fixture index — neutralise sub-index-repos.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

# Two-version package fixture — versions 1.0.0 and 2.0.0.
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "dpkg",
    description = "Local fixture for cli_target_compat_test.sh",
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "dpkg"),
                 "#!/bin/sh\necho dpkg@" .. pkginfo.version() .. "\n")
    return true
end

function config()
    xvm.add("dpkg", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("dpkg")
    return true
end
LUA

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF

log "Initializing sandbox XLINGS_HOME at $HOME_DIR"
RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

active_version() {
  python3 - "$HOME_DIR" <<'PY'
import json, sys, pathlib
ws = json.loads(pathlib.Path(sys.argv[1], "subos/default/.xlings.json").read_text())
entry = (ws.get("workspace") or {}).get("dpkg")
# 0.4.19+: workspace value is {active, installed[]} dict; tolerate both forms.
if isinstance(entry, dict):
    print(entry.get("active", "<none>"))
elif isinstance(entry, str):
    print(entry)
else:
    print("<none>")
PY
}

versions_in_db() {
  python3 - "$HOME_DIR" <<'PY'
import json, sys, pathlib
data = json.loads(pathlib.Path(sys.argv[1], ".xlings.json").read_text())
vers = sorted((data.get("versions") or {}).get("dpkg", {}).get("versions", {}))
print(",".join(vers))
PY
}

# ── Setup: install both versions, active = 2.0.0 ───────────────────
log "Setup: install dpkg@1.0.0 + dpkg@2.0.0, active = 2.0.0"
RUN install dpkg@1.0.0 -y >/dev/null 2>&1 || fail "setup: install 1.0.0 failed"
RUN install dpkg@2.0.0 --use -y >/dev/null 2>&1 || fail "setup: install 2.0.0 failed"
[[ "$(active_version)" == "2.0.0" ]] || fail "setup: active should be 2.0.0; got $(active_version)"
[[ "$(versions_in_db)" == "1.0.0,2.0.0" ]] || fail "setup: DB should have both versions; got $(versions_in_db)"

# ──────────── use ────────────

log "S1: use dpkg@1.0.0 (combined form)"
RUN use dpkg@1.0.0 >/dev/null 2>&1 || fail "S1: use combined failed"
[[ "$(active_version)" == "1.0.0" ]] || fail "S1: active should be 1.0.0; got $(active_version)"

log "S2: use dpkg 2.0.0 (separated form)"
RUN use dpkg 2.0.0 >/dev/null 2>&1 || fail "S2: use separated failed"
[[ "$(active_version)" == "2.0.0" ]] || fail "S2: active should be 2.0.0; got $(active_version)"

log "S3: use dpkg (bare → list versions, lists both versions)"
run_capture use dpkg
echo "$LAST_OUT" | grep -q "1.0.0" \
  || fail "S3: bare use should list 1.0.0; got:\n$LAST_OUT"

# S4/S5: cmdline lib drops action's return code, so assert on:
#   (a) error message printed, (b) state didn't change.
log "S4: use dpkg@1.0.0 1.0.0 (ambiguous → error printed, active unchanged)"
RUN use dpkg 2.0.0 >/dev/null 2>&1   # known starting state
run_capture use dpkg@1.0.0 1.0.0
echo "$LAST_OUT" | grep -q "ambiguous target" \
  || fail "S4: should mention 'ambiguous target'; got:\n$LAST_OUT"
[[ "$(active_version)" == "2.0.0" ]] \
  || fail "S4: active must remain 2.0.0 after rejected ambiguous; got $(active_version)"

log "S5: use a b c (too many → error printed, active unchanged)"
run_capture use a b c
echo "$LAST_OUT" | grep -q "too many positional" \
  || fail "S5: should mention 'too many positional'; got:\n$LAST_OUT"
[[ "$(active_version)" == "2.0.0" ]] \
  || fail "S5: active must remain 2.0.0 after rejected too-many; got $(active_version)"

# ──────────── remove ────────────

# Setup: ensure both versions installed, active = 2.0.0 (S2 left us at 2.0.0)
RUN install dpkg@1.0.0 -y >/dev/null 2>&1 || true
[[ "$(versions_in_db)" == "1.0.0,2.0.0" ]] || fail "remove setup: both versions in DB; got $(versions_in_db)"

log "S6: ★ remove dpkg 1.0.0 (separated, non-active) → 1.0.0 deleted, 2.0.0 kept"
log "    (regression check for the silent-drop bug fixed in this PR)"
RUN remove dpkg 1.0.0 -y >/dev/null 2>&1 || fail "S6: remove separated failed"
[[ "$(versions_in_db)" == "2.0.0" ]] \
  || fail "S6: only 2.0.0 should remain in DB; got '$(versions_in_db)'"
[[ "$(active_version)" == "2.0.0" ]] \
  || fail "S6: active must remain 2.0.0; got $(active_version)"

log "S7: remove dpkg@2.0.0 (combined, active) → both gone"
RUN remove dpkg@2.0.0 -y >/dev/null 2>&1 || fail "S7: remove combined failed"
[[ -z "$(versions_in_db)" ]] || fail "S7: DB should be empty for dpkg; got '$(versions_in_db)'"

log "S8: remove dpkg@1.0.0 1.0.0 (ambiguous → error printed, version preserved)"
RUN install dpkg@1.0.0 -y >/dev/null 2>&1 || true
run_capture remove dpkg@1.0.0 1.0.0 -y
echo "$LAST_OUT" | grep -q "ambiguous target" \
  || fail "S8: should mention 'ambiguous target'; got:\n$LAST_OUT"
[[ "$(versions_in_db)" == "1.0.0" ]] \
  || fail "S8: 1.0.0 must remain in DB after rejected ambiguous; got '$(versions_in_db)'"

log "S9: remove a b c (too many → error printed)"
run_capture remove a b c -y
echo "$LAST_OUT" | grep -q "too many positional" \
  || fail "S9: should mention 'too many positional'; got:\n$LAST_OUT"

# ──────────── info ────────────

log "S10: info dpkg@1.0.0 (combined form → prints package info)"
run_capture info dpkg@1.0.0
echo "$LAST_OUT" | grep -q "dpkg" \
  || fail "S10: info combined should print dpkg info; got:\n$LAST_OUT"

log "S11: info dpkg 1.0.0 (separated form → prints package info)"
run_capture info dpkg 1.0.0
echo "$LAST_OUT" | grep -q "dpkg" \
  || fail "S11: info separated should print dpkg info; got:\n$LAST_OUT"

log "S12: info dpkg@1.0.0 1.0.0 (ambiguous → error printed)"
run_capture info dpkg@1.0.0 1.0.0
echo "$LAST_OUT" | grep -q "ambiguous target" \
  || fail "S12: should mention 'ambiguous target'; got:\n$LAST_OUT"

# ──────────── update ────────────

# update is a network-touching command in real flow; with our offline
# fixture, it goes through the same parse path, so the parse-side
# assertions (ambiguous / too-many) are what we test here. Also test
# that bare `update` (no positional) is still valid (refresh-index-only).
log "S13: update (bare, no positional → refresh-index-only, prints 'index updated')"
run_capture update
echo "$LAST_OUT" | grep -q "index updated" \
  || fail "S13: bare update should print 'index updated'; got:\n$LAST_OUT"

log "S14: update dpkg@1.0.0 1.0.0 (ambiguous → error printed)"
run_capture update dpkg@1.0.0 1.0.0 -y
echo "$LAST_OUT" | grep -q "ambiguous target" \
  || fail "S14: should mention 'ambiguous target'; got:\n$LAST_OUT"

log "S15: update a b c (too many → error printed)"
run_capture update a b c -y
echo "$LAST_OUT" | grep -q "too many positional" \
  || fail "S15: should mention 'too many positional'; got:\n$LAST_OUT"

# ──────────── install (multi-package, friendly hint) ────────────

log "S16: install dpkg 2.0.0 → friendly hint about combined form"
run_capture install dpkg 2.0.0 -y
echo "$LAST_OUT" | grep -q "did you mean .*dpkg@2.0.0" \
  || fail "S16: install should print a friendly hint about combined form; got:\n$LAST_OUT"

log "PASS: cli target-spec compatibility — scenarios 1-16"
