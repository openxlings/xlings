#!/usr/bin/env bash
# E2E: `xlings subos info` tells the user which driver actually renders.
#
# The graphics stack fails by succeeding. glvnd dlopens each vendor by name
# and falls through on failure with no diagnostic, so a subos whose NVIDIA
# vendor cannot load still draws a window, still prints a GL_RENDERER, and
# still exits 0 — on llvmpipe. Measured on a real host: the install itself
# says nothing either, because a config hook's log does not surface on the
# success path. This panel is the only channel the verdict has.
#
# The unit tests cover the reader. What they cannot cover is the path from a
# record on disk, through the JSON event, to the characters a user sees — and
# that path is where a wrong colour or a dropped row would turn a reported
# failure back into silence.
#
# So this is a DIFFERENTIAL, not a smoke test. The same command is run over
# four states of one subos, and the point is that they produce four DIFFERENT
# answers:
#
#   1. broken vendor recorded   -> names it, and says GL will render elsewhere
#   2. vendor directory empty   -> says every GL program falls to software
#   3. vendors but no record    -> says nobody measured them
#   4. no dispatch at all       -> says nothing (this subos does no GL)
#
# A reader that printed a cheerful summary in all four would pass a test that
# only asserted "the graphics section appears".
set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$(runtime_home_dir subos_graphics_wiring)"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { [[ -n "${E2E_KEEP:-}" ]] || rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

assert_home_is_isolated "$HOME_DIR"
BIN="$(find_xlings_bin)"
log "client: $("$BIN" --version 2>&1 | head -1)"

mkdir -p "$HOME_DIR/subos/default/lib"
cat > "$HOME_DIR/.xlings.json" <<EOF
{ "mirror": "GLOBAL", "index_repos": [] }
EOF

# A libglvnd payload and the farm entry that points at it. Nothing is
# installed: the panel reads the filesystem, and building the tree by hand is
# what lets this test assert on states a real install cannot be talked into
# producing on demand.
PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-libglvnd/1.7.0.1"
VENDOR_DIR="$PAYLOAD/lib/glx-vendor"
mkdir -p "$VENDOR_DIR"
: > "$PAYLOAD/lib/libGLX.so.0"
ln -sf "$PAYLOAD/lib/libGLX.so.0" "$HOME_DIR/subos/default/lib/libGLX.so.0"

info_output() {
  run_xlings "$HOME_DIR" "" subos info default 2>&1 | strip_ansi
}

# ── 1. a recorded failure is named, with its consequence ───────────────────
#
# The consequence is the load-bearing half. "broken" alone reads as "this
# feature is unavailable"; what actually happens is that GL keeps working on
# another driver and says nothing, which is why nobody noticed for so long.

: > "$VENDOR_DIR/libGLX_nvidia.so.0"
: > "$VENDOR_DIR/libGLX_mesa.so.0"
cat > "$VENDOR_DIR/.wiring" <<EOF
dispatch=$PAYLOAD
vendor=libGLX_nvidia.so.0 state=ok
vendor=libEGL_nvidia.so.0 state=broken reason=runpath-not-transitive
vendor=libGLESv2_nvidia.so.2 state=broken missing=libpthread.so.0
vendor=libGLX_mesa.so.0 state=native
EOF

OUT="$(info_output)"
log "── recorded ──"
printf '%s\n' "$OUT" | sed 's/^/    /'

assert_contains "$OUT" "GL dispatch"       "the payload this subos loads"
assert_contains "$OUT" "nvidia EGL"        "the failing entry point, by name"
assert_contains "$OUT" "BROKEN"            "and that it is broken"
assert_contains "$OUT" "without saying so" "and what will happen instead"
assert_contains "$OUT" "libpthread.so.0"   "the unresolved library, when known"
assert_contains "$OUT" "nvidia GLX"        "the entry point that does work"

# The vendor that works must NOT be marked. A panel that flags every row is a
# panel with no signal in it.
if printf '%s\n' "$OUT" | grep -E "nvidia GLX +(ok|.*)" | grep -q "BROKEN"; then
  fail "a working vendor was reported as broken"
