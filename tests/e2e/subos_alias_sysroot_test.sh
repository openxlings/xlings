#!/usr/bin/env bash
# subos_alias_sysroot_test.sh — an alias that baked a subos path at install
# time must follow the ACTIVE subos at execution time.
#
# `gcc.lua` writes `g++ --sysroot=<system.subos_sysrootdir()>` into the xvm
# alias: the absolute path of whichever subos was active when gcc was
# installed. The versions DB is shared by the whole home and `subos use`
# rewrites nothing, so that path outlives every switch -- the user switches to
# `default` and their g++ keeps compiling against `dev`'s sysroot, which may
# not even exist any more. Nothing in doctor sees it either: the alias's
# PROGRAM resolves fine, only its --sysroot argument points somewhere stale.
#
# The fixture reproduces exactly that shape (bake at install, execute later
# from another subos) with a probe that prints the arguments it was handed.
#
# Refs: .agents/docs/2026-07-30-subos-selection-leak-and-foreign-payload-plan.md
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_alias_sysroot"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() {
  # The copied index carries read-only git objects; make them removable.
  chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

# Every command names the subos it runs in: this test is entirely about which
# subos a thing happens in, so there is no default form.
RUN_IN() {
  local subos="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS="$subos" \
      "$XLINGS_BIN" "$@" )
}

# Execute a shim (not the CLI) with a named active subos. `""` means "no
# XLINGS_ACTIVE_SUBOS at all", i.e. selection falls to the persisted field.
RUN_SHIM() {
  local subos="$1" shim="$2"; shift 2
  if [[ -n "$subos" ]]; then
    ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS="$subos" "$shim" "$@" )
  else
    ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$shim" "$@" )
  fi
}

mkdir -p "$HOME_DIR/subos/default/bin" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/s"

# sr-probe registers an alias onto a DIFFERENT real binary (sr-real) so the
# shim is not recursive, and bakes the install-time sysroot into it -- byte
# for byte what gcc.lua does.
cat > "$LOCAL_INDEX_DIR/pkgs/s/sr-probe.lua" <<'LUA'
package = {
    spec = "1", name = "sr-probe",
    description = "alias sysroot normalization fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.system")
import("xim.libxpkg.xvm")
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "sr-real"),
                 "#!/bin/sh\necho \"probe-args: $*\"\n")
    return true
end
function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    xvm.add("sr-probe", {
        bindir = bindir,
        alias  = "sr-real --sysroot=" .. system.subos_sysrootdir(),
    })
    return true
end
function uninstall()
    xvm.remove("sr-probe")
    return true
end
LUA

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN_IN default self init >/dev/null 2>&1 || fail "self init failed"
RUN_IN default subos new dev >/dev/null 2>&1 || fail "subos new dev failed"

# ── the real-world sequence ──────────────────────────────────────────
#
#   1. install in `default`      → alias bakes default, default's workspace
#                                  and shim exist
#   2. install in `dev`          → payload already there, but the CONFIG hook
#                                  re-runs and rewrites the SHARED alias to
#                                  dev; dev gets its own workspace + shim
#   3. work in `default` again   → default's shim runs, and used to hand the
#                                  compiler dev's sysroot
log "install sr-probe in 'default', then in 'dev'"
RUN_IN default install sr-probe >/dev/null 2>&1 || fail "install in default failed"
RUN_IN dev install sr-probe >/dev/null 2>&1 || fail "install in dev failed"

PAYLOAD_BIN="$(find "$HOME_DIR/data/xpkgs" -name sr-real -type f | head -1)"
[[ -n "$PAYLOAD_BIN" ]] || fail "payload not installed"
chmod +x "$PAYLOAD_BIN"

# The invariant, replacing the old precondition.
#
# This used to assert that the alias DID bake the install-time subos -- that
# was the shape of the bug. Registration now lifts the sysroot out of the
# alias into VData::sysroot, so the shared database holds the intent and no
# subos path at all. Both halves are asserted: a record with neither would
# also have no path, and would pass a one-sided check for the wrong reason.
grep -q '"sysroot": true' "$HOME_DIR/.xlings.json" \
  || fail "invariant: the registration did not record the sysroot intent"
if grep -q -- '--sysroot=' "$HOME_DIR/.xlings.json"; then
  fail "invariant: a subos path is still stored in the home-wide database"
fi

# ── A1: run from 'dev' — the baseline, must target dev ───────────────
log "A1: executing under 'dev' targets dev's sysroot"
out="$(RUN_SHIM dev "$HOME_DIR/subos/dev/bin/sr-probe" 2>&1 || true)"
grep -q "$HOME_DIR/subos/dev" <<<"$out" \
  || fail "A1: running in its own subos must target it; got:\n$out"

