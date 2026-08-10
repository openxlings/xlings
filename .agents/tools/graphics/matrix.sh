#!/usr/bin/env bash
# The graphics acceptance matrix: who actually renders, in every cell.
#
# WHY A MATRIX AND NOT A TEST
#
# This stack has never failed by not working. It fails by working through
# something else and not saying so — glvnd falls through to the next vendor,
# mesa falls back to llvmpipe, EGL falls back to zink, and every one of those
# paths returns success. So the useful artifact is not pass/fail; it is a
# table of WHO rendered each cell, next to who was supposed to.
#
# Three axes, because this stack has been observed to differ along each:
#
#   API         glvnd dispatches GLX, EGL, GLESv1 and GLESv2 through SEPARATE
#               vendor libraries. Each is its own load-chain root and fails on
#               its own — measured: GLX on the GPU while EGL fell to zink.
#   Path        GPU / CPU / offline-headless. The headless path uses a
#               different EGL platform entirely (surfaceless, device) and can
#               work when the display path does not, or the reverse.
#   Environment THREE xlings environments — plain subos, --sandbox, and
#               --sandbox --gpu. The sandbox rebuilds /dev and the mount
#               namespace, which is exactly where a GPU node or a vendor JSON
#               goes missing without a diagnostic, and --gpu is what puts them
#               back; measuring only the flagless one reports the sandbox as
#               broken when it is merely unflagged.
#
# The host is a BASELINE, not a fourth environment. It is what the machine
# does with xlings out of the way, and its only job is to separate "this box
# is like that" from "it is like that only through our stack".
#
# Usage: matrix.sh [--home DIR] [--subos NAME] [--bin PATH] [--host-baseline]
set -uo pipefail

HOME_DIR="${XLINGS_HOME:-$HOME/.xlings}"
SUBOS="default"
XBIN=""
WITH_HOST=1

while [ $# -gt 0 ]; do
    case "$1" in
        --home)  HOME_DIR="$2"; shift 2 ;;
        --subos) SUBOS="$2";    shift 2 ;;
        --bin)   XBIN="$2";     shift 2 ;;
        --no-host-baseline) WITH_HOST=0; shift ;;
        -h|--help) sed -n '1,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$XBIN" ]; then
    XBIN=$(ls -t target/*/*/bin/xlings 2>/dev/null | head -1 || true)
    [ -n "$XBIN" ] || { echo "no xlings binary under target/; pass --bin" >&2; exit 2; }
    XBIN=$(cd "$(dirname "$XBIN")" && pwd)/xlings
fi

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=$(mktemp -d -t gfx-matrix-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

echo "xlings : $XBIN"
echo "home   : $HOME_DIR"
echo "subos  : $SUBOS"
echo "display: ${DISPLAY:-<unset>}"
echo

# ── the cells ──────────────────────────────────────────────────────────────
#
# `probe|env-overrides|what this cell is supposed to prove`
#
# The expectation is prose on purpose. Hard-coding "must be NVIDIA" would make
# this file wrong on every other machine, and the value here is the comparison
# a human makes across a row, not a green tick.
#
# `{MESA_EGL_JSON}` is substituted per environment — the host's copy and the
# subos's copy are different files, and pointing at the wrong one measures the
# wrong stack.
CELLS=(
  "glx||GPU path: the default GLX vendor"
  "glx|__GLX_VENDOR_LIBRARY_NAME=nvidia|GPU path: NVIDIA GLX explicitly"
  "glx|__GLX_VENDOR_LIBRARY_NAME=mesa;LIBGL_ALWAYS_SOFTWARE=1|CPU via GLX (needs BOTH — see note)"
  "glx|DISPLAY=|offline: GLX has no headless mode — must fail, cleanly"
  "egl||GPU path via EGL (the entry point measured broken on NVIDIA)"
  "egl-surfaceless|DISPLAY=|offline: EGL surfaceless, no display server at all"
  "egl-surfaceless|DISPLAY=;LIBGL_ALWAYS_SOFTWARE=1|the software flag ALONE — expected inert"
  "egl-surfaceless|DISPLAY=;LIBGL_ALWAYS_SOFTWARE=1;__EGL_VENDOR_LIBRARY_FILENAMES={MESA_EGL_JSON}|CPU offline, actually forced"
  "egl-device|DISPLAY=|offline: EGL device platform — the headless GPU route"
  "gles1||GLESv1 dispatch (its own vendor library)"
  "gles2||GLESv2 dispatch (its own vendor library)"
  "gles2-surfaceless|DISPLAY=|offline GLESv2"
  "vulkan||Vulkan ICD reachability (same failure shape, different loader)"
)

classify() {
    # Name the renderer family, because that is the fact a reader acts on.
    # A renderer string is a driver's self-description and varies by version;
    # the family does not.
    local r="$1"
    case "$r" in
        *NVIDIA*|*nvidia*)                 echo "GPU (nvidia)" ;;
        *llvmpipe*|*softpipe*|*swrast*)    echo "CPU (software)" ;;
        *zink*)                            echo "GPU via zink (vk translation)" ;;
        *Intel*|*Mesa\ Intel*|*iris*)      echo "GPU (intel)" ;;
        *AMD*|*radeonsi*)                  echo "GPU (amd)" ;;
        "")                                echo "-" ;;
        *)                                 echo "other" ;;
    esac
}

