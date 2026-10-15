#!/usr/bin/env bash
#
# Fresh-install smoke suites — Linux, CentOS 7, macOS.
#
# Verifies RELEASED xlings (floating latest, exactly what a new user gets) on a
# machine with no xlings state, from `quick_install` through the mainstream
# feature set. It does NOT test the code in the working tree — see
# xlings-ci-linux*.yml for that.
#
# The script bootstraps xlings itself rather than relying on the caller, so the
# native Linux leg and the CentOS 7 `docker run` leg execute byte-identical code
# and neither needs $GITHUB_PATH.
#
#   usage: bash tests/fresh-install/smoke.sh <core|gcc|llvm|luban-console|luban-window|luban-archive|luban-machine|luban-upgrade|luban-lived-in|luban-host>
#
# Run it locally the same way CI does — but note it uninstalls xlings from $HOME
# at the end of the `core` suite, so point HOME somewhere disposable:
#
#   HOME=$(mktemp -d) bash tests/fresh-install/smoke.sh core

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/fresh-install/lib.sh
. "$HERE/lib.sh"
# shellcheck source=tests/fresh-install/luban.sh
. "$HERE/luban.sh"

# ── Versions under test ───────────────────────────────────────────────
# Pinned rather than `latest` so an assertion can name an exact expected
# string. Two distinct versions per package, because the switch assertions are
# differential: one version alone would pass even if `use` did nothing.
# Overridable so a failure can be re-run against other versions locally.
QUICK_INSTALL_URL="${QUICK_INSTALL_URL:-https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh}"
NINJA_VERSION="${NINJA_VERSION:-1.12.1}"
MCPP_OLD="${MCPP_OLD:-2026.7.28.2}"
MCPP_NEW="${MCPP_NEW:-2026.7.29.1}"
GCC_OLD="${GCC_OLD:-15.1.0}"
GCC_NEW="${GCC_NEW:-16.1.0}"
LLVM_OLD="${LLVM_OLD:-20.1.7}"
LLVM_NEW="${LLVM_NEW:-22.1.8}"

XLINGS_HOME_DIR="$HOME/.xlings"

# quick_install.sh reads from /dev/tty when its stdin is a pipe, which would
# hang a local run outside CI. Set it here rather than only in the workflow so
# `bash smoke.sh` behaves identically in both places.
export XLINGS_NON_INTERACTIVE=1

# ── Phase 0: bootstrap ────────────────────────────────────────────────

bootstrap() {
    section "Bootstrap — quick_install on a cold machine"

    if [ -e "$XLINGS_HOME_DIR" ]; then
        fail "not a fresh environment: $XLINGS_HOME_DIR already exists"
    fi

    # No version argument: resolve to the newest release, which is the whole
    # point — this is the command printed in the README.
    # The upgrade leg starts one release back: the newest but one, asked of
    # GitHub now -- never a number written here.
    local from=()
    if [ "${FROM_PREVIOUS_RELEASE:-0}" = 1 ]; then
        local previous_release
        # Authenticated where CI gives a token: the anonymous API limit is a
        # runner IP's, shared, and answers 403 when spent.
        local auth=()
        [ -n "${GH_TOKEN:-}" ] && auth=(-H "Authorization: Bearer $GH_TOKEN")
        previous_release="$(curl -fsSL --retry 3 ${auth[@]+"${auth[@]}"} "https://api.github.com/repos/openxlings/xlings/releases?per_page=2" \
            | python3 -c 'import json,sys; print(json.load(sys.stdin)[1]["tag_name"])')" \
            || fail "cannot ask GitHub for the release before the newest"
        from=(-s -- "$previous_release")
        log "starting from the release before the newest: $previous_release"
    fi
    log "\$ curl -fsSL $QUICK_INSTALL_URL | bash ${from[*]+${from[*]}}"
    curl -fsSL "$QUICK_INSTALL_URL" | bash ${from[@]+"${from[@]}"} \
        || fail "quick_install.sh exited $? — the published release is unusable"

    [ -x "$XLINGS_HOME_DIR/subos/current/bin/xlings" ] \
        || fail "quick_install left no xlings shim at $XLINGS_HOME_DIR/subos/current/bin/xlings"

    # The installer writes shell-profile hooks, but this process was started
    # before they existed; put the same directories on PATH by hand.
    export PATH="$XLINGS_HOME_DIR/subos/current/bin:$XLINGS_HOME_DIR/bin:$PATH"

    command -v xlings >/dev/null || fail "xlings is not on PATH after quick_install"
    ok "xlings $(tool_version xlings) installed at $XLINGS_HOME_DIR"

    # Runners reach github.com but not the gitee/gitcode endpoints, so force
    # GLOBAL rather than letting region detection guess.
    run "config mirror"  xlings config --mirror GLOBAL
    # The release tarball carries an index snapshot frozen at build time;
    # refresh it so recently added package versions resolve.
    run "index update"   xlings update

    overlay_index
}

