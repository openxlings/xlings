# shellcheck shell=bash
#
# Assertion helpers for the fresh-install smoke suites.
#
# Sourced by smoke.sh. Kept separate so the assertions — especially the
# release-group check, which is the whole point of the gcc suite — can be read
# without wading through the suite bodies.

RED=''; GRN=''; YLW=''; DIM=''; RST=''
if [ -t 1 ]; then
    RED=$'\033[31m'; GRN=$'\033[32m'; YLW=$'\033[33m'
    DIM=$'\033[2m';  RST=$'\033[0m'
fi

section() { printf '\n%s══ %s%s\n' "$YLW" "$*" "$RST"; }
log()     { printf '   %s%s%s\n' "$DIM" "$*" "$RST"; }
ok()      { printf '   %sOK%s  %s\n' "$GRN" "$RST" "$*"; }
fail()    { printf '\n%sFAIL%s %s\n' "$RED" "$RST" "$*" >&2; exit 1; }

# run <description> <cmd...> — echo the command, then run it, failing loudly.
run() {
    local desc="$1"; shift
    log "\$ $*"
    "$@" || fail "$desc: '$*' exited $?"
}

# extract_version <text> — first dotted numeric run in <text>.
#
# Handles every --version shape in the matrix without per-tool parsing:
#   "gcc (GCC) 16.1.0"        → 16.1.0
#   "clang version 20.1.7"    → 20.1.7
#   "1.12.1"                  → 1.12.1
#   "mcpp 2026.7.29.1"        → 2026.7.29.1
#
# No `head -1` on purpose: under `set -o pipefail` a SIGPIPE'd grep would fail
# the whole script. The first line is taken with parameter expansion instead.
extract_version() {
    local all
    all="$(printf '%s\n' "$1" | grep -oE '[0-9]+(\.[0-9]+)+' || true)"
    printf '%s' "${all%%
*}"
}

# tool_version <cmd> — the version <cmd> reports, or fail with its raw output.
tool_version() {
    local cmd="$1" out rc=0 ver
    command -v "$cmd" >/dev/null 2>&1 || fail "'$cmd' is not on PATH"
    out="$("$cmd" --version 2>&1)" || rc=$?
    [ "$rc" -eq 0 ] || fail "'$cmd --version' exited $rc:
$out"
    ver="$(extract_version "$out")"
    [ -n "$ver" ] || fail "'$cmd --version' printed no version number:
$out"
    printf '%s' "$ver"
}

# assert_tool_version <cmd> <expected>
assert_tool_version() {
    local cmd="$1" want="$2" got
    got="$(tool_version "$cmd")"
    [ "$got" = "$want" ] || fail "$cmd reports $got, expected $want"
    ok "$cmd → $got"
}

# assert_switch <pkg> <version> <cmd> [cmd...]
#
# Switch to <pkg>@<version>, then assert EVERY listed command reports exactly
# that version. For a release group (gcc ships gcc/g++/c++/cpp/... as one unit)
# this is the real assertion: a switch that moves `gcc` but strands `g++` on the
# previous version passes any check that only looks at `gcc --version`.
#
# Every member is probed before failing, so the error names all stranded
# members at once instead of stopping at the first.
#
# Callers invoke this twice with two DIFFERENT versions. That is what makes it
# differential: a `use` that silently no-ops cannot satisfy both calls, whereas
# a single call could pass merely because the version was already active.
assert_switch() {
    local pkg="$1" ver="$2"; shift 2
    run "xlings use $pkg@$ver" xlings use "$pkg@$ver"

    local cmd got stranded=''
    for cmd in "$@"; do
        got="$(tool_version "$cmd")"
        if [ "$got" = "$ver" ]; then
            log "  $cmd → $got"
        else
            stranded="$stranded
     $cmd reports $got (expected $ver)"
        fi
    done

    if [ -n "$stranded" ]; then
        fail "$pkg@$ver: release group did not switch as a unit:$stranded"
    fi
    ok "$pkg@$ver — [$*] switched as a unit"
}

# assert_runs <description> <file> <expected-stdout-substring> — execute a
# freshly compiled binary and check it actually produced output.
assert_runs() {
    local desc="$1" bin="$2" want="$3" out
    [ -x "$bin" ] || fail "$desc: '$bin' was not produced or is not executable"
    out="$("$bin")" || fail "$desc: '$bin' exited $?"
    case "$out" in
        *"$want"*) ok "$desc → $out" ;;
        *) fail "$desc: '$bin' printed '$out', expected it to contain '$want'" ;;
    esac
}
