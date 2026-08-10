#!/usr/bin/env bash
# Cross-check what `xlings subos info` SAYS about the graphics wiring against
# what the dynamic loader actually DOES.
#
# WHY THIS EXISTS
#
# The graphics stack's failure mode is that it succeeds: glvnd dlopens each
# vendor by name and, when one fails, falls through to the next with no
# diagnostic. A machine whose NVIDIA vendor cannot load still draws a window,
# still prints a GL_RENDERER, and still exits 0 — on llvmpipe.
#
# `xlings subos info` now reports a per-vendor verdict, but it READS a record
# written at install time; it does not measure. That is the right design (a
# local query answers instantly, and one question gets one answerer) and it
# has one failure mode of its own: the record can be wrong, and nothing in the
# product would notice. This script is the thing that notices.
#
# So it does not check "is the stack healthy". It checks "does the record
# agree with the loader" — a disagreement is the finding, in either direction:
#
#   record says ok,     dlopen fails   -> the panel is lying about a failure
#   record says broken, dlopen works   -> the panel invents a failure
#
# HOW THE MEASUREMENT IS DONE
#
# `dlopen(<vendor>, RTLD_NOW)` from inside the subos. The interposer names the
# host driver by absolute path in its own DT_NEEDED, so opening the interposer
# loads the host driver, and the host driver's own DT_NEEDED is resolved using
# the interposer's search path — but only if that path is TRANSITIVE. That is
# the whole bug: DT_RUNPATH is not transitive, DT_RPATH is. Nothing here
# inspects a tag; it opens the library and reports what the loader said.
#
# It must run INSIDE the subos: the failure only appears under our loader with
# our payloads, where the libraries glibc 2.34 merged into libc (libpthread,
# librt, libdl) do not exist as separate files the way they still do on the
# host. Run on the host, every vendor opens fine and this script proves
# nothing.
#
# Usage:
#   .agents/tools/graphics-acceptance.sh [--home DIR] [--subos NAME] [--bin PATH]
#
# Exit: 0 when the record and the loader agree on every vendor, 1 otherwise.
set -euo pipefail

HOME_DIR="${XLINGS_HOME:-$HOME/.xlings}"
SUBOS="default"
# An absolute path, never `find | head -1`: several fingerprint directories
# can hold an `xlings`, and picking the wrong one silently tests an older
# build. Defaults to the newest, but say so.
XBIN=""

while [ $# -gt 0 ]; do
    case "$1" in
        --home)  HOME_DIR="$2"; shift 2 ;;
        --subos) SUBOS="$2";    shift 2 ;;
        --bin)   XBIN="$2";     shift 2 ;;
        -h|--help) sed -n '1,50p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$XBIN" ]; then
    # `ls -t` over an explicit glob, and the result is printed. A silently
    # chosen binary is how a green run gets attributed to the wrong build.
    XBIN=$(ls -t target/*/*/bin/xlings 2>/dev/null | head -1 || true)
    [ -n "$XBIN" ] || { echo "no xlings binary under target/; pass --bin" >&2; exit 2; }
    XBIN=$(cd "$(dirname "$XBIN")" && pwd)/xlings
fi

echo "xlings : $XBIN"
echo "home   : $HOME_DIR"
echo "subos  : $SUBOS"
echo

# ── what the product says ──────────────────────────────────────────────────
#
# Read the record itself rather than scraping the panel: the panel is prose
# aimed at a human and will be reworded, and a harness that greps prose fails
# on a wording change while the thing it guards is fine.
DISPATCH=$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos info "$SUBOS" 2>/dev/null \
           | awk '/GL dispatch/ {print $3; exit}')
if [ -z "$DISPATCH" ]; then
    echo "SKIP: subos '$SUBOS' has no GL dispatch — nothing to cross-check"
    exit 0
fi
RECORD="$DISPATCH/lib/glx-vendor/.wiring"
VENDOR_DIR="$DISPATCH/lib/glx-vendor"

if [ ! -f "$RECORD" ]; then
    echo "FAIL: dispatch present but no wiring record at $RECORD"
    echo "      run 'xlings install graphics' in this home first"
    exit 1
fi

echo "── record ─────────────────────────────────────────────"
cat "$RECORD"
echo