# overlay_index — swap in a candidate xim-pkgindex, if one was requested.
#
# Recipe bugs are only ever caught on a real install, and a recipe fix
# therefore cannot be validated until it is already published. This lets an
# unmerged xim-pkgindex branch be run through the whole suite, on every
# platform, before it lands.
#
# Only `pkgs/` is replaced: the rest of the index directory is metadata the
# fetch wrote (cache manifests, sub-index state) and swapping that out would
# test a state no user is ever in. The directory is replaced in place rather
# than symlinked, because a symlinked index turns later differentials into
# unfalsifiable passes.
overlay_index() {
    [ -n "${XIM_PKGINDEX_REF:-}" ] || return 0

    section "Index override — xim-pkgindex@$XIM_PKGINDEX_REF"
    local dst="$XLINGS_HOME_DIR/data/xim-pkgindex"
    [ -d "$dst/pkgs" ] || fail "no published index at $dst/pkgs to override"

    local tmp; tmp="$(mktemp -d)"
    git clone -q --depth 1 --branch "$XIM_PKGINDEX_REF" \
        https://github.com/openxlings/xim-pkgindex.git "$tmp/idx" \
        || fail "cannot clone xim-pkgindex@$XIM_PKGINDEX_REF"

    rm -rf "$dst/pkgs"
    cp -R "$tmp/idx/pkgs" "$dst/pkgs" || fail "failed to overlay pkgs/"
    # The catalog is the published index's, keyed on its version: kept, it
    # does not know a package the branch adds, and the miss refreshes the
    # index -- replacing these pkgs/ with the published ones again. Rebuilt.
    rm -f "$dst/.xlings-index-cache.json"
    ok "pkgs/ replaced from xim-pkgindex@$XIM_PKGINDEX_REF ($(git -C "$tmp/idx" rev-parse --short HEAD))"
}

# ── core: lifecycle + multi-version switch on mcpp ────────────────────

suite_core() {
    section "Discovery — search on a cold index"
    # `tr -d '\0'`: the TUI writes null bytes, and command substitution warns
    # about (and drops) them, which would spray noise across the CI log.
    local out
    out="$(xlings search mcpp 2>&1 | tr -d '\0')" || fail "xlings search mcpp exited $?"
    case "$out" in
        *mcpp*) ok "search found mcpp" ;;
        *) fail "xlings search mcpp did not mention mcpp:
$out" ;;
    esac

    section "Install and run — ninja"
    # ninja is the install-and-run probe: tiny, statically linked, and present
    # on every platform in the matrix, so a failure here is xlings's, not the
    # package's.
    run "install ninja" xlings install "ninja@$NINJA_VERSION" -y -g
    assert_tool_version ninja "$NINJA_VERSION"

    section "Multi-version switch — mcpp"
    run "install mcpp $MCPP_OLD" xlings install "mcpp@$MCPP_OLD" -y -g
    run "install mcpp $MCPP_NEW" xlings install "mcpp@$MCPP_NEW" -y -g
    # Two switches, two distinct versions: a `use` that silently no-ops fails
    # one of them no matter which version happened to be active.
    assert_switch mcpp "$MCPP_OLD" mcpp
    assert_switch mcpp "$MCPP_NEW" mcpp

    section "Inventory — list"
    out="$(xlings list 2>&1 | tr -d '\0')" || fail "xlings list exited $?"
    case "$out" in *ninja*) ;; *) fail "xlings list omits ninja:
