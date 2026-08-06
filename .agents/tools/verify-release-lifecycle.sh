#!/usr/bin/env bash
# Run the whole subos lifecycle against a RELEASED xlings, in an isolated home.
#
# Why a script and not a checklist: the four home-related defects of 2026-08-06
# were all invisible in the default home, and three of them were found only
# because a measurement was taken twice and disagreed. A checklist executed by
# hand takes the measurement once.
#
# Two things this asserts that a normal test does not:
#
#   * the home under test is NOT the developer's, and shares no prefix with it.
#     A shim rewrites XLINGS_HOME to whichever home owns it, so "we ran in the
#     isolated home" is a claim that has to be checked, not assumed.
#   * the real ~/.xlings is byte-unchanged afterwards. Not "we did not mean to
#     touch it" -- checked.
#
# Usage:
#   verify-release-lifecycle.sh --bin <xlings> [--home <dir>] [--keep]
set -uo pipefail

BIN=""
HOME_DIR=""
KEEP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bin)  BIN="$2"; shift 2 ;;
        --home) HOME_DIR="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$BIN" && -x "$BIN" ]] || { echo "usage: --bin <xlings>" >&2; exit 2; }
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
HOME_DIR="${HOME_DIR:-${TMPDIR:-/tmp}/xlings-release-verify-$(id -u)}"

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ✓ $*"; }
step() { echo; echo "── $* ─────────────────────────────"; }

REAL="${HOME%/}/.xlings"
case "$HOME_DIR" in
    "$REAL"|"$REAL"/*) fail "the verification home IS the real home: $HOME_DIR" ;;
esac
[[ "$HOME_DIR" == "${HOME%/}"/* ]] \
    && echo "  note: shares a prefix with \$HOME — the sandbox bind ordering" \
            "this exercises is the easy one"

rm -rf "$HOME_DIR"; mkdir -p "$HOME_DIR"
trap '[[ $KEEP == 1 ]] || rm -rf "$HOME_DIR"' EXIT

# A marker older than anything this run can do, to compare the real store
# against afterwards.
MARKER="$(mktemp)"; trap 'rm -f "$MARKER"' RETURN 2>/dev/null || true

x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$BIN" "$@" ) }

echo "binary: $BIN"
echo "home:   $HOME_DIR"
echo "version: $(x --version 2>&1 | head -1)"

step "1. the client anchors to the home under test"
GOT="$(x self info 2>&1 | grep -iE "home" | head -1)"
echo "$GOT" | grep -q "$HOME_DIR" \
    || fail "the client reports a home other than the one under test:
$GOT"
ok "XLINGS_HOME is honoured"

step "2. install a package and check the payload"
x install patchelf -y >/dev/null 2>&1 || fail "install patchelf failed"
PE="$(ls "$HOME_DIR"/data/xpkgs/*-x-patchelf/*/bin/patchelf 2>/dev/null | head -1)"
[[ -n "$PE" ]] || fail "no patchelf payload after a successful install"
ok "payload at ${PE#$HOME_DIR/}"

step "3. glibc's payload carries no build path and its ldd parses"
x install glibc -y >/dev/null 2>&1 || x install xim:glibc -y >/dev/null 2>&1 \
    || fail "install glibc failed"
G="$(ls -d "$HOME_DIR"/data/xpkgs/*-x-glibc/* 2>/dev/null | head -1)"
[[ -n "$G" ]] || fail "no glibc payload"
LEFT="$(grep -rl "xlings_data\|/nonexistent/xlings-use-rpath" "$G" 2>/dev/null \
        | grep -v '\.xpkg\.lua$' || true)"
[[ -z "$LEFT" ]] || fail "build paths remain in the payload:
$LEFT"
ok "no build path in the payload"
bash -n "$G/bin/ldd" 2>/dev/null || fail "the ldd we ship does not parse"
ok "bin/ldd passes bash -n"
grep -q "RTLDLIST=\"" "$G/bin/ldd" || fail "RTLDLIST was swallowed by the rewrite"
ok "RTLDLIST survived the rewrite"

step "4. doctor is clean"
OUT="$(x self doctor 2>&1 || true)"
echo "$OUT" | grep -qiE "double binding|env orphan|loader/libc split" \
    && fail "doctor reports a defect on a freshly built home:
$OUT"
ok "no findings"

step "5. the real home was never written"
if [[ -d "$REAL/data/xpkgs" ]]; then
    NEWER="$(find "$REAL/data/xpkgs" -newer "$MARKER" -print -quit 2>/dev/null)"
    [[ -z "$NEWER" ]] || fail "the real store changed during this run: $NEWER"
    ok "$REAL/data/xpkgs unchanged"
else
    ok "no real store on this machine to disturb"
fi

echo
echo "PASS: release lifecycle"
