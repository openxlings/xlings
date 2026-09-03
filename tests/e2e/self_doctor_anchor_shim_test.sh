#!/usr/bin/env bash
# E2E: install must not write a shim for a name it deliberately left inactive,
# and the loop that produced must not come back. Issue #452.
#
# The setup reproduces the real one (gcc + musl-gcc's gcc-flavor group):
#
#   anchor-base    registers program `shared-tool` → active
#   anchor-flavor  registers  · `anchor-flavor`             (its own entry point)
#                             · `xim-anchor-root@1.0-flavor` (virtual binding root)
#                             · `shared-tool@1.0-flavor`     bound to that root
#
# Installing anchor-flavor second means its second group already has an active
# member (`shared-tool`, owned by anchor-base), so registration withholds
# activation from that whole release — including the root, which nobody else
# contests. Before this fix the ProgramShim effect wrote a shim for the root
# anyway, and `self doctor` called the result an `orphan shim` error.
#
# Scenarios:
#   1. install leaves the inactive root with no shim
#   2. doctor is clean, exit 0
#   3. an existing home's leftover anchor shim is a notice, not an error
#   4. --fix removes it
#   5. CONVERGENCE: the install after --fix does not bring it back
#
# S5 is the point. Every earlier fix in this area made one --fix converge;
# none checked that the next install stayed clean.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/self_doctor_anchor_shim"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR"

cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/a"