fi

# Each of the four entry points is its own load-chain root, and a real host
# measured GLX rendering on the GPU while EGL silently fell back. If the panel
# collapsed a vendor to one verdict, this is where it shows.
if ! printf '%s\n' "$OUT" | grep -q "nvidia GLESv2"; then
  fail "GLESv2 was recorded but is not reported — entry points are being collapsed"
fi

# ── 2. an empty vendor directory is the software-rendering failure ─────────

rm -f "$VENDOR_DIR"/* "$VENDOR_DIR"/.wiring
OUT="$(info_output)"
log "── no vendors ──"
printf '%s\n' "$OUT" | sed 's/^/    /'
assert_contains "$OUT" "software rendering" "an empty vendor dir is called what it is"
if printf '%s\n' "$OUT" | grep -q "nvidia"; then
  fail "a vendor was reported for a subos that has none"
fi

# ── 3. vendors with no record are unmeasured, not healthy ─────────────────

: > "$VENDOR_DIR/libGLX_nvidia.so.0"
OUT="$(info_output)"
log "── unrecorded ──"
printf '%s\n' "$OUT" | sed 's/^/    /'
# It must not claim the vendor works, and it must not claim software rendering
# either — both would be answers nobody measured.
if printf '%s\n' "$OUT" | grep -qi "software rendering"; then
  fail "a subos WITH a vendor was reported as falling back to software"
fi
assert_contains "$OUT" "wired before" "an unmeasured stack says so"

# ── 4. no dispatch: the section must be absent, not reassuring ─────────────
#
# Most subos do no graphics. An extra section on every one of them is noise,
# and a "graphics: none" row invites the reading that something is missing.

rm -f "$HOME_DIR/subos/default/lib/libGLX.so.0"
OUT="$(info_output)"
log "── no dispatch ──"
printf '%s\n' "$OUT" | sed 's/^/    /'
if printf '%s\n' "$OUT" | grep -qi "GL dispatch\|vendors\|software rendering"; then
  fail "a subos with no GL dispatch grew a graphics section"
fi
# The panel itself must still work.
assert_contains "$OUT" "default" "the subos is still reported"

# ── 5. the answer is per-subos, not per-home ──────────────────────────────
#
# The architectural claim behind reading `<subos>/lib/libGLX.so.0` rather than
# searching the store: a home can hold several libglvnd payloads and the
# question "which one renders" only has an answer inside a subos.
#
# So put the payload back — untouched, exactly where it was — and ask a
# SECOND subos in the SAME home. A store-searching implementation finds the
# payload and reports a graphics stack for a subos where no GL program could
# load one: a confident wrong answer, which is worse than no answer.

fs_restore() {
  mkdir -p "$VENDOR_DIR"
  : > "$PAYLOAD/lib/libGLX.so.0"
  : > "$VENDOR_DIR/libGLX_nvidia.so.0"
  printf 'dispatch=%s\nvendor=libGLX_nvidia.so.0 state=ok\n' "$PAYLOAD" \
    > "$VENDOR_DIR/.wiring"
}
fs_restore
mkdir -p "$HOME_DIR/subos/nogfx/lib"
ln -sf "$PAYLOAD/lib/libGLX.so.0" "$HOME_DIR/subos/default/lib/libGLX.so.0"

WIRED="$(info_output)"
OUT="$(run_xlings "$HOME_DIR" "" subos info nogfx 2>&1 | strip_ansi)"
log "── second subos, same home ──"
printf '%s\n' "$OUT" | sed 's/^/    /'

assert_contains "$WIRED" "nvidia GLX" "the wired subos still reports its vendor"
if printf '%s\n' "$OUT" | grep -qi "GL dispatch\|nvidia\|vendors"; then
  fail "a subos with no dispatch inherited another subos's graphics stack"
fi

log "PASS: four states of one subos produce four different answers, and a"
log "      second subos in the same home is answered separately"