# Can a user BUILD a GL program in this environment?
#
# A separate question from "can one run", and this stack answers them
# differently: installed GL programs run, because elfpatch writes each one an
# RPATH at package build time, while a program the user compiles here gets a
# RUNPATH naming only the glibc and gcc payloads. Reporting only the runtime
# cells would call a stack usable that a developer cannot develop against.
report_build_defect() {
    local sdir="$1"
    local src="$HOME_DIR/.gl-link-test.c"
    printf 'extern void glFlush(void);\nint main(){glFlush();return 0;}\n' > "$src"
    local out
    out=$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos use "$SUBOS" --cmd \
          "gcc -o '$HOME_DIR/.gl-link-test' '$src' -lGL && '$HOME_DIR/.gl-link-test' && echo BUILD_AND_RUN_OK" 2>&1)
    if printf '%s' "$out" | grep -q BUILD_AND_RUN_OK; then
        echo "  build a GL program (gcc -lGL): OK"
    elif printf '%s' "$out" | grep -q "undefined reference\|not found (try using -rpath"; then
        echo "  build a GL program (gcc -lGL): CANNOT LINK — libGL's own DT_NEEDED"
        echo "      is unreachable (RUNPATH is not transitive for ld either)"
    elif printf '%s' "$out" | grep -q "error while loading shared libraries"; then
        echo "  build a GL program (gcc -lGL): links, but the binary CANNOT RUN"
        echo "      (its RUNPATH names only the glibc and gcc payloads)"
    else
        echo "  build a GL program (gcc -lGL): failed — $(printf '%s' "$out" | tail -1)"
    fi
}

# The tag differential: the same probe, built twice, differing only in whether
# its search path is DT_RPATH or DT_RUNPATH.
#
# This is not a style check. DT_RPATH on an executable is consulted for EVERY
# dlopen anywhere in the process; DT_RUNPATH is not consulted for the process
# at all. The graphics stack's load chain is three to four dlopens deep
# (glvnd -> vendor -> external platform module -> its own deps), so only the
# transitive tag reaches the bottom. Measured: identical path content, RPATH
# reaches the GPU on all four entry points, RUNPATH fails at eglInitialize.
#
# `elfpatch` stamps DT_RUNPATH (patchelf's --set-rpath default; --force-rpath
# is what writes DT_RPATH). One recipe in the whole index flips it by hand --
# godot -- and godot is the only installed program observed to render on the
# GPU. 1 binary with the right tag, 68 with the right PATH and the wrong tag.
#
# So this row answers "would a program built here see the GPU", which is a
# different question from every other row, and the one that decides whether
# the ecosystem's binaries work.
report_tag_differential() {
    local libdir="$1" nvdir="$2"
    local src="$HOME_DIR/.tagdiff.c"
    cp "$HERE/probe.c" "$src"
    local out
    out=$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos use "$SUBOS" --cmd       "gcc -O0 -o '$HOME_DIR/.tag-rpath'   '$src' -ldl -Wl,--disable-new-dtags,-rpath,$libdir:$nvdir &&
       gcc -O0 -o '$HOME_DIR/.tag-runpath' '$src' -ldl -Wl,-rpath,$libdir:$nvdir &&
       echo RPATH=\$(GFX_PROBE_LIBDIR=$libdir '$HOME_DIR/.tag-rpath' egl) &&
       echo RUNPATH=\$(GFX_PROBE_LIBDIR=$libdir '$HOME_DIR/.tag-runpath' egl)" 2>&1)
    local r u
    r=$(printf '%s' "$out" | sed -n 's/^RPATH=//p'   | cut -d"|" -f3- | cut -c1-40)
    u=$(printf '%s' "$out" | sed -n 's/^RUNPATH=//p' | cut -d"|" -f3- | cut -c1-40)
    echo "  tag differential (same paths, different tag):"
    printf '      DT_RPATH   -> %s\n' "${r:-(no result)}"
    printf '      DT_RUNPATH -> %s\n' "${u:-(no result)}"
    if [ -n "$r" ] && [ "$r" != "$u" ]; then
        echo "      ^ THE TAG DECIDES. Programs stamped DT_RUNPATH cannot reach"
        echo "        the GPU however correct their paths are (elfpatch stamps"
        echo "        DT_RUNPATH; only godot's recipe flips it by hand)."
    fi
}

