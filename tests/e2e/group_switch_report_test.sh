#!/usr/bin/env bash
# group_switch_report_test.sh — switching a release must say what it did not
# cover.
#
# `xlings use` writes the members of the release it switches TO and nothing
# else. A program the incoming release has no version of is therefore left
# exactly where it was: still active, still resolving into the release the
# user just left. The switch is correct; the problem is that the entire output
# was one line about the entry target.
#
# Measured on a real home: `xlings use llvm 20.1.7` printed
# `[xlings] llvm -> 20.1.7`, and `clang++` went on answering for a different
# release. The user found out from their compiler.
#
# Two releases of one package routinely register different program sets -- a
# tool added between versions, a platform-specific name, a payload whose
# members were registered under the wrong names -- so this is the normal case,
# not a corner one.
#
# Refs: .agents/docs/2026-07-30-cli-determinism-and-followup-plan.md §2.2
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/group_switch_report"
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
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
      "$XLINGS_BIN" "$@" </dev/null )
}

# The shim has to be told which home it belongs to, exactly like the CLI --
# without XLINGS_HOME it resolves against the caller's real home and reports
# "not installed" for a package this sandbox installed perfectly.
RUN_SHIM() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
      "$HOME_DIR/subos/default/bin/$1" 2>&1 || true )
}

mkdir -p "$HOME_DIR/subos/default/bin" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/g"

# 1.0.0 registers gs-a and gs-b; 2.0.0 registers only gs-a. That asymmetry is
# the whole fixture.
cat > "$LOCAL_INDEX_DIR/pkgs/g/gs-probe.lua" <<'LUA'
package = {
    spec = "1", name = "gs-probe", description = "group switch fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
local function members()
    if pkginfo.version() == "1.0.0" then return {"gs-a", "gs-b"} end
    return {"gs-a"}
end
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    for _, n in ipairs(members()) do
        io.writefile(path.join(bindir, n),
            "#!/bin/sh\necho \"" .. n .. " " .. pkginfo.version() .. "\"\n")
    end
    return true
end
function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    local binding = "gs-probe@" .. pkginfo.version()
    xvm.add("gs-probe")
    for _, n in ipairs(members()) do
        xvm.add(n, { bindir = bindir, binding = binding })
    end
    return true
end
function uninstall() xvm.remove("gs-probe") return true end
LUA

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN self init >/dev/null 2>&1 || fail "self init failed"
RUN install gs-probe@1.0.0 -y >/dev/null 2>&1 || fail "install 1.0.0 failed"
RUN install gs-probe@2.0.0 -y >/dev/null 2>&1 || fail "install 2.0.0 failed"
# io.writefile leaves the payload non-executable; this is a fixture, not a
# real package, so the bit is set here rather than faked in Lua.
find "$HOME_DIR/data/xpkgs" -type f -name 'gs-*' -exec chmod +x {} +

RUN use gs-probe 1.0.0 >/dev/null 2>&1 || fail "use 1.0.0 failed"
out="$(RUN_SHIM gs-b)"
grep -q "gs-b 1.0.0" <<<"$out" || fail "precondition: gs-b does not run; got:\n$out"

# ── G1: --strict refuses, and changes nothing ────────────────────────
#
# Checked before the permissive switch, because after it the state has
# already moved and "nothing changed" is no longer falsifiable.
log "G1: --strict refuses a switch that would leave a program behind"
rc=0
strict_out="$(RUN use gs-probe 2.0.0 --strict 2>&1)" || rc=$?
[[ "$rc" != "0" ]] || fail "G1: --strict accepted a switch that strands gs-b"
# Refusing for the right reason: an unrecognised flag also exits non-zero.
grep -q "gs-b" <<<"$strict_out" \
  || fail "G1: the refusal does not name what would be left behind; got:\n$strict_out"
out="$(RUN_SHIM gs-a)"
grep -q "gs-a 1.0.0" <<<"$out" \
  || fail "G1: a refused switch moved gs-a anyway; got:\n$out"

# ── G2: the permissive switch names what it left behind ──────────────
log "G2: the switch reports the program the new release does not have"
out="$(RUN use gs-probe 2.0.0 2>&1 || fail "use 2.0.0 failed")"
grep -q -- "gs-probe -> 2.0.0" <<<"$out" || fail "G2: no switch line; got:\n$out"
grep -q "gs-b" <<<"$out" \
  || fail "G2: gs-b was left on the old release with no mention of it; got:\n$out"
grep -q "1.0.0" <<<"$out" \
  || fail "G2: the report does not say what gs-b still resolves to; got:\n$out"

# The report has to be true: gs-a moved, gs-b did not.
grep -q "gs-a 2.0.0" <<<"$(RUN_SHIM gs-a)" || fail "G2: gs-a did not switch"
grep -q "gs-b 1.0.0" <<<"$(RUN_SHIM gs-b)" \
  || fail "G2: gs-b was silently deactivated rather than left behind"

# ── G3: a switch that leaves nothing behind stays quiet ──────────────
#
# Otherwise the warning becomes noise and stops being read.
log "G3: a switch with nothing stranded says nothing extra"
out="$(RUN use gs-probe 1.0.0 2>&1 || fail "use back to 1.0.0 failed")"
if grep -qi "still resolve" <<<"$out"; then
  fail "G3: reported a stranded program on a switch that strands none; got:\n$out"
fi

log "PASS: group_switch_report"
