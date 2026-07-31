#!/usr/bin/env bash
# E2E: a package that is installed and has no active version.
#
# The measured break: `codex` failing with
# `/usr/bin/env: 'node': No such file or directory` on a home where node was
# installed and its payload intact. node's release registers `node`, `npm` and
# `npx`; a separately installed npm package held `npm`; the activation vote was
# "is ANY member name already active", so one contested name suppressed the
# whole release. `node` — which nothing else provides — came out installed and
# unreachable.
#
# Nothing reported it. `xlings list` reads installed[] and never looks at the
# active selection. `self doctor`'s shim checks walk the active workspace (a
# name with no active version is not in it) and binDir (there is no file). The
# payload check walks the versions DB and finds everything healthy, which it
# is: what is broken is the selection.
#
# Scenarios:
#   S1  a foreign provider holding one member name does not veto its siblings
#   S2  active pointer removed by hand   → doctor reports it, exit 1
#   S3  --fix activates and the command runs
#   S4  `list` marks it inactive
#   S5  a virtual anchor with no command is NOT reported
#   S6  a same-provider contest still stays inactive, and --fix does not
#       choose a release for the user

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/inactive_installed"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

# A shim, invoked the way a user's shell would: through the subos bin dir, with
# the sandbox home. Running it by absolute path with no XLINGS_HOME makes it
# dispatch against the real home, which is both wrong and a way for this test
# to pass or fail for reasons that have nothing to do with it.
RUNSHIM() {
  local name="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH="$BIN_DIR:/usr/bin:/bin" \
      XLINGS_HOME="$HOME_DIR" "$BIN_DIR/$name" "$@" )
}

mkdir -p "$HOME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/i"

# ── Fixtures ───────────────────────────────────────────────────────
#
# `solo-tool` registers the single name `shared-cmd`. `group-tool` registers a
# release of three: its own root plus `shared-cmd` and `only-here`. Installing
# group-tool after solo-tool reproduces the node/npm shape exactly — one
# contested name, two uncontested ones, two different providers.

