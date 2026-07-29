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
#   usage: bash tests/fresh-install/smoke.sh <core|gcc|llvm>
#
# Run it locally the same way CI does — but note it uninstalls xlings from $HOME
# at the end of the `core` suite, so point HOME somewhere disposable:
#
#   HOME=$(mktemp -d) bash tests/fresh-install/smoke.sh core

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/fresh-install/lib.sh
. "$HERE/lib.sh"

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
    log "\$ curl -fsSL $QUICK_INSTALL_URL | bash"
    curl -fsSL "$QUICK_INSTALL_URL" | bash \
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

# ── entry point ───────────────────────────────────────────────────────

main() {
    local suite="${1:-}"
    case "$suite" in
        core|gcc|llvm) ;;
        '') fail "usage: $0 <core|gcc|llvm>" ;;
        *)  fail "unknown suite '$suite' — expected core, gcc or llvm" ;;
    esac

    printf '%s┌─ fresh-install: %s suite on %s %s%s\n' \
        "$YLW" "$suite" "$(uname -s)" "$(uname -m)" "$RST"

    bootstrap
    "suite_$suite"

    section "PASS — $suite suite"
}

main "$@"
