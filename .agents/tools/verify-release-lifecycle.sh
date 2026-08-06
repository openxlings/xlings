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
x self init >/dev/null 2>&1 || true

# The mirror, before anything downloads.
#
# A fresh home defaults to GLOBAL, and on a CN-routed machine that is the
# difference between a two-minute run and an hour of stalls -- the first
# attempt at this script sat at 140 MB for over an hour and I misread it as a
# hung process. The default here follows the DEVELOPER's own home rather than
# a hardcoded region: whatever route works for them works for the verification.
if [[ -z "${XLINGS_VERIFY_MIRROR:-}" ]] && [[ -f "$REAL/.xlings.json" ]]; then
    XLINGS_VERIFY_MIRROR="$(sed -n 's/.*"mirror"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
                            "$REAL/.xlings.json" | head -1)"
fi
XLINGS_VERIFY_MIRROR="${XLINGS_VERIFY_MIRROR:-GLOBAL}"
x config --mirror "$XLINGS_VERIFY_MIRROR" >/dev/null 2>&1 || true
echo "  mirror: $XLINGS_VERIFY_MIRROR"
GOT="$(x self config 2>&1)"
echo "$GOT" | grep -q "$HOME_DIR" \
    || fail "the client reports a home other than the one under test.
That is the failure this whole script exists to make visible: a shim rewrites
XLINGS_HOME to whichever home owns it, so every measurement taken afterwards
would describe the developer's home while looking exactly like a measurement of
this one.
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
# TEXT files only, and that distinction is the point rather than a shortcut.
#
# ld.so has the build prefix compiled into it as its default library search
# path, and after AD-11 that is `/nonexistent/xlings-use-rpath-not-default-
# search` -- deliberately a path that cannot exist, so that everything must
# come from DT_RPATH and an unpatched binary fails loudly instead of quietly
# picking up the host's loader. A marker inside a binary is the design; the
# same marker inside a shell script or a .pc file is the defect.
#
# `.xpkg.lua` is the recipe copied in as a record, and it mentions the marker
# in prose.
LEFT="$(grep -rlI "xlings_data" "$G" 2>/dev/null \
        | grep -v '\.xpkg\.lua$' || true)"
if [[ -n "$LEFT" ]]; then
    echo "$LEFT" | head -10 >&2
    fail "$(echo "$LEFT" | wc -l) text file(s) in the payload still name the build machine"
fi
ok "no build path in any text file"
bash -n "$G/bin/ldd" 2>/dev/null || fail "the ldd we ship does not parse"
ok "bin/ldd passes bash -n"
grep -q "RTLDLIST=\"" "$G/bin/ldd" || fail "RTLDLIST was swallowed by the rewrite"
ok "RTLDLIST survived the rewrite"

step "4. a subos of its own, entered, with its environment"
x subos new relverify >/dev/null 2>&1 || fail "subos new failed"
x subos use relverify >/dev/null 2>&1 || true

# The graphics stack is the real consumer of subos.env, but it is large and not
# present on every machine. glibc is: it registers with xvm, so `subos use`
# has something to resolve and PATH has something to carry.
OUT="$(x subos use relverify --cmd 'echo "IN=[$XLINGS_HOME]"; echo "P0=[${PATH%%:*}]"' 2>&1)"
echo "$OUT" | grep -q "IN=\[$HOME_DIR\]" \
    || fail "inside the subos, XLINGS_HOME is not the home under test.
A session that re-anchors to another home makes every measurement taken inside
it a measurement of the wrong home, while looking exactly like the right one:
$OUT"
ok "subos session anchors to the home under test"

echo "$OUT" | grep -q "P0=\[$HOME_DIR/subos/relverify/bin\]" \
    || fail "PATH[0] inside the subos is not this subos's bin:
$OUT"
ok "PATH[0] is this subos's bin"

step "5. one package, one live version"
# Two versions of glibc exist in the index. Installing both must leave the
# environment naming exactly one -- the contract E2E-64 states, checked here
# against a released binary and the real index rather than a fixture.
x install xim:glibc@2.44 -y >/dev/null 2>&1 || true
MAN="$HOME_DIR/subos/relverify/.xlings.json"
if [[ -f "$MAN" ]]; then
    DUP="$(python3 - "$MAN" <<'PY'
import json, sys, collections
d = json.load(open(sys.argv[1]))
envs = d.get("subos_info", {}).get("envs", {})
by = collections.Counter(k.split("@")[0] for k in envs)
ws  = d.get("workspace", {})
bad = [n for n, c in by.items()
       if c > 1 and not ws.get(n, {}).get("active")]
print(" ".join(bad))
PY
)"
    [[ -z "$DUP" ]] || fail "a package is bound at several versions with no
active version, so every one of them contributes: $DUP"
    ok "no contested binding"
fi

step "6. doctor, from inside the subos"
OUT="$(x subos use relverify --cmd "$BIN self doctor" 2>&1 || true)"
echo "$OUT" | grep -qiE "double binding|loader/libc split" \
    && fail "doctor reports a structural defect on a freshly built home:
$OUT"
ok "no structural findings from inside the subos"

step "7. doctor is clean"
OUT="$(x self doctor 2>&1 || true)"
echo "$OUT" | grep -qiE "double binding|env orphan|loader/libc split" \
    && fail "doctor reports a defect on a freshly built home:
$OUT"
ok "no findings"

step "8. the real home was never written"
if [[ -d "$REAL/data/xpkgs" ]]; then
    NEWER="$(find "$REAL/data/xpkgs" -newer "$MARKER" -print -quit 2>/dev/null)"
    [[ -z "$NEWER" ]] || fail "the real store changed during this run: $NEWER"
    ok "$REAL/data/xpkgs unchanged"
else
    ok "no real store on this machine to disturb"
fi

echo
echo "PASS: release lifecycle"