# ── A2: THE ASSERTION — the same shim under a different active subos ─
#
# Same DB entry, same baked alias, different active subos. Which shim FILE is
# invoked is irrelevant: dispatch resolves the program by name out of the
# home-wide DB, so this is the same code path a user hits after switching.
# Before the exec-time normalization this printed dev's sysroot regardless.
log "A2: executing under 'default' must target default's sysroot"
[[ -x "$HOME_DIR/subos/default/bin/sr-probe" ]] || fail "A2: no shim in default"
out="$(RUN_SHIM default "$HOME_DIR/subos/default/bin/sr-probe" 2>&1 || true)"
grep -q "$HOME_DIR/subos/default" <<<"$out" \
  || fail "A2: shim still targets the install-time subos; got:\n$out"
if grep -q "$HOME_DIR/subos/dev/" <<<"$out"; then
  fail "A2: the install-time subos leaked into execution; got:\n$out"
fi

# ── A2b: the same, selected by the persisted field instead of the env ─
#
# XLINGS_ACTIVE_SUBOS and the home config's activeSubos are two different
# selection modes and only one of them is set in A2. `subos use --global`
# exercises the other, with no env var in sight.
log "A2b: selection via the persisted activeSubos field"
RUN_IN default subos use --global default >/dev/null 2>&1 \
  || fail "subos use --global default failed"
out="$(RUN_SHIM "" "$HOME_DIR/subos/default/bin/sr-probe" 2>&1 || true)"
grep -q "$HOME_DIR/subos/default" <<<"$out" \
  || fail "A2b: persisted selection ignored; got:\n$out"

# ── A3: a LEGACY record, as an upgraded home actually has ────────────
#
# Everything above tested a fresh install. A home that installed gcc before
# this release has the old shape on disk -- the path inside the alias -- and
# nothing reinstalls it. That state is written here directly, because that is
# exactly how it arrives: not through any code path this binary still has.
log "A3: inject a pre-upgrade record"
LEGACY_PY="$RUNTIME_DIR/inject_legacy.py"
cat > "$LEGACY_PY" <<'INJECT'
import json, sys
path, home = sys.argv[1], sys.argv[2]
d = json.load(open(path))
e = d["versions"]["sr-probe"]["versions"]["1.0.0"]
e["alias"] = ["sr-real --sysroot=%s/subos/dev" % home]
e.pop("sysroot", None)
json.dump(d, open(path, "w"), indent=2)
INJECT
python3 "$LEGACY_PY" "$HOME_DIR/.xlings.json" "$HOME_DIR"
grep -q '/subos/dev' "$HOME_DIR/.xlings.json" \
  || fail "A3: failed to inject the legacy record"

# It still WORKS -- exec-time normalization is what keeps an un-migrated home
# correct, and that must hold whether or not anyone ever runs doctor.
out="$(RUN_SHIM default "$HOME_DIR/subos/default/bin/sr-probe" 2>&1 || true)"
grep -q "$HOME_DIR/subos/default" <<<"$out" \
  || fail "A3: a legacy record stopped following the active subos; got:\n$out"

# ── A4: doctor sees it ───────────────────────────────────────────────
#
# Until now this state was invisible to every diagnostic: the alias's PROGRAM
# resolves, so `alias unresolved` never fired, and nothing else looked at the
# arguments at all.
log "A4: doctor reports the baked path"
out="$(RUN_IN default self doctor 2>&1 || true)"
grep -q "subos path" <<<"$out" \
  || fail "A4: doctor is silent about the baked subos path; got:\n$out"

# It is a warning, not an error: execution is already correct.
RUN_IN default self doctor >/dev/null 2>&1 \
  || fail "A4: a baked path must not fail the doctor run"

# ── A5: --fix rewrites it, and is idempotent ─────────────────────────
log "A5: doctor --fix rewrites the record"
RUN_IN default self doctor --fix >/dev/null 2>&1 \
  || fail "A5: doctor --fix failed"
if grep -q -- '--sysroot=' "$HOME_DIR/.xlings.json"; then
  fail "A5: --fix left a subos path in the DB instead of migrating it"
fi
grep -q '"sysroot": true' "$HOME_DIR/.xlings.json" \
  || fail "A5: --fix dropped the path without recording the intent"
out="$(RUN_IN default self doctor 2>&1 || true)"
if grep -q "subos path" <<<"$out"; then
  fail "A5: the finding survived --fix; got:\n$out"
fi