$out" ;; esac
    case "$out" in *mcpp*) ;; *) fail "xlings list omits mcpp:
$out" ;; esac
    ok "list reports ninja and mcpp"

    section "Project mode — .xlings.json workspace"
    local proj; proj="$(mktemp -d)"
    cat > "$proj/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "workspace": {
    "ninja": "$NINJA_VERSION"
  }
}
EOF
    ( cd "$proj" && run "project install" xlings install -y )

    local shim="$proj/.xlings/subos/_/bin/ninja"
    [ -x "$shim" ] || fail "project install produced no project-local shim at $shim"
    local got; got="$(extract_version "$("$shim" --version 2>&1)")"
    [ "$got" = "$NINJA_VERSION" ] \
        || fail "project-local ninja shim reports $got, expected $NINJA_VERSION"
    ok "project-local shim resolves ninja $got"

    section "Routing survives a project-scope install"
    # Named, so this cell fails with a CAUSE rather than with a generic
    # doctor exit code. Every `fresh (core)` cell was red from 2026-09-03 to
    # 2026-09-20 because a project-scope install rebuilt the GLOBAL shim table
    # from a workspace it had read out of the project's subos -- empty -- and
    # removed everything the project did not itself declare (#582, #604).
    # `ninja` survived because the project declares it; `mcpp` did not.
    #
    # `self doctor` below does catch it, but only as "shim table: N missing",
    # which is one indirection away from what happened and was read for weeks
    # as a doctor problem rather than an install one.
    # No `.exe` branch: this script is the POSIX suite (Linux / CentOS 7 /
    # macOS). Windows runs smoke.ps1, which carries the same assertion.
    local mcpp_shim="$XLINGS_HOME_DIR/subos/default/bin/mcpp"
    if [ ! -e "$mcpp_shim" ]; then
        fail "the project install removed the globally-installed mcpp shim:
  expected: $mcpp_shim
  bin now:  $(ls "$XLINGS_HOME_DIR/subos/default/bin" 2>&1 | tr '\n' ' ')"
    fi
    ok "globally-installed shims survived the project install"

    section "Self-management — doctor"
    run "self doctor" xlings self doctor

    section "Teardown — self uninstall"
    run "self uninstall" xlings self uninstall -y
    if [ -d "$XLINGS_HOME_DIR" ]; then
        fail "self uninstall left $XLINGS_HOME_DIR behind"
    fi
    ok "self uninstall removed $XLINGS_HOME_DIR"
}

# ── gcc: release-group switch ─────────────────────────────────────────

suite_gcc() {
    case "$(uname -s)" in
        Linux) ;;
        *) fail "the gcc suite is Linux-only — the package index ships no gcc for $(uname -s)" ;;
    esac

    section "Install two gcc versions"
    run "install gcc $GCC_OLD" xlings install "gcc@$GCC_OLD" -y -g
    run "install gcc $GCC_NEW" xlings install "gcc@$GCC_NEW" -y -g

    section "Release-group switch — gcc/g++/c++/cpp must move together"
    # gcc registers its drivers as one xvm group. Asserting only `gcc` here
    # would pass while `g++` stayed on the other version — the exact failure
    # this suite exists to catch.
    assert_switch gcc "$GCC_OLD" gcc g++ c++ cpp
    assert_switch gcc "$GCC_NEW" gcc g++ c++ cpp

    section "The switched toolchain compiles and runs"
    local work; work="$(mktemp -d)"
    cat > "$work/hello.cpp" <<'EOF'
