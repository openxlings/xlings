#!/usr/bin/env bash
# install_use_semantics_test.sh — what "no version given" means, on both sides
# of the install/use split.
#
# Two defects of the same family, both of which reported success while handing
# back something the user did not ask for:
#
#   * a dependency written `xim:pkg` (no `@`) resolved to the index's NEWEST
#     version, so installing a small tool silently replaced the toolchain
#     underneath it. "No constraint" is not "give me the latest" -- it is
#     "any version will do", and one already was.
#
#   * `xlings use <pkg>` in a subos that never installed <pkg> opted the subos
#     in silently. That works for a self-contained package and not for one
#     with dependencies: the versions DB records no dependency information, so
#     activating gcc activated exactly gcc, leaving a toolchain that ran,
#     printed the right sysroot, and could not compile.
#
# Every case here is differential: it fails on 2026.7.31.2.
#
# Refs: .agents/docs/2026-07-31-install-use-semantics-plan.md §P1 §P2
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/install_use_semantics"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() {
  chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

# Every run names its subos explicitly: this file is about per-subos state, and
# a case that silently landed in another subos would pass for the wrong reason.
RUN() {
  local subos="$1"; shift
  ( cd /tmp && timeout 120 env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS="$subos" \
      "$XLINGS_BIN" "$@" </dev/null )
}

rc_of() {
  local rc=0
  "$@" >/dev/null 2>&1 || rc=$?
  [[ $rc -eq 124 ]] && fail "command blocked until the timeout: $*"
  printf '%s\n' "$rc"
}

mkdir -p "$HOME_DIR/subos/default/bin" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/s"

# write_pkg <name> <deps-lua> <version...>
# Each version's payload prints its own version, so "which one is active" is
# answered by running it rather than by reading the state file that the code
# under test writes.
write_pkg() {
  local name="$1" deps="$2"; shift 2
  local versions=("$@")
  {
    printf 'package = {\n'
    printf '    spec = "1", name = "%s",\n' "$name"
    printf '    description = "install/use semantics fixture",\n'
    printf '    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",\n'
    printf '    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},\n'
    printf '    xpm = {\n'
    # `deps` is a sibling of the version entries, not a member of one --
    # putting it inside a version silently declares no dependency at all, and
    # every assertion downstream then passes for the wrong reason.
    for os in linux macosx windows; do
      printf '        %s = { deps = {%s}, ' "$os" "$deps"
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
                 "#!/bin/sh\necho \"" .. pkginfo.name() .. " " .. pkginfo.version() .. "\"\n")
    return true
end
function config()
    xvm.add(pkginfo.name(), { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall() xvm.remove(pkginfo.name()) return true end
LUA
  } > "$LOCAL_INDEX_DIR/pkgs/s/$name.lua"
}

write_pkg sem-lib      ''                     1.0.0 1.5.0 2.0.0
write_pkg sem-bare     '"xim:sem-lib"'        1.0.0
write_pkg sem-range    '"xim:sem-lib@1"'      1.0.0
write_pkg sem-exact    '"xim:sem-lib@2.0.0"'  1.0.0

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN default self init >/dev/null 2>&1 || fail "self init failed"

chmod_payloads() {
  find "$HOME_DIR/data/xpkgs" -type f -name 'sem-*' -exec chmod +x {} + 2>/dev/null || true
}

# What the shim actually runs, in a named subos.
says() {
  local subos="$1" name="$2"
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      XLINGS_ACTIVE_SUBOS="$subos" "$HOME_DIR/subos/$subos/bin/$name" 2>&1 || true )
}

payload_exists() {
  [[ -d "$HOME_DIR/data/xpkgs/xim-x-sem-lib/$1" ]]
}

# ── S1: an unpinned dependency takes what is already active ──────────
#
# The reported case: `mcpp-short-cmd` declares `xim:mcpp` with no version, the
# machine already had mcpp, and the plan proposed replacing it.
log "S1: deps without a version use the active version, not the newest"
RUN default install sem-lib@1.0.0 -y >/dev/null 2>&1 || fail "S1: install sem-lib@1.0.0 failed"
chmod_payloads
grep -q "sem-lib 1.0.0" <<<"$(says default sem-lib)" \
  || fail "S1: setup did not leave sem-lib 1.0.0 active"

out="$(RUN default install sem-bare -y 2>&1 || true)"
chmod_payloads
if grep -q "sem-lib@2.0.0" <<<"$out"; then
  fail "S1: an unpinned dep planned the newest version; got:\n$out"