# The probe must still work after the rewrite.
out="$(RUN_SHIM default "$HOME_DIR/subos/default/bin/sr-probe" 2>&1 || true)"
grep -q "$HOME_DIR/subos/default" <<<"$out" \
  || fail "A5: the rewritten alias no longer targets the active subos:\n$out"

# ── A6: a dangling sysroot link is seen and removed ──────────────────
#
# The measured shape: an isolated run materialized header links into the real
# home's subos and then deleted its own payload store, leaving links into a
# /tmp path that no longer exists. `[ -e ]` says they are gone; the compiler
# following them says otherwise.
log "A6: dangling sysroot links"
mkdir -p "$HOME_DIR/subos/default/usr/include"
ln -s "$RUNTIME_DIR/gone/linux" "$HOME_DIR/subos/default/usr/include/linux"
[[ -L "$HOME_DIR/subos/default/usr/include/linux" ]] \
  || fail "A6: fixture link not created"
if [[ -e "$HOME_DIR/subos/default/usr/include/linux" ]]; then
  fail "A6: fixture link must be dangling"
fi

out="$(RUN_IN default self doctor 2>&1 || true)"
grep -q "dangling sysroot link" <<<"$out" \
  || fail "A6: doctor missed the dangling link; got:\n$out"

RUN_IN default self doctor --fix >/dev/null 2>&1 \
  || fail "A6: doctor --fix failed"
# Both halves, again: a link that is still there fails -e too.
if [[ -L "$HOME_DIR/subos/default/usr/include/linux" ]]; then
  fail "A6: --fix left the dangling link on disk"
fi

# ── A7: the portable spelling a recipe can write today ───────────────
#
# `<home>/subos/current` is the symlink `self init` creates and
# `subos use --global` maintains. A recipe that writes it instead of the
# install-time subos records something that already follows the user: an old
# client (no exec-time normalization) follows the link rather than freezing,
# and a current client normalizes it exactly like any other subos path.
#
# So it must NOT be reported as a baked path -- otherwise the recipes doing
# the right thing keep a standing warning they can do nothing about, and the
# whole reason to change them disappears.
log "A7: a recipe writing subos/current is lifted like any other subos path"
cat > "$LOCAL_INDEX_DIR/pkgs/s/sr-portable.lua" <<'LUA'
package = {
    spec = "1", name = "sr-portable",
    description = "portable subos path fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.system")
import("xim.libxpkg.xvm")
-- What a real recipe would do: keep the sysroot the API hands back, but spell
-- the subos segment `current` so the path is not frozen at install time.
local function portable_sysroot()
    local dir = system.subos_sysrootdir()
    return (dir:gsub("([/\\])subos([/\\])[^/\\]+", "%1subos%2current", 1))
end
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "sp-real"),
                 "#!/bin/sh\necho \"probe-args: $*\"\n")
    return true
end
function config()
    xvm.add("sr-portable", {
        bindir = path.join(pkginfo.install_dir(), "bin"),
        alias  = "sp-real --sysroot=" .. portable_sysroot(),
    })
    return true
end
function uninstall() xvm.remove("sr-portable") return true end
LUA

RUN_IN dev install sr-portable >/dev/null 2>&1 || fail "A7: install failed"
chmod +x "$(find "$HOME_DIR/data/xpkgs" -name sp-real -type f | head -1)"

# The portable spelling gets the SAME treatment as any other subos path:
# lifted out and recorded as intent. `current` is better than a pinned subos
# and it is still a subos path in a database every subos shares, so the record
# must not keep it either.
if grep -q '/subos/current' "$HOME_DIR/.xlings.json"; then
  fail "A7: the portable spelling was stored instead of being lifted"
fi
grep -q '"sysroot": true' "$HOME_DIR/.xlings.json" \
  || fail "A7: the portable spelling was dropped without recording the intent"

out="$(RUN_IN default self doctor 2>&1 || true)"
if grep -q "sr-portable" <<<"$out"; then
  fail "A7: a lifted record was reported as a baked path; got:\n$out"
fi

# ...and it still resolves to whichever subos is active, not to `current`.
RUN_IN dev install sr-portable >/dev/null 2>&1 || true
out="$(RUN_SHIM default "$HOME_DIR/subos/default/bin/sr-portable" 2>&1 || true)"
grep -q "$HOME_DIR/subos/default" <<<"$out" \
  || fail "A7: the portable spelling did not reach the active subos; got:\n$out"
if grep -q "/subos/current" <<<"$out"; then
  fail "A7: the shim handed the compiler the symlink instead of the real dir; got:\n$out"
fi

log "PASS: subos_alias_sysroot"