#include <iostream>
#include <string>
#include <vector>
int main() {
    std::vector<std::string> parts{"fresh", "install", "ok"};
    for (const auto& p : parts) std::cout << p << ' ';
    std::cout << '\n';
}
EOF
    ( cd "$work" && run "g++ compile" g++ -std=c++20 hello.cpp -o hello )
    assert_runs "g++ $GCC_NEW binary" "$work/hello" "fresh install ok"

    # Switch back and rebuild: proves the switch is real for compilation, not
    # just for what --version prints.
    assert_switch gcc "$GCC_OLD" gcc g++ c++ cpp
    ( cd "$work" && run "g++ recompile" g++ -std=c++20 hello.cpp -o hello_old )
    assert_runs "g++ $GCC_OLD binary" "$work/hello_old" "fresh install ok"
}

# ── llvm: version switch ──────────────────────────────────────────────

suite_llvm() {
    section "Install two llvm versions"
    run "install llvm $LLVM_OLD" xlings install "llvm@$LLVM_OLD" -y -g
    run "install llvm $LLVM_NEW" xlings install "llvm@$LLVM_NEW" -y -g

    section "Version switch — clang/clang++ must move together"
    assert_switch llvm "$LLVM_OLD" clang clang++
    assert_switch llvm "$LLVM_NEW" clang clang++

    section "The switched toolchain compiles and runs"
    # C, not C++, on purpose: this asserts the compiler driver and the bundled
    # libc work end to end without also betting on libc++ wiring, which is a
    # separate concern from the version switch under test here.
    local work; work="$(mktemp -d)"
    cat > "$work/hello.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("fresh install ok\n"); return 0; }
EOF
    ( cd "$work" && run "clang compile" clang hello.c -o hello )
    assert_runs "clang $LLVM_NEW binary" "$work/hello" "fresh install ok"
}

# ── luban: Luban OS as a first-time user meets it (Linux) ─────────────

