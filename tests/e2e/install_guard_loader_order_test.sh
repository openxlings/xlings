#!/usr/bin/env bash
# E2E-97: the install-time same-source guard judges what the LOADER would do,
# and refuses one node rather than one plan.
#
# Two defects, one fixture. Both were found on a real home, where the guard's
# predicate also drives `self doctor`:
#
#   P1  The predicate demanded that EVERY RUNPATH directory holding a core
#       file agree with the interpreter's payload. The loader stops at the
#       first directory that has a soname, so a complete glibc lib dir ahead
#       of a subos farm that has since moved to another glibc is not a split:
#       the farm's libc is never opened. 152 doctor errors on the measured
#       home, 0 real; the same predicate REFUSED installs.
#   P2  The refusal `return`ed out of Installer::execute -- the shape the
#       caller documents as "cancel or plan-level error" -- so one refused
#       node took every other node down with it, no install_summary was sent,
#       and no failure marker was written.
#
# The rig: fake glibc payloads 1.0 and 2.0 in the store; the default subos lib
# farm symlinks to 2.0. Three payload-free recipes carry one rigged
# executable each:
#
#   orderok    PT_INTERP -> 1.0; RUNPATH [1.0/lib64, farm]  -> loader takes 1.0's
#              libc: NOT a split. Must install. (old binary: refused)
#   splitreal  PT_INTERP -> 1.0; RUNPATH [farm]              -> libc comes from
#              2.0: a REAL split. Must be refused, WITH a failure marker.
#   plainok    host-linked, untouched                        -> must install even
#              when planned together with splitreal.
#
# Asserted, in order:
#   A1 `install orderok`            exits 0, no mismatch in the output
#   A2 its stamp is not marked incomplete
#   B1 `install splitreal plainok`  exits non-zero and names splitreal
#   B2 splitreal's payload carries the failure marker    (old binary: none)
#   B3 plainok is installed anyway, stamp not incomplete (old binary: plan died)
#   B4 the message still says this is a resolution defect to report
#
# Needs gcc + patchelf to build the rig; Linux only (PT_INTERP into a store
# payload is the Linux arrangement). Missing tools SKIP loudly.

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

[[ "$(uname -s)" == "Linux" ]] \
  || { log "SKIP: the same-source guard only has a subject on Linux"; exit 0; }
for tool in gcc patchelf; do
  command -v "$tool" >/dev/null 2>&1 \
    || { log "SKIP: $tool not available (CI installs it; local runs need it on PATH)"; exit 0; }
done

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/install_guard_loader_order"
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
mkdir -p "$LOCAL_INDEX_DIR/pkgs/o" "$LOCAL_INDEX_DIR/pkgs/s" "$LOCAL_INDEX_DIR/pkgs/p"

# ── the store: two fake glibc payloads, and a farm that points at the newer ──
FAKE1="$HOME_DIR/data/xpkgs/xim-x-glibc/1.0"
FAKE2="$HOME_DIR/data/xpkgs/xim-x-glibc/2.0"
FARM="$HOME_DIR/subos/default/lib"
mkdir -p "$FAKE1/lib64" "$FAKE2/lib64" "$FARM"
for so in ld-linux-x86-64.so.2 libc.so.6; do
  touch "$FAKE1/lib64/$so" "$FAKE2/lib64/$so"
  ln -sfn "$FAKE2/lib64/$so" "$FARM/$so"
done

# ── the rigged executables ─────────────────────────────────────────────
printf 'int main(void){return 0;}\n' > "$RIG_DIR/main.c"
gcc -o "$RIG_DIR/plainok" "$RIG_DIR/main.c" || fail "gcc failed on a 1-line program"
cp "$RIG_DIR/plainok" "$RIG_DIR/orderok"
cp "$RIG_DIR/plainok" "$RIG_DIR/splitreal"

# Every soname the rig NEEDs has to exist in 1.0 so rule D (closure) stays
# quiet; content is irrelevant, the scans are name maps.
while read -r so; do
  [ -z "$so" ] && continue
  touch "$FAKE1/lib64/$so" "$FAKE2/lib64/$so"
done < <(patchelf --print-needed "$RIG_DIR/plainok")

patchelf --set-interpreter "$FAKE1/lib64/ld-linux-x86-64.so.2" "$RIG_DIR/orderok" \
  || fail "patchelf --set-interpreter failed (orderok)"