cat > "$LOCAL_INDEX_DIR/pkgs/a/anchor-base.lua" <<'LUA'
package = {
    spec = "1",
    name = "anchor-base",
    description = "Owns `shared-tool` so the flavor release loses the activation vote",
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
    io.writefile(path.join(bindir, "shared-tool"), "#!/bin/sh\necho base\n")
    return true
end

function config()
    xvm.add("shared-tool", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("shared-tool")
    return true
end
LUA

cat > "$LOCAL_INDEX_DIR/pkgs/a/anchor-flavor.lua" <<'LUA'
package = {
    spec = "1",
    name = "anchor-flavor",
    description = "Publishes a second `shared-tool` flavor under a virtual root",
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
    io.writefile(path.join(bindir, "anchor-flavor"), "#!/bin/sh\necho flavor\n")
    io.writefile(path.join(bindir, "shared-tool"), "#!/bin/sh\necho flavor\n")
    return true
end

function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    -- The package's own entry point: its own singleton group, activated
    -- normally because nothing else owns the name.
    xvm.add("anchor-flavor", { bindir = bindir })

    -- The flavor subtree. `xim-anchor-root` is a pure anchor: no bindir,
    -- no alias, nothing to dispatch to. Deliberately registered without
    -- `type = "group"` — that is the shape the bug needs, and the shape
    -- three real recipes had when #452 was filed.
    xvm.add("xim-anchor-root", { version = "1.0.0-flavor" })
    xvm.add("shared-tool", {
        bindir  = bindir,
        version = "1.0.0-flavor",
        binding = "xim-anchor-root@1.0.0-flavor",
    })
    return true
end

function uninstall()
    xvm.remove("anchor-flavor")
    xvm.remove("shared-tool", "1.0.0-flavor")
    xvm.remove("xim-anchor-root", "1.0.0-flavor")
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
ROOT_SHIM="$BIN_DIR/xim-anchor-root"
WS="$HOME_DIR/subos/default/.xlings.json"

active_version_of() {
  python3 - "$WS" "$1" <<'PY'
import json, sys, pathlib
ws = json.loads(pathlib.Path(sys.argv[1]).read_text()).get("workspace") or {}
print((ws.get(sys.argv[2]) or {}).get("active", ""))
PY
}

installed_versions_of() {
  python3 - "$WS" "$1" <<'PY'
import json, sys, pathlib
ws = json.loads(pathlib.Path(sys.argv[1]).read_text()).get("workspace") or {}
print(",".join((ws.get(sys.argv[2]) or {}).get("installed", [])))
PY
}

RUN install anchor-base@1.0.0 -y >/dev/null 2>&1 \
  || fail "setup: install anchor-base failed"
[[ -e "$BIN_DIR/shared-tool" ]] || fail "setup: shared-tool shim should exist"
[[ "$(active_version_of shared-tool)" == "1.0.0" ]] \
  || fail "setup: anchor-base should own shared-tool"

# ── S1: the inactive root gets no shim ─────────────────────────────
log "S1: install leaves an inactive binding root without a shim"
RUN install anchor-flavor@1.0.0 -y >/dev/null 2>&1 \
  || fail "S1: install anchor-flavor failed"

# Precondition — without this the test proves nothing: the flavor release
# must actually have lost the activation vote.
[[ "$(active_version_of xim-anchor-root)" == "" ]] \
  || fail "S1 precondition: the root was activated, so there is no bug to catch"
[[ "$(installed_versions_of xim-anchor-root)" == "1.0.0-flavor" ]] \
  || fail "S1 precondition: the root was not registered at all"
[[ "$(active_version_of shared-tool)" == "1.0.0" ]] \
  || fail "S1 precondition: the flavor stole shared-tool from anchor-base"

[[ ! -e "$ROOT_SHIM" ]] \
  || fail "S1: install wrote a shim for a name with no active version"

# ── S2: doctor is clean ────────────────────────────────────────────
log "S2: doctor after the install → exit 0, no orphan"
out=$(RUN self doctor 2>&1) || fail "S2: doctor should exit 0; got:\n$out"
echo "$out" | grep -q "orphan shim" \
  && fail "S2: doctor reported an orphan shim; got:\n$out"

# ── S3: a leftover shim from an older client is a notice ───────────
log "S3: pre-existing anchor shim → notice, not an error"
ln -sf "$HOME_DIR/xlings" "$ROOT_SHIM"
[[ -e "$ROOT_SHIM" ]] || fail "S3 setup: could not plant the shim"

# 2026.9.3.1: the per-name `orphan shim` / `anchor shim` findings became one
# per-subos `shim table` finding compared against the table the workspace
# implies. The JUDGEMENT this scenario pins is unchanged and is the reason the
# new finding is levelled by direction: a stale entry is a file that resolved
# to nothing before and after, and on an existing home it is there through no
# act of the user's, so it must not reach the exit code. Only a MISSING entry
# -- a program that is active and unreachable -- is an error.
out=$(RUN self doctor 2>&1) || fail "S3: a stale shim must not fail the run; got:\n$out"
echo "$out" | grep -q "orphan shim" \
  && fail "S3: a stale shim was reported as an orphan error; got:\n$out"

out=$(RUN self doctor --all 2>&1) || fail "S3: doctor --all should exit 0; got:\n$out"
echo "$out" | grep -q "shim table" \
  || fail "S3: --all should name the stale table entry; got:\n$out"

# A real orphan — a program nothing anchors — must still be an error.
# anchor-flavor's shim is already there; dropping its workspace entry is
# what makes it an orphan.
[[ -e "$BIN_DIR/anchor-flavor" ]] || fail "S3 setup: anchor-flavor shim missing"
python3 - "$WS" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
data = json.loads(p.read_text())
ws = data.get("workspace") or {}
ws.pop("anchor-flavor", None)
data["workspace"] = ws
p.write_text(json.dumps(data))
PY
# A shim for a name this subos has no active version of is now the same
# finding as any other stale entry, and carries the same judgement: `--fix`
# removes it, and it does not fail the run on its own. What DOES fail the run
# is the other direction -- an active program with no file -- which S1/S2
# already cover. So this half now asserts the report, not the exit code.
out=$(RUN self doctor --all 2>&1) || true
echo "$out" | grep -q "shim table" \
  || fail "S3: a stale shim must still be reported; got:\n$out"

# ── S4: --fix removes the anchor shim ──────────────────────────────
log "S4: doctor --fix removes both"
RUN self doctor --fix >/dev/null 2>&1 || fail "S4: doctor --fix should succeed"
[[ ! -e "$ROOT_SHIM" ]] || fail "S4: --fix left the anchor shim behind"

# ── S5: convergence ────────────────────────────────────────────────
log "S5: the install after --fix does not bring it back"
RUN remove anchor-flavor@1.0.0 -y >/dev/null 2>&1 \
  || fail "S5: remove anchor-flavor failed"
RUN install anchor-flavor@1.0.0 -y >/dev/null 2>&1 \
  || fail "S5: re-install anchor-flavor failed"

[[ ! -e "$ROOT_SHIM" ]] \
  || fail "S5: the re-install recreated the anchor shim — the loop is still open"
out=$(RUN self doctor 2>&1) || fail "S5: doctor should be clean after re-install; got:\n$out"
echo "$out" | grep -qE "orphan shim|shim table" \
  && fail "S5: doctor reported table drift after re-install; got:\n$out"

log "PASS: an inactive binding root gets no shim, and the repair converges"