cat > "$LOCAL_INDEX_DIR/pkgs/i/solo-tool.lua" <<'LUA'
package = {
    spec = "1",
    name = "solo-tool",
    description = "Local fixture: owns `shared-cmd` on its own",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    programs = {"shared-cmd"},
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
    io.writefile(path.join(bindir, "shared-cmd"), "#!/bin/sh\necho solo\n")
    if os.host() ~= "windows" then
        os.exec("chmod +x " .. path.join(bindir, "shared-cmd"))
    end
    return true
end

function config()
    xvm.add("shared-cmd", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("shared-cmd")
    return true
end
LUA

cat > "$LOCAL_INDEX_DIR/pkgs/i/group-tool.lua" <<'LUA'
package = {
    spec = "1",
    name = "group-tool",
    description = "Local fixture: a release of three names, one contested",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    programs = {"group-tool", "only-here"},
    xpm = {
        linux   = { ["2.0.0"] = {} },
        macosx  = { ["2.0.0"] = {} },
        windows = { ["2.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    for _, name in ipairs({"group-tool", "shared-cmd", "only-here"}) do
        io.writefile(path.join(bindir, name), "#!/bin/sh\necho " .. name .. "@2.0.0\n")
        if os.host() ~= "windows" then
            os.exec("chmod +x " .. path.join(bindir, name))
        end
    end
    return true
end

function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    xvm.add("group-tool", { bindir = bindir })
    -- `binding` is a "root@version" STRING (libxpkg xvm.lua). A table is
    -- accepted and ignored, which leaves every member its own singleton
    -- group -- the fixture then passes for the wrong reason.
    local root = "group-tool@" .. pkginfo.version()
    xvm.add("shared-cmd", { bindir = bindir, binding = root })
    xvm.add("only-here", { bindir = bindir, binding = root })
    return true
end

function uninstall()
    xvm.remove("group-tool")
    xvm.remove("shared-cmd")
    xvm.remove("only-here")
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

BIN_DIR="$HOME_DIR/subos/default/bin"

# ── S1: a foreign claim does not veto the siblings ─────────────────
log "S1: install solo-tool, then group-tool → uncontested members activate"
RUN install solo-tool@1.0.0 -y >/dev/null 2>&1 || fail "S1 setup: solo-tool install failed"
[[ -e "$BIN_DIR/shared-cmd" ]] || fail "S1 setup: shared-cmd shim should exist"

RUN install group-tool@2.0.0 -y >/dev/null 2>&1 || fail "S1 setup: group-tool install failed"

# The contested name stays with the package that already owned it.
out=$(RUNSHIM shared-cmd 2>&1 || true)
echo "$out" | grep -q "solo" \
  || fail "S1: installing group-tool took 'shared-cmd' from solo-tool; got:\n$out"

# ...and the names nobody contests are usable. This is the whole defect:
# before the fix these two came out installed with no active version and no
# shim, because one sibling's name was spoken for.
[[ -e "$BIN_DIR/only-here" ]] \
  || fail "S1: 'only-here' is uncontested and must be activated — it had no shim, which is the node/npx break"
[[ -e "$BIN_DIR/group-tool" ]] \
  || fail "S1: 'group-tool' is uncontested and must be activated"
RUNSHIM only-here >/dev/null 2>&1 \
  || fail "S1: the shim exists but does not run"

RUN self doctor >/dev/null 2>&1 \
  || fail "S1: a home where every installed program is reachable should be clean"

# ── S2: an active pointer removed by hand is reported ──────────────
#
# What `remove` leaves behind when it takes out the active version and finds no
# coherent replacement (removal.cppm), and what registration used to produce on
# every contested install.
log "S2: drop the active pointer → doctor reports it, exit 1"
python3 - "$HOME_DIR" <<'PY' || fail "S2 setup: could not clear the active pointer"
import json, pathlib, sys
p = pathlib.Path(sys.argv[1], "subos", "default", ".xlings.json")
data = json.loads(p.read_text())
ws = data["workspace"]
for name in ("group-tool", "only-here"):
    entry = ws[name]
    assert "active" in entry, f"S2 setup: {name} should have been active"
    assert entry.get("installed"), f"S2 setup: {name} should be installed"
    del entry["active"]
p.write_text(json.dumps(data, indent=2))
PY
rm -f "$BIN_DIR/group-tool" "$BIN_DIR/only-here"

rc=0
out=$(RUN self doctor 2>&1) || rc=$?
echo "$out" | grep -q "no active version" \
  || fail "S2: installed-and-inactive must be reported; got:\n$out"
echo "$out" | grep -q "group-tool@2.0.0" \
  || fail "S2: the finding should name the release, not just a program; got:\n$out"
[[ $rc -eq 1 ]] || fail "S2: an unreachable installed package is an error; expected exit 1, got $rc"

# The repair is `use`, which moves the whole release — including `shared-cmd`,
# which solo-tool currently owns. The finding has to say so BEFORE --fix runs.
echo "$out" | grep -q "also moves shared-cmd" \
  || fail "S2: activating this release will move a name another package owns; the finding must disclose it; got:\n$out"

# One line per RELEASE. Fifty programs across three releases produced fifty
# identical-looking lines before this was grouped, which is how the one that
# mattered got buried.
count=$(echo "$out" | grep -c "✗ no active version" || true)
[[ "$count" -eq 1 ]] \
  || fail "S2: the release's programs must collapse onto ONE finding, got $count:\n$out"

# ── S4: `list` says so too ─────────────────────────────────────────
#
# Checked before the repair, while the state is still broken. This is the only
# one of the three silent surfaces a user looks at on purpose.
log "S4: list marks the inactive package"
out=$(RUN list 2>&1) || true
echo "$out" | grep -q "inactive" \
  || fail "S4: list rendered an unreachable package exactly like a working one; got:\n$out"

# ── S3: --fix activates it ─────────────────────────────────────────
log "S3: doctor --fix activates the release and the command runs"
RUN self doctor --fix >/dev/null 2>&1 || true
[[ -e "$BIN_DIR/only-here" ]] || fail "S3: --fix did not recreate the shim"
RUNSHIM only-here >/dev/null 2>&1 || fail "S3: the shim exists but does not run"
RUN self doctor >/dev/null 2>&1 || fail "S3: doctor should be clean after --fix"

# Activating a release moves the whole release, including a name another
# package held. That is what `use` means and it is what the remedy does when a
# user runs it by hand, so `--fix` doing it is not the problem -- doing it
# WITHOUT SAYING SO would be, which is the shape this entire check exists to
# end. S2 already asserted the disclosure; here we pin the consequence.
out=$(RUNSHIM shared-cmd 2>&1 || true)
echo "$out" | grep -q "shared-cmd@2.0.0" \
  || fail "S3: the release was activated but its contested member did not move with it; got:\n$out"

# ── S5: a virtual anchor is not a missing command ──────────────────
#
# A release anchor has no program behind it, so it has no active version to
# lack. Reporting one would send `--fix` to activate it, which writes a shim
# that can only print "no active version" — issue #452, reopened from the
# other side.
log "S5: a group-kind entry with no command is not reported"
python3 - "$HOME_DIR" <<'PY' || fail "S5 setup: could not add the anchor"
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
state = home / ".xlings.json"
data = json.loads(state.read_text())
payload = (data["versions"]["group-tool"]["versions"]["2.0.0"])["path"]
data["versions"]["anchor-only"] = {
    "filename": "anchor-only",
    "type": "program",
    "versions": {"9.9.9": {"kind": "group", "path": payload}},
}
state.write_text(json.dumps(data, indent=2))

sub = home / "subos" / "default" / ".xlings.json"
subdata = json.loads(sub.read_text())
subdata["workspace"]["anchor-only"] = {"installed": ["9.9.9"]}
sub.write_text(json.dumps(subdata, indent=2))
PY
rc=0
out=$(RUN self doctor 2>&1) || rc=$?
echo "$out" | grep -q "anchor-only" \
  && fail "S5: a release anchor has no command to activate; got:\n$out"
[[ $rc -eq 0 ]] || fail "S5: nothing is wrong here; expected exit 0, got $rc"

# ── S6: a same-provider contest is the user's call ─────────────────
#
# Two releases of ONE package is the mixed-toolchain hazard the binding-group
# model exists to prevent, and choosing between them is `use`. Install must not
# move part of the workspace, and `--fix` must not pick either.
log "S6: reinstalling a second release of the same package changes nothing"
python3 - "$LOCAL_INDEX_DIR" <<'PY' || fail "S6 setup: could not add the second release"
import pathlib, sys, re
p = pathlib.Path(sys.argv[1], "pkgs", "i", "group-tool.lua")
text = p.read_text()
text = text.replace('["2.0.0"] = {}', '["2.0.0"] = {}, ["3.0.0"] = {}')
p.write_text(text)
PY
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
RUN install group-tool@3.0.0 -y >/dev/null 2>&1 || fail "S6 setup: install failed"

active=$(python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1], "subos", "default", ".xlings.json")
print(json.loads(p.read_text())["workspace"]["group-tool"].get("active", ""))
PY
)
[[ "$active" == "2.0.0" ]] \
  || fail "S6: install moved the workspace to a new release of the same package (active=$active)"

log "all scenarios passed"