# ── what the loader does ───────────────────────────────────────────────────

PROBE_SRC=$(mktemp -t glvnd-probe-XXXXXX.c)
PROBE_BIN="${PROBE_SRC%.c}"
trap 'rm -f "$PROBE_SRC" "$PROBE_BIN"' EXIT

cat > "$PROBE_SRC" <<'EOF'
/* Open each vendor library the way glvnd would, and say what the loader said.
   RTLD_NOW so an unresolved symbol surfaces here rather than at first call. */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        void* h = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        if (h) {
            printf("LOADED %s\n", argv[i]);
            dlclose(h);
        } else {
            printf("FAILED %s :: %s\n", argv[i], dlerror());
            bad = 1;
        }
    }
    return bad;
}
EOF

# Probe the libraries the RECORD names, by BARE SONAME — which is how glvnd
# opens them, and is the only form that covers all four entry points.
#
# Listing `glx-vendor/` instead was the first version of this, and it silently
# measured a third of the stack: that directory holds the GLX vendors only
# (that is its whole job — it is what libGLX.so.0's RPATH points at). EGL
# vendors are found through `__EGL_VENDOR_LIBRARY_DIRS` JSON and the GLES ones
# through the farm, so four of six libraries were never opened while this
# script printed PASS. Driving the probe from the record instead makes the set
# measured equal to the set claimed, by construction.
VENDORS=$(sed -n 's/^vendor=\([^ ]*\).*/\1/p' "$RECORD")
[ -n "$VENDORS" ] || { echo "FAIL: the record names no vendor"; exit 1; }

if [ ! -d "$VENDOR_DIR" ] || [ -z "$(ls -A "$VENDOR_DIR" 2>/dev/null | grep -v '^\.')" ]; then
    echo "FAIL: GLX vendor directory is empty — GL falls back to software"
    exit 1
fi

# Each SONAME resolved through the subos farm to the real payload file.
#
# NOT the bare SONAME: nothing puts the farm's lib directory on the probe's
# search path, so every `dlopen("libGLX_nvidia.so.0")` returns "cannot open
# shared object file" — an artifact of the probe, indistinguishable in the
# output from the real "the closure is incomplete" failure this measures.
#
# NOT the `glx-vendor/` entry either, even for the GLX vendors: `$ORIGIN` in
# an RPATH expands against the path the library was OPENED by, not its
# realpath, so opening a symlink anchors the search somewhere the loader would
# not. The payload file is the one location every route ends at.
SUBOS_DIR=$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos info "$SUBOS" 2>/dev/null \
            | awk '/^ *dir /{print $2; exit}')
[ -n "$SUBOS_DIR" ] || { echo "FAIL: could not resolve the subos directory" >&2; exit 1; }

ARGS=""
UNRESOLVED=""
for v in $VENDORS; do
    p=$(readlink -f "$SUBOS_DIR/lib/$v" 2>/dev/null || true)
    if [ -n "$p" ] && [ -f "$p" ]; then
        ARGS="$ARGS $p"
    else
        # A vendor the record names that the farm does not carry. Report it as
        # its own outcome rather than letting it look like a load failure.
        UNRESOLVED="$UNRESOLVED $v"
    fi
done
[ -z "$UNRESOLVED" ] || echo "not in this subos's farm:$UNRESOLVED"

echo "── loader ─────────────────────────────────────────────"
# `subos use --cmd` is the supported way to run something inside the subos.
# The compile and the run happen in the SAME invocation: two invocations can
# straddle a `use`, and then the probe is built against one subos and run in
# another.
# BOTH TAGS, and this is the correction that matters most in this file.
#
# The probe used to be built with the toolchain's default dtags, which is
# DT_RUNPATH -- the same tag the record's verdict assumes. So the tool agreed
# with the record by SHARING ITS ASSUMPTION, and reported "record and loader
# agree" about vendors that an installed program loads without trouble
# (openxlings/xlings#537).
#
# A measuring tool that carries the assumption under test agrees with it,
# wrongly. That is the third form of this trap in this one file: the first
# took its probe set from a directory listing, the second probed by bare
# SONAME. Both printed PASS having measured the wrong thing.
#
# DT_RPATH is what an INSTALLED program carries (elfpatch stamps it since
# libxpkg 0.0.57); DT_RUNPATH is what a program built in the subos gets
# (#532). A vendor that loads under one and not the other is not `broken` and
# not `ok` -- it is `needs-transitive-consumer`, and only measuring both can
# tell those three apart.
MEASURED=$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos use "$SUBOS" --cmd \
    "gcc -O0 -o '$PROBE_BIN' '$PROBE_SRC' -ldl && '$PROBE_BIN' $ARGS" 2>&1 || true)
