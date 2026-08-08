#!/usr/bin/env bash
# E2E: the install-time closure guard (C2, execution point 2) agrees with the
# index CI's dep-closure-check.sh on the same tree -- and it actually fires.
#
# Two implementations of one predicate is how reporter/repairer pairs have
# drifted three times in this repo. The client check is C++ (patchelf reads),
# the CI check is bash (readelf) -- they cannot share code, so they share a
# fixture: one rigged payload, both verdicts, asserted to agree.
#
# The rig: an executable whose PT_INTERP points into an xpkgs glibc payload
# at version 1.0 (form X, and older than any real host glibc), NEEDing
#   libc.so.6        -- provided by the fake glibc payload   -> NOT a gap
#   libnothere.so.9  -- provided by nothing                  -> the gap
#
# Asserted, in order:
#   1. install exits 0                 (the guard is warn-only, C2.5)
#   2. warn names libnothere.so.9     (rule D fires on the real gap)
#   3. warn does NOT name libc.so.6   (a provided soname is not a gap)
#   4. version-floor warn names 1.0 vs the recorded host glibc (rule A)
#   5. dep-closure-check.sh on the same payload also fails, also names
#      libnothere.so.9, and also does not call libc.so.6 host-only
#
# Needs gcc + patchelf to build the rig, lua + readelf for the CI script.
# Missing tools SKIP loudly -- in CI the workflow installs them, so a skip
# there is a workflow regression, not a pass.

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

for tool in gcc patchelf lua5.4 readelf; do
  command -v "$tool" >/dev/null 2>&1 \
    || { log "SKIP: $tool not available (CI installs it; local runs need it on PATH)"; exit 0; }
done

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/closure_guard_differential"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
HOME_DIR="$RUNTIME_DIR/home"
RIG_DIR="$RUNTIME_DIR/rig"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR" "$RIG_DIR"

BIN="$(find_xlings_bin)"
log "client: $("$BIN" --version 2>&1 | head -1)"

cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/c"

# ── the rigged executable ───────────────────────────────────────────────
printf 'int main(void){return 0;}\n' > "$RIG_DIR/main.c"
gcc -o "$RIG_DIR/rigged" "$RIG_DIR/main.c" || fail "gcc failed on a 1-line program"

FAKE_GLIBC="$HOME_DIR/data/xpkgs/xim-x-glibc/1.0"
patchelf --set-interpreter "$FAKE_GLIBC/lib64/ld-linux-x86-64.so.2" "$RIG_DIR/rigged" \
  || fail "patchelf --set-interpreter failed"
patchelf --add-needed libnothere.so.9 "$RIG_DIR/rigged" \
  || fail "patchelf --add-needed failed"