run_env() {
    # $1 label, $2 runner ("host"|"subos"|"sandbox")
    local label="$1" runner="$2"
    echo "════════ $label ════════"

    # The probe binary must land somewhere BOTH sides can see. $WORK is under
    # /tmp, and the sandbox gives /tmp a fresh tmpfs — the compile succeeded
    # and the output went into a namespace that vanished with the shell, which
    # surfaced as a link error naming a path that "does not exist".
    local bin="$WORK/probe-$runner"
    [ "$runner" = "host" ] || bin="$HOME_DIR/.gfx-probe-$runner"
    local mesa_json_runner="$runner"
    local mesa_json
    local libdir=""
    # Build with -ldl only, and point the probe at the environment's lib
    # directory (GFX_PROBE_LIBDIR) instead of linking the GL libraries.
    #
    # Bare-name dlopen would be wrong here: nothing puts a subos's lib
    # directory on the loader's default search path, so a dlopen-only probe
    # reported "libEGL.so.1 not loadable" for every cell of a subos whose
    # libEGL works — an artifact of the probe printed in the same column as a
    # real failure. Linking `-lGL` would be the faithful alternative, and:
    #
    # MEASURED, and it is a product finding rather than a harness convenience:
    # `gcc -lGL` inside a subos does not link. The linker resolves libGL.so's
    # own DT_NEEDED (libGLdispatch.so.0, libGLX.so.0) neither from -L nor from
    # libGL's DT_RUNPATH, because RUNPATH is not transitive for ld any more
    # than it is for ld.so — the same property that broke the NVIDIA
    # interposer at runtime (#525), showing up in a second tool. Adding
    # -rpath-link makes it link, and the product then cannot RUN: the subos
    # toolchain writes a RUNPATH naming only the glibc and gcc payloads, so
    # the binary cannot find libGL.so.1 at startup. Filed separately; this
    # harness must not paper over it, so `report_build_defect` says so
    # explicitly instead of the table quietly working around it.
    case "$runner" in
        host)
            # /usr/bin/cc explicitly: `cc` on PATH may be an xlings toolchain,
            # and a statically-linked musl probe cannot dlopen anything — the
            # host baseline would come back empty and look like a host with no
            # graphics at all.
            /usr/bin/cc -O0 -o "$bin" "$HERE/probe.c" -ldl 2>"$WORK/cc.err" || {
                echo "  (cannot build the probe on the host: $(head -1 "$WORK/cc.err"))"; echo; return; }
            mesa_json="/usr/share/glvnd/egl_vendor.d/50_mesa.json"
            libdir=""     # the host resolves these by name, as it should
            ;;
        subos|sandbox|sandbox-gpu)
            # Built INSIDE, with the subos's own toolchain. A host-built probe
            # runs under the host loader and would resolve libraries this
            # environment does not actually provide — the measurement would be
            # of the host, wearing the subos's name.
            local sandbox_flag=""
            [ "$runner" = "sandbox" ]     && sandbox_flag="--sandbox"
            # --gpu is not a nicety: bwrap's --dev builds a fresh /dev with a
            # hard-coded whitelist that excludes /dev/nvidia* and /dev/dri/*,
            # so without it the sandbox is a software-rendering environment by
            # construction. Measuring only the flagless case would report the
            # sandbox as broken when it is merely unflagged.
            [ "$runner" = "sandbox-gpu" ] && sandbox_flag="--sandbox --gpu"
            # The source must live somewhere the sandbox can see. bwrap
            # rebuilds the mount namespace, so a path under the repo simply
            # does not exist in there and the compile fails naming the .c file.
            local src="$HOME_DIR/.gfx-probe.c"
            cp "$HERE/probe.c" "$src"
            XLINGS_HOME="$HOME_DIR" "$XBIN" subos use "$SUBOS" $sandbox_flag --cmd \
                "gcc -O0 -o '$bin' '$src' -ldl" \
                >"$WORK/cc.err" 2>&1 || {
                echo "  (cannot build the probe in this environment:)"
                sed 's/^/    /' "$WORK/cc.err" | tail -3; echo; return; }
            local sdir
            sdir="$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos info "$SUBOS" 2>/dev/null \
                    | awk '/^ *dir /{print $2; exit}')"
            mesa_json="$sdir/share/glvnd/egl_vendor.d/50_mesa.json"
            libdir="$sdir/lib"
            report_build_defect "$sdir"
            report_tag_differential "$sdir/lib" \
              "$(ls -d "$HOME_DIR"/data/xpkgs/xim-x-nvidia-gl-host-link/*/lib 2>/dev/null | head -1)"
            ;;
    esac

    local libenv=""
    [ -n "$libdir" ] && libenv="GFX_PROBE_LIBDIR=$libdir"
    printf '  %-22s %-30s %-30s %s\n' PROBE OVERRIDES RENDERER FAMILY
    for cell in "${CELLS[@]}"; do
        IFS='|' read -r probe overrides _note <<<"$cell"
        overrides="${overrides//\{MESA_EGL_JSON\}/$mesa_json}"
        local envprefix=""
        if [ -n "$overrides" ]; then
            # `DISPLAY=` (empty) means UNSET, not empty-string: an empty
            # DISPLAY is a valid-looking value that XOpenDisplay treats
            # differently from absent, and the offline cells are about absent.
            local IFS_SAVE="$IFS"; IFS=';'
            for kv in $overrides; do
                case "$kv" in
                    *=) envprefix="$envprefix env -u ${kv%=}" ;;
                    *)  envprefix="$envprefix $kv" ;;
                esac
            done
            IFS="$IFS_SAVE"
        fi

        # Filter to the probe's own line in every environment. Drivers write
        # unsolicited warnings to stderr (`libEGL warning: pci id for fd 3 …`),
        # and a `head -1` puts that warning in the RENDERER column — the cell
        # then reads as an unrecognised renderer instead of the result it got.
        local out=""
        case "$runner" in
            host)
                out=$(eval "$libenv $envprefix '$bin' '$probe'" 2>&1 | grep -E "^$probe\|" | head -1) ;;
            subos)
                out=$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos use "$SUBOS" --cmd \
                      "$libenv $envprefix '$bin' '$probe'" 2>&1 | grep -E "^$probe\|" | head -1) ;;
            sandbox)
                out=$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos use "$SUBOS" --sandbox --cmd \
                      "$libenv $envprefix '$bin' '$probe'" 2>&1 | grep -E "^$probe\|" | head -1) ;;
            sandbox-gpu)
                out=$(XLINGS_HOME="$HOME_DIR" "$XBIN" subos use "$SUBOS" --sandbox --gpu --cmd \
                      "$libenv $envprefix '$bin' '$probe'" 2>&1 | grep -E "^$probe\|" | head -1) ;;
        esac

        local renderer family
        if [ -z "$out" ]; then
            renderer="(no output)"; family="UNMEASURED"
        else
            renderer=$(cut -d'|' -f3- <<<"$out")
            if [ "$(cut -d'|' -f2 <<<"$out")" = "ERROR" ]; then
                family="no context"
            else
                family=$(classify "$renderer")
            fi
        fi
        printf '  %-22s %-30s %-30.30s %s\n' \
               "$probe" "${overrides:--}" "$renderer" "$family"
    done
    echo
}

[ "$WITH_HOST" -eq 1 ] && run_env "HOST (no xlings — the baseline to beat)" host
run_env "SUBOS $SUBOS (xlings stack, no sandbox)" subos
run_env "SUBOS $SUBOS --sandbox (bwrap: fresh /dev and mounts)" sandbox
run_env "SUBOS $SUBOS --sandbox --gpu (device nodes re-exposed)" sandbox-gpu

cat <<'NOTE'
Reading this table
  • Compare DOWN a column across environments: a row that is "GPU (nvidia)" on
    the host and "CPU (software)" in the subos is a silent regression — both
    rendered, both exited 0.
  • "no context" is a HONEST failure: the cell said it could not get there.
  • "UNMEASURED" means the probe produced nothing at all; that is a broken
    measurement, not a result, and must be chased before reading the row.
  • GLX with DISPLAY unset is EXPECTED to be "no context". GLX has no headless
    mode; that cell exists to prove the offline story runs through EGL.
NOTE