MEASURED_RPATH=$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos use "$SUBOS" --cmd \
    "gcc -O0 -o '$PROBE_BIN.rp' '$PROBE_SRC' -ldl \
       -Wl,--disable-new-dtags,-rpath,$SUBOS_DIR/lib && '$PROBE_BIN.rp' $ARGS" 2>&1 || true)
echo "$MEASURED"
echo

# ── the comparison ─────────────────────────────────────────────────────────

fail=0
while read -r line; do
    case "$line" in vendor=*) ;; *) continue ;; esac
    soname=$(echo "$line" | sed -n 's/^vendor=\([^ ]*\).*/\1/p')
    state=$(echo "$line" | sed -n 's/.* state=\([^ ]*\).*/\1/p')
    # `native` means our own build with no host driver behind it. It is a pass,
    # and it is a DIFFERENT claim from `ok` — do not collapse them.
    case "$state" in
        ok|native)                  claimed="loads" ;;
        broken)                     claimed="fails" ;;
        needs-transitive-consumer)  claimed="needs-transitive-consumer" ;;
        *)                          claimed="unknown" ;;
    esac

    # Match on the path that was actually probed, resolved the same way it was
    # built. Matching on the SONAME as a trailing path component looks
    # equivalent and is not: `readlink -f` lands on the real file, whose name
    # is the fully-versioned one (`libEGL_mesa.so.0.0.0`), so a SONAME match
    # found nothing for every vendor whose farm entry is a symlink chain — and
    # "found nothing" is one edit away from reading as "agrees".
    probed=$(readlink -f "$SUBOS_DIR/lib/$soname" 2>/dev/null || true)
    if [ -n "$probed" ] && echo "$MEASURED" | grep -qF "LOADED $probed"; then
        observed="loads"
    elif [ -n "$probed" ] && echo "$MEASURED" | grep -qF "FAILED $probed "; then
        observed="fails"
    else
        observed="not-measured"
    fi
    if [ -n "$probed" ] && echo "$MEASURED_RPATH" | grep -qF "LOADED $probed"; then
        observed_rp="loads"
    elif [ -n "$probed" ] && echo "$MEASURED_RPATH" | grep -qF "FAILED $probed "; then
        observed_rp="fails"
    else
        observed_rp="not-measured"
    fi
    # The two tags disagreeing IS a state, not a contradiction to resolve.
    if [ "$observed" = "fails" ] && [ "$observed_rp" = "loads" ]; then
        observed="needs-transitive-consumer"
    fi

    # An unmeasured vendor is NOT an agreeing vendor. The first version of
    # this script let `?` rows through and printed PASS having opened two of
    # six libraries — the same shape as the bug the whole graphics round
    # exists to remove: a check that reports success for work it did not do.
    if [ "$claimed" = "unknown" ] || [ "$observed" = "not-measured" ]; then
        printf '  XX %-28s record=%-10s loader=%s   <-- NOT CHECKED\n' \
               "$soname" "$state" "$observed"
        fail=1
        continue
    fi
    if [ "$claimed" = "$observed" ]; then
        printf '  ok %-28s record=%-10s loader=%s\n' "$soname" "$state" "$observed"
    else
        printf '  XX %-28s record=%-10s loader=%s   <-- DISAGREE\n' \
               "$soname" "$state" "$observed"
        fail=1
    fi
done < "$RECORD"

echo
if [ "$fail" -eq 0 ]; then
    n=$(printf '%s\n' "$VENDORS" | wc -l)
    echo "PASS: the record and the loader agree on all $n vendors"
else
    echo "FAIL: a vendor the panel describes was not confirmed by the loader"
fi
exit "$fail"