# ── the fixture recipe that carries it into a payload ───────────────────
cat > "$LOCAL_INDEX_DIR/pkgs/c/closurefix.lua" <<LUA
package = {
    spec = "1",
    name = "closurefix",
    description = "Local fixture for tests/e2e/closure_guard_differential_test.sh",
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

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    os.cp("$RIG_DIR/rigged", path.join(dir, "bin", "rigged"))
    return true
end

function config()
    return true
end
LUA

# ── isolated home; the guard needs the payload patchelf and a recorded
#    host glibc, both of which a fresh `self init` provides on this branch ──
mkdir -p "$HOME_DIR/data/xim-index-repos"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$BIN" "$@" ) }

x self init >/dev/null 2>&1 || true

HOST_GLIBC="$(/usr/bin/getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')"
[ -n "$HOST_GLIBC" ] || { log "SKIP: no glibc host (rule A unprovable here)"; exit 0; }

# Providers: everything the rig ACTUALLY NEEDs except the deliberate gap.
# Derived rather than hardcoded -- which sonames a 1-line program NEEDs
# depends on the compiler that built it (a subos gcc emits `libc.so`
# alongside `libc.so.6`), and a hardcoded list turns compiler variance into
# a false positive. Content is irrelevant: the provider map is a name map.
mkdir -p "$FAKE_GLIBC/lib64"
touch "$FAKE_GLIBC/lib64/ld-linux-x86-64.so.2"
while read -r so; do
  [ -z "$so" ] && continue
  [ "$so" = "libnothere.so.9" ] && continue
  touch "$FAKE_GLIBC/lib64/$so"
done < <(patchelf --print-needed "$RIG_DIR/rigged")

# The scan reads ELF fields through the payload patchelf (R6: xlings resolves
# the payload, never PATH). Give the store one.
REAL_PATCHELF="$(command -v patchelf)"
mkdir -p "$HOME_DIR/data/xpkgs/xim-x-patchelf/0.18.0/bin"
cp "$REAL_PATCHELF" "$HOME_DIR/data/xpkgs/xim-x-patchelf/0.18.0/bin/patchelf"

# ── side 1: the client, at install time ─────────────────────────────────
# `RC=0` then `|| RC=$?`, not `; RC=$?`: the shared lib sets -e, and a plain
# assignment from a failing substitution kills the script before the next
# statement -- silently, which is worse than the failure it hides.
RC=0
OUT="$(x install closurefix@1.0.0 -y 2>&1)" || RC=$?
printf '%s\n' "$OUT" | sed -n '1,40{s/^/      | /;p}'

[ "$RC" = "0" ] || fail "1: guard is warn-only but install exited $RC"
log "  ✓ 1: install succeeds (warn-only)"

echo "$OUT" | strip_ansi | grep -q "closure gap.*libnothere\.so\.9\|libnothere\.so\.9.*closure gap" \
  || echo "$OUT" | strip_ansi | grep "libnothere.so.9" | grep -q "closure gap" \
  || fail "2: rule D did not name libnothere.so.9:
$OUT"
log "  ✓ 2: rule D names the real gap (libnothere.so.9)"

echo "$OUT" | strip_ansi | grep "closure gap" | grep -q "libc\.so\.6" \
  && fail "3: rule D flagged libc.so.6, which the fake glibc payload provides"
log "  ✓ 3: a provided soname is not a gap"

echo "$OUT" | strip_ansi | grep "version floor" | grep -q "1\.0.*$HOST_GLIBC\|$HOST_GLIBC.*1\.0" \
  || fail "4: rule A did not report glibc 1.0 vs host $HOST_GLIBC:
$OUT"
log "  ✓ 4: rule A reports the version floor (1.0 < host $HOST_GLIBC)"

# ── side 2: the index CI's script, same tree ────────────────────────────
# -print -quit, not `| head -1`: the shared lib sets -e and this file sets
# pipefail, so find taking SIGPIPE from head on a second match kills the
# whole script with no output at all.
PAYLOAD="$(find "$HOME_DIR/data/xpkgs" -mindepth 2 -maxdepth 2 -type d \
                -path '*closurefix*' -print -quit)"
[ -n "$PAYLOAD" ] || fail "5: no closurefix payload on disk"

CI_SCRIPT="$LOCAL_INDEX_DIR/.github/scripts/dep-closure-check.sh"
[ -f "$CI_SCRIPT" ] || { log "SKIP-HALF: fixture index has no dep-closure-check.sh"; exit 0; }

CI_RC=0
CI_OUT="$(bash "$CI_SCRIPT" "$PAYLOAD" "$LOCAL_INDEX_DIR/pkgs/c/closurefix.lua" \
                linux "$HOME_DIR/data/xpkgs" 2>&1)" || CI_RC=$?
printf '%s\n' "$CI_OUT" | sed -n '1,30{s/^/      | /;p}'

[ "$CI_RC" = "1" ] \
  || fail "5: dep-closure-check.sh returned $CI_RC on a payload the client warned about (verdicts drifted)"
echo "$CI_OUT" | grep -q "libnothere.so.9" \
  || fail "5: the CI script's failure does not name libnothere.so.9 (different defect?):
$CI_OUT"
# Its D2 (host-only) list must not contain the provided soname either.
echo "$CI_OUT" | grep -i "no provider\|host-only" | grep -q "libc\.so\.6" \
  && fail "5: the CI script calls libc.so.6 unprovided; the client does not (drift)"
log "  ✓ 5: dep-closure-check.sh agrees: fails, names libnothere.so.9, not libc.so.6"

log "E2E closure-guard-differential: PASS"