# luban_os <script> <screenshot-dir> [args...] -- the window, headless, driven
# by a script of a person's input (luban-os --script; tests/README.md).
# A script that fails leaves the window's log and every machine's console
# and supervisor logs beside its screenshots (the job's artifact), and the
# end of each console here: a machine that never shows its desktop is only
# explained by what it printed.
luban_os() {
    local script="$1"; shift
    local rc=0
    SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=software \
        timeout 1800 "$LUBAN_OS" --headless --script "$script" "$@" || rc=$?
    if [ "$rc" -ne 0 ]; then
        local logs="${XLINGS_HOME_DIR:-$HOME/.xlings}/logs" out
        out="$(dirname "$script")/logs"
        mkdir -p "$out"
        cp "$logs/luban-os.log" "$out/" 2>/dev/null || true
        cp -r "$logs/machines" "$out/" 2>/dev/null || true
        local console
        for console in "$out"/machines/*/console.log; do
            [ -f "$console" ] || continue
            log "the end of $(basename "$(dirname "$console")")'s console:"
            tail -n 40 "$console" | tr -d '\r' | sed 's/^/     /'
        done
    fi
    return "$rc"
}

# pixel <ppm> <x> <y> -- "r g b" of a screenshot's pixel.
pixel() {
    python3 - "$@" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
w = int(parts[1].split()[0])
x, y = int(sys.argv[2]), int(sys.argv[3])
o = len(data) - len(parts[3]) + (y * w + x) * 3
print(data[o], data[o + 1], data[o + 2])
PY
}

suite_luban_window() {
    [ "$(uname -s)" = Linux ] || fail "the luban suite is Linux's: Luban systems are Linux systems"
    local shots="${LUBAN_SCREENSHOTS:-$PWD/luban-os-fresh}"
    mkdir -p "$shots"

    section "Install the Luban OS window, as a person does"
    run "install luban-os" xlings install luban-os -y -g
    LUBAN_OS="$(command -v luban-os)" || fail "luban-os is not on PATH after its install"
    ok "luban-os at $LUBAN_OS"
    # The one-time setup a restricted host asks for (docs/luban/07-host-support.md).
    run "luban setup" luban setup -y
    # Installed where xlings puts programs, not beside luban: luban finds it.
    STATUS="$(luban status 2>&1)" || true
    grep -Eq 'desktop +ready' <<<"$STATUS" || fail "the window is installed and luban status says no desktop: $STATUS"
    ok "luban status: a desktop can be shown"

    section "First use -- the quick start makes a system and its console runs what is typed"
    cat > "$shots/first.script" <<EOF
wait quick-start
frames 2
screenshot $shots/1-quick-start.bmp
click card:tiny
click make
wait job
wait console 900
see 120s root@tiny-1
type echo luban-\$((6*7))
key Return
see 60s luban-42
screenshot $shots/2-console.bmp
EOF
    luban_os "$shots/first.script" || fail "the first use did not go through (screenshots in $shots)"
    ok "made tiny-1 from the quick start; its console ran echo"

    section "A desktop -- the desktop edition in a sandbox shows xdesk"
    run "make the desktop edition" luban new desk --edition desktop -y
    run "run it in a sandbox" luban config desk --mode sandbox
    cat > "$shots/desktop.script" <<EOF
wait launcher
click row:desk
wait desktop 600
waitfor desktop.texture.0 1100 600
sleep 8000
frames 2
screenshot $shots/3-desktop.ppm
screenshot $shots/3-desktop.bmp
EOF
    luban_os "$shots/desktop.script" || fail "the desktop did not show (screenshots in $shots)"
    local bg panel
    bg="$(pixel "$shots/3-desktop.ppm" 550 400)"
    panel="$(pixel "$shots/3-desktop.ppm" 550 12)"
    [ "$panel" != "$bg" ] || fail "no panel across the top of the desktop (pixel $panel, background $bg)"
    ok "xdesk shows its panel (top $panel, background $bg)"

    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        section "The desktop edition as a machine (KVM)"
        run "make a desktop machine" luban new deskm --edition desktop -y
        # A first start installs what this host lacks (its machine monitor, a
        # package of the index) as one of its steps, and the desktop shows.
        cat > "$shots/machine.script" <<EOF
wait launcher
click row:deskm
see 300s installing the virtual machine monitor
wait desktop 420
waitfor desktop.texture.0 1100 120
sleep 8000
frames 2
screenshot $shots/4-machine.ppm
screenshot $shots/4-machine.bmp
EOF
        luban_os "$shots/machine.script" || fail "the machine's desktop did not show (screenshots in $shots)"
        bg="$(pixel "$shots/4-machine.ppm" 550 400)"
        panel="$(pixel "$shots/4-machine.ppm" 550 12)"
        [ "$panel" != "$bg" ] || fail "no panel on the machine's desktop"
        ok "the machine shows xdesk"
    else
        log "no usable /dev/kvm here: the machine leg is skipped"
    fi
}

# ── entry point ───────────────────────────────────────────────────────

main() {
    local suite="${1:-}"
    case "$suite" in
        core|gcc|llvm) ;;
        luban|luban-window) suite=luban-window ;;
        luban-console|luban-archive|luban-machine|luban-host) ;;
        luban-upgrade|luban-lived-in) export FROM_PREVIOUS_RELEASE=1 ;;
        '') fail "usage: $0 <core|gcc|llvm|luban-console|luban-window|luban-archive|luban-machine|luban-upgrade|luban-lived-in|luban-host>" ;;
        *)  fail "unknown suite '$suite'" ;;
    esac

    printf '%s┌─ fresh-install: %s suite on %s %s%s\n' \
        "$YLW" "$suite" "$(uname -s)" "$(uname -m)" "$RST"

    bootstrap
    "suite_${suite//-/_}"

    section "PASS — $suite suite"
}

main "$@"