patchelf --force-rpath --set-rpath "$FAKE1/lib64:$FARM" "$RIG_DIR/orderok" \
  || fail "patchelf --set-rpath failed (orderok)"

patchelf --set-interpreter "$FAKE1/lib64/ld-linux-x86-64.so.2" "$RIG_DIR/splitreal" \
  || fail "patchelf --set-interpreter failed (splitreal)"
patchelf --force-rpath --set-rpath "$FARM" "$RIG_DIR/splitreal" \
  || fail "patchelf --set-rpath failed (splitreal)"

# ── three fixture recipes that carry them into payloads ────────────────
write_recipe() {
  local name="$1" letter="$2" rig="$3"
  cat > "$LOCAL_INDEX_DIR/pkgs/$letter/$name.lua" <<LUA
package = {
    spec = "1",
    name = "$name",
    description = "Local fixture for tests/e2e/install_guard_loader_order_test.sh",
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
    os.cp("$rig", path.join(dir, "bin", "$name"))
    return true
end

function config()
    return true
end
LUA
}
write_recipe orderok   o "$RIG_DIR/orderok"
write_recipe splitreal s "$RIG_DIR/splitreal"
write_recipe plainok   p "$RIG_DIR/plainok"

# ── isolated home ──────────────────────────────────────────────────────
mkdir -p "$HOME_DIR/data/xim-index-repos"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "${XLINGS_TEST_MIRROR:-GLOBAL}",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$BIN" "$@" ) }

x self init >/dev/null 2>&1 || true

# `self init` may have (re)built the farm; the rig needs it pointing at 2.0.
for so in ld-linux-x86-64.so.2 libc.so.6; do
  ln -sfn "$FAKE2/lib64/$so" "$FARM/$so"
done

stamp_of() { find "$HOME_DIR/data/xpkgs" -mindepth 3 -maxdepth 3 -name '.xpkg-install.json' -path "*-x-$1/*" -print -quit; }

# ── A: own glibc ahead of a drifted farm is not a split ─────────────────
RC=0
OUT="$(x install orderok@1.0.0 -y 2>&1)" || RC=$?
printf '%s\n' "$OUT" | sed -n '1,30{s/^/      | /;p}'
[ "$RC" = "0" ] || fail "A1: orderok must install (its own 1.0 lib dir answers libc.so.6 before the farm); exited $RC"
echo "$OUT" | strip_ansi | grep -q "loader/libc payload mismatch" \
  && fail "A1: the guard reported a split on a binary whose loader never reaches the farm"
log "  ✓ A1: orderok installed; no split reported"

STAMP="$(stamp_of orderok)"
[ -n "$STAMP" ] || fail "A2: orderok has no install stamp"
grep -q '"incomplete": true' "$STAMP" && fail "A2: orderok's stamp is marked incomplete"
log "  ✓ A2: orderok's stamp is clean"

# ── B: a real split is refused as ONE node, and the plan goes on ────────
RC=0
OUT="$(x install splitreal@1.0.0 plainok@1.0.0 -y 2>&1)" || RC=$?
printf '%s\n' "$OUT" | sed -n '1,40{s/^/      | /;p}'
[ "$RC" != "0" ] || fail "B1: a plan containing a real split must exit non-zero"
echo "$OUT" | strip_ansi | grep -q "splitreal" \
  || fail "B1: the failure does not name splitreal:
$OUT"
log "  ✓ B1: the plan exits non-zero and names splitreal"

STAMP="$(stamp_of splitreal)"
[ -n "$STAMP" ] || fail "B2: splitreal left no stamp at all (payload on disk, nothing saying why)"
grep -q '"incomplete": true' "$STAMP" \
  || fail "B2: splitreal's refusal wrote no failure marker; the stamp reads as a finished install"
grep -q 'loader/libc' "$STAMP" \
  || fail "B2: the marker does not say WHY (expected the mismatch in its reason)"
log "  ✓ B2: splitreal carries a failure marker naming the mismatch"

STAMP="$(stamp_of plainok)"
[ -n "$STAMP" ] || fail "B3: plainok was not installed -- one refused node took the plan down"
grep -q '"incomplete": true' "$STAMP" && fail "B3: plainok's stamp is marked incomplete"
log "  ✓ B3: plainok installed in the same plan"

echo "$OUT" | strip_ansi | grep -q "resolution defect" \
  || fail "B4: the refusal no longer says it is a resolution defect to report"
log "  ✓ B4: the refusal still asks for a report"

log "E2E install-guard-loader-order: PASS"