fi
if payload_exists 2.0.0; then
  fail "S1: sem-lib 2.0.0 was downloaded for an unpinned dep"
fi
grep -q "sem-lib 1.0.0" <<<"$(says default sem-lib)" \
  || fail "S1: installing sem-bare moved sem-lib off the active version"
grep -q "sem-bare 1.0.0" <<<"$(says default sem-bare)" \
  || fail "S1: sem-bare itself was not installed"

# ── S2: a range constraint is a range, not a literal version ─────────
#
# `@1` used to be handed to the loader verbatim as a version string on the
# dependency path, so it could only ever mean "unknown version: 1".
log "S2: a dep pinned to a range is satisfied by the active version"
out="$(RUN default install sem-range -y 2>&1 || true)"
chmod_payloads
if grep -qi "unknown version" <<<"$out"; then
  fail "S2: a range dep was treated as a literal version; got:\n$out"
fi
if payload_exists 1.5.0; then
  fail "S2: a satisfied range dep still installed another version"
fi
grep -q "sem-lib 1.0.0" <<<"$(says default sem-lib)" \
  || fail "S2: a satisfied range dep moved the active version"

# ── S3: an exact pin is still an exact pin ──────────────────────────
#
# The other half of the contract. Preferring what is installed must never
# override a version the dependency actually named.
log "S3: an exact dep pin installs that version even though another is active"
RUN default install sem-exact -y >/dev/null 2>&1 || fail "S3: install sem-exact failed"
chmod_payloads
payload_exists 2.0.0 || fail "S3: an exact dep pin (@2.0.0) did not install 2.0.0"

# ── S4: with nothing active, a range picks the highest that fits ─────
log "S4: an unsatisfied range resolves through the index, highest match wins"
RUN default subos new ranged >/dev/null 2>&1 || fail "S4: subos new failed"
RUN ranged install sem-range -y >/dev/null 2>&1 || fail "S4: install sem-range failed"
chmod_payloads
# 1.5.0 is the highest version inside `1`; 2.0.0 is outside it and is on disk
# already from S3, so picking it would be indistinguishable from "did nothing"
# without this assertion.
got="$(says ranged sem-lib)"
grep -q "sem-lib 1.5.0" <<<"$got" \
  || fail "S4: expected the highest version satisfying '1' (1.5.0); got: $got"

# ── S5: `use` does not half-install into a fresh subos ───────────────
log "S5: use refuses in a subos that never installed the package"
RUN default subos new probe >/dev/null 2>&1 || fail "S5: subos new failed"
rc="$(rc_of RUN probe use sem-lib 1.0.0)"
[[ "$rc" != "0" ]] || fail "S5: use switched to a version this subos never installed"
out="$(RUN probe use sem-lib 1.0.0 2>&1 || true)"
grep -q "install" <<<"$out" \
  || fail "S5: the refusal did not name the command that would work; got:\n$out"
grep -q "probe" <<<"$out" \
  || fail "S5: the refusal did not name the subos; got:\n$out"
# Nothing may have been written: no shim, and no entry in this subos's state.
[[ ! -e "$HOME_DIR/subos/probe/bin/sem-lib" ]] \
  || fail "S5: a refused use still created a shim"
if [[ -f "$HOME_DIR/subos/probe/.xlings.json" ]]; then
  if grep -q "sem-lib" "$HOME_DIR/subos/probe/.xlings.json"; then
    fail "S5: a refused use still recorded the package in the subos workspace"
  fi
fi

# ── S6: install in that same subos makes use work ───────────────────
#
# The refusal is only correct if the command it points at is the one that
# works. This is the differential's other half: same subos, same version.
log "S6: install here, then use here, works"
RUN probe install sem-lib@1.0.0 -y >/dev/null 2>&1 || fail "S6: install in probe failed"
chmod_payloads
rc="$(rc_of RUN probe use sem-lib 1.0.0)"
[[ "$rc" == "0" ]] || fail "S6: use failed after install in the same subos, exit $rc"
grep -q "sem-lib 1.0.0" <<<"$(says probe sem-lib)" \
  || fail "S6: the switch did not take effect"

# The default subos must be untouched by all of the above.
grep -q "sem-lib 1.0.0" <<<"$(says default sem-lib)" \
  || fail "S6: work in probe/ranged changed the default subos"

log "PASS: install_use_semantics"
