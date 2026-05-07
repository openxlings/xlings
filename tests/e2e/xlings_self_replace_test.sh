#!/usr/bin/env bash
# E2E: `xim install` / `xvm use` of xlings itself must atomically replace the
# bootstrap binary at $XLINGS_HOME/bin/xlings, so the active version actually
# takes effect when invoked through PATH.
#
# Background: main.cpp's multiplexer short-circuits xlings/xim/xvm names to
# cli::run() without consulting the workspace, so updating workspace[xlings]
# alone has no observable effect — the bootstrap file itself must change.
#
# Scenarios (uses a fake xpkg named "xlings" that drops a printable script
# at <bindir>/xlings; we rely only on the file-replacement contract):
#
#   1. fresh subos, install xlings@1.0.0   → bootstrap replaced (no prior active)
#   2. install xlings@2.0.0 (1.0.0 active) → bootstrap NOT replaced (preserve)
#   3. install xlings@2.0.0 --use          → bootstrap replaced
#   4. xvm use xlings 1.0.0                → bootstrap replaced (back to 1.0.0)

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/xlings_self_replace"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

FIXTURE_PKG="$LOCAL_INDEX_DIR/pkgs/x/xlings.lua"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

# Read the marker line from a "fake xlings" script: each version drops
# `BOOTSTRAP_VERSION=<version>` as the second line. We hash against this
# rather than trying to exec the script (avoids worrying about /bin/sh
# semantics inside a sandbox).
bootstrap_version() {
  local f="$HOME_DIR/bin/xlings"
  [[ -f "$f" ]] || { echo "<missing>"; return; }
  grep -oE 'BOOTSTRAP_VERSION=[0-9.]+' "$f" 2>/dev/null | head -1 \
    | cut -d= -f2 || echo "<unparseable>"
}

active_version() {
  python3 - "$HOME_DIR" <<'PY'
import json, sys, pathlib
home = sys.argv[1]
ws = json.loads(pathlib.Path(home, "subos/default/.xlings.json").read_text())
entry = (ws.get("workspace") or {}).get("xlings")
# 0.4.19+: workspace value is {active, installed[]} dict; pre-0.4.19 was a
# bare string. Tolerate both.
if isinstance(entry, dict):
    print(entry.get("active", "<none>"))
elif isinstance(entry, str):
    print(entry)
else:
    print("<none>")
PY
}

mkdir -p "$HOME_DIR"

# Private copy of the shared fixture index, neutralise sub-index repos.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$(dirname "$FIXTURE_PKG")"

# Override pkgs/x/xlings.lua with a fixture that:
#   - declares two versions (1.0.0, 2.0.0), no URL (no downloads)
#   - install hook drops a printable script at <install_dir>/bin/xlings
#     containing a parseable BOOTSTRAP_VERSION=<ver> marker
#   - config hook registers via xvm.add("xlings", { bindir = ... })
cat > "$FIXTURE_PKG" <<'LUA'
package = {
    spec = "1",
    name = "xlings",
    description = "Local fixture for tests/e2e/xlings_self_replace_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
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
    local fake = "#!/bin/sh\n# BOOTSTRAP_VERSION=" .. pkginfo.version() .. "\n"
                .. "echo 'fake xlings " .. pkginfo.version() .. "'\n"
    io.writefile(path.join(bindir, "xlings"), fake)
    return true
end

function config()
    xvm.add("xlings", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("xlings")
    return true
end
LUA

# Seed XLINGS_HOME with a sentinel "old bootstrap" file so we can detect
# replacement (not the real xlings binary — we only care about file content).
mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/bin"
printf '#!/bin/sh\n# BOOTSTRAP_VERSION=0.0.0\necho original\n' > "$HOME_DIR/bin/xlings"
chmod +x "$HOME_DIR/bin/xlings"

# Standard config — point at our private (neutralised) index.
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

# self init writes a real xlings binary into $HOME_DIR/bin/xlings, clobbering
# our sentinel. Restore the sentinel so scenario 1 has a known starting
# point ("bootstrap = 0.0.0").
printf '#!/bin/sh\n# BOOTSTRAP_VERSION=0.0.0\necho original\n' > "$HOME_DIR/bin/xlings"
chmod +x "$HOME_DIR/bin/xlings"
[[ "$(bootstrap_version)" == "0.0.0" ]] || fail "setup: bootstrap should be 0.0.0; got $(bootstrap_version)"

# ── Scenario 1: fresh install (no prior active) → bootstrap replaced to 1.0.0
log "S1: install xlings@1.0.0 (fresh, no active) → bootstrap replaced"
RUN install xlings@1.0.0 -y >/dev/null 2>&1 || fail "S1: install failed"
[[ "$(active_version)" == "1.0.0" ]] || fail "S1: active should be 1.0.0; got $(active_version)"
got=$(bootstrap_version)
[[ "$got" == "1.0.0" ]] || fail "S1: bootstrap should be 1.0.0 (replaced); got $got"

# ── Scenario 2: install xlings@2.0.0 with 1.0.0 active → bootstrap PRESERVED
log "S2: install xlings@2.0.0 (1.0.0 active, no --use) → bootstrap preserved"
RUN install xlings@2.0.0 -y >/dev/null 2>&1 || fail "S2: install failed"
[[ "$(active_version)" == "1.0.0" ]] || fail "S2: active should still be 1.0.0; got $(active_version)"
got=$(bootstrap_version)
[[ "$got" == "1.0.0" ]] || fail "S2: bootstrap should still be 1.0.0 (preserved); got $got"

# ── Scenario 3: install xlings@2.0.0 --use → bootstrap replaced to 2.0.0
log "S3: install xlings@2.0.0 --use → bootstrap replaced"
RUN install xlings@2.0.0 --use -y >/dev/null 2>&1 || fail "S3: install --use failed"
[[ "$(active_version)" == "2.0.0" ]] || fail "S3: active should be 2.0.0; got $(active_version)"
got=$(bootstrap_version)
[[ "$got" == "2.0.0" ]] || fail "S3: bootstrap should be 2.0.0 (replaced via --use); got $got"

# ── Scenario 4: xvm use xlings 1.0.0 → bootstrap replaced back to 1.0.0
log "S4: xlings use xlings 1.0.0 → bootstrap replaced"
RUN use xlings 1.0.0 >/dev/null 2>&1 || fail "S4: use failed"
[[ "$(active_version)" == "1.0.0" ]] || fail "S4: active should be 1.0.0; got $(active_version)"
got=$(bootstrap_version)
[[ "$got" == "1.0.0" ]] || fail "S4: bootstrap should be 1.0.0 (synced via use); got $got"

log "PASS: xlings self-replace scenarios 1, 2, 3, 4"
