#!/usr/bin/env bash
# E2E: the terminal output contract.
#
# Every renderer used to pick its own width and its own answer to "what if it
# does not fit", and no two agreed. `print_info_panel` widened the canvas past
# the terminal and let the terminal hard-wrap every line (the reported "ragged
# output in a small window"); everything else drew at the full terminal width
# and let ftxui shrink each hbox child proportionally, which eats a package
# name as readily as a description — `xim:binutils@2.42` rendered as
# `xim:binutils@2`, a version number that looks valid and is wrong. Nothing
# asked whether stdout was a terminal, so a pipe got true color, a CR per
# line, trailing padding and a NUL byte.
#
# The invariants asserted here:
#   S1. No rendered line exceeds the terminal width, at any width.
#   S2. A package name is never truncated; the description is.
#   S3. A pipe gets no NUL, no CR, no trailing spaces and no ANSI.
#   S4. NO_COLOR silences color even on a terminal.
#   S5. The active version is marked by a glyph, not by color alone, and the
#       command says what to do next.
#   S6. Glyphs come from the single table in src/core/glyph.cppm.
#
# Widths are measured in display columns, never bytes — measuring in bytes is
# the bug, so a test that measured in bytes would pass over it.
#
# The checkers are files rather than heredocs on purpose. `python3 - <<'PY'`
# feeds the heredoc to python as its *program*, so a piped-in payload never
# arrives and `sys.stdin.read()` returns "" — a width check written that way
# passes on every input, including a broken one. That is the same
# looks-like-it-ran-and-did-nothing failure this suite exists to catch.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index
command -v python3 >/dev/null 2>&1 || { log "SKIP: python3 not available"; exit 0; }

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/tui_output_contract"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
BIN_DIR="$RUNTIME_DIR/checkers"
STATE="$HOME_DIR/.xlings.json"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"
# RUN cds to /tmp, so a relative XLINGS_BIN would resolve to nothing there.
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"

# A deliberately long name: at 40 columns it is wider than the whole terminal,
# which is exactly the case where the old renderer started cutting into it.
PKG="widthfixture-with-a-deliberately-long-name"

RUN() {
  local width="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_TERM_WIDTH="$width" \
      "$XLINGS_BIN" "$@" )
}

mkdir -p "$BIN_DIR"

# ── checkers ────────────────────────────────────────────────────────

cat > "$BIN_DIR/common.py" <<'PY'
import re, sys, unicodedata

SGR = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

def plain(text):
    return SGR.sub("", text)

def cols(s):
    n = 0
    for ch in s:
        if unicodedata.combining(ch):
            continue
        n += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return n

def read_stdin():
    data = sys.stdin.buffer.read()
    if not data:
        # An empty payload would make every check below pass. Say so instead.
        print("no input reached the checker")
        sys.exit(2)
    return data
PY

# Fails if any line of stdin is wider than $1 display columns.
cat > "$BIN_DIR/check_width.py" <<'PY'
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import plain, cols, read_stdin

limit = int(sys.argv[1])
text = plain(read_stdin().decode("utf-8", "replace"))
bad = [(cols(l), l) for l in text.split("\n") if cols(l) > limit]
if bad:
    w, l = bad[0]
    print(f"{len(bad)} line(s) exceed {limit} columns; first is {w}: {l!r}")
    sys.exit(1)
PY

# Fails if stdin holds anything a pipe should never receive.
cat > "$BIN_DIR/check_pipe.py" <<'PY'
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import read_stdin

data = read_stdin()
problems = []
if b"\x00" in data: problems.append("NUL byte")
if b"\r" in data:   problems.append("carriage return")
if b"\x1b" in data: problems.append("ANSI escape")
for line in data.split(b"\n"):
    if line != line.rstrip(b" "):
        problems.append("trailing spaces")
        break
if problems:
    print("piped output contains: " + ", ".join(sorted(set(problems))))
    sys.exit(1)
PY

# Fails unless $1 appears in stdin once whitespace is ignored. A wrapped
# identifier keeps every character and splits across lines; a truncated one
# loses characters. Comparing whitespace-free tells those two apart.
cat > "$BIN_DIR/check_contains.py" <<'PY'
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import plain, read_stdin

want = re.sub(r"\s+", "", sys.argv[1])
got = re.sub(r"\s+", "", plain(read_stdin().decode("utf-8", "replace")))
if want not in got:
    print(f"{want!r} is missing or was cut")
    sys.exit(1)
PY

# Fails if the SUBCOMMANDS rows of `--help` do not share a description column.
cat > "$BIN_DIR/check_help_align.py" <<'PY'
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import plain, read_stdin

lines = plain(read_stdin().decode("utf-8", "replace")).split("\n")
try:
    start = lines.index("  SUBCOMMANDS") + 1
except ValueError:
    print("no SUBCOMMANDS section found")
    sys.exit(1)

starts, rows = set(), 0
for l in lines[start:]:
    if not l.strip():
        break
    m = re.match(r"^(\s{4}\S+\s{2,})\S", l)
    if m:
        rows += 1
        starts.add(len(m.group(1)))
if rows < 2:
    print(f"expected several two-column rows, saw {rows}")
    sys.exit(1)
if len(starts) > 1:
    print(f"description starts at differing columns: {sorted(starts)}")
    sys.exit(1)
PY

# Fails unless exactly one version row carries the active marker glyph.
cat > "$BIN_DIR/check_active_marker.py" <<'PY'
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import plain, read_stdin

lines = plain(read_stdin().decode("utf-8", "replace")).split("\n")
rows = [l for l in lines if re.search(r"\b[12]\.0\.0\b", l) and "hint" not in l]
marked = [l for l in rows if l.lstrip().startswith("▸")]
if len(rows) < 2:
    print(f"expected two version rows, got {len(rows)}: {rows}")
    sys.exit(1)
if len(marked) != 1:
    print(f"expected exactly one glyph-marked row, got {len(marked)}: {rows}")
    sys.exit(1)
PY

# Runs xlings on a real pty so the color decision is exercised for real.
cat > "$BIN_DIR/run_pty.py" <<'PY'
import os, pty, fcntl, termios, struct, select, subprocess, sys

nocolor, home, binary, *args = sys.argv[1:]
env = {"HOME": os.environ.get("HOME", "/tmp"), "PATH": "/usr/bin:/bin",
       "XLINGS_HOME": home, "TERM": "xterm-256color"}
if nocolor == "1":
    env["NO_COLOR"] = "1"
mfd, sfd = pty.openpty()
fcntl.ioctl(sfd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
p = subprocess.Popen([binary, *args], stdin=subprocess.DEVNULL,
                     stdout=sfd, stderr=sfd, env=env, close_fds=True)
os.close(sfd)
out = b""
while True:
    r, _, _ = select.select([mfd], [], [], 20)
    if not r:
        break
    try:
        d = os.read(mfd, 65536)
    except OSError:
        break
    if not d:
        break
    out += d
p.wait()
sys.stdout.buffer.write(out)
PY

check_width()    { python3 "$BIN_DIR/check_width.py" "$@"; }
check_pipe()     { python3 "$BIN_DIR/check_pipe.py"; }
check_contains() { python3 "$BIN_DIR/check_contains.py" "$@"; }

# The checkers refuse an empty payload, so prove the plumbing before relying
# on it: a deliberately over-wide line must be reported.
printf 'x%.0s' $(seq 1 50) | check_width 40 >/dev/null 2>&1 \
  && fail "S0: the width checker accepted a 50-column line at limit 40"
log "S0: checkers are wired to their input"

# ── fixture index with a two-version package ────────────────────────
mkdir -p "$HOME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/w"

cat > "$LOCAL_INDEX_DIR/pkgs/w/$PKG.lua" <<LUA
package = {
    spec = "1",
    name = "$PKG",
    description = "A fixture whose description is long enough that a narrow terminal has to shorten it, while the name above must survive intact",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    io.writefile(path.join(dir, "bin", "widthfixture"), "#!/bin/sh\necho widthfixture\n")
    os.exec("chmod +x " .. path.join(dir, "bin", "widthfixture"))
    return true
end

function config()
    xvm.add("widthfixture", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall() return true end
LUA

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$STATE" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF

log "Initializing sandbox XLINGS_HOME at $HOME_DIR"
RUN 100 self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

RUN 100 install "$PKG@1.0.0" -y >/dev/null 2>&1 || fail "fixture install 1.0.0 failed"
RUN 100 install "$PKG@2.0.0" -y >/dev/null 2>&1 || fail "fixture install 2.0.0 failed"

# The commands that render something. `use widthfixture` lists versions rather
# than prompting, because stdin here is not a terminal.
COMMANDS=(
  "--help"
  "list --all"
  "search widthfixture"
  "info $PKG"
  "config"
  "use widthfixture"
  "self doctor"
)

# ── S1: nothing exceeds the terminal width ──────────────────────────
log "S1: no rendered line exceeds the terminal width"
for width in 40 60 100 200; do
  for cmd in "${COMMANDS[@]}"; do
    # shellcheck disable=SC2086
    out="$(RUN "$width" $cmd 2>&1 || true)"
    msg="$(printf '%s\n' "$out" | check_width "$width")" \
      || fail "S1: \`xlings $cmd\` at $width cols: $msg"
  done
done

# ── S2: identifiers survive, descriptions give way ──────────────────
log "S2: the package name is never truncated"
# Below a width that fits it the identifier wraps rather than being cut, so
# the assertion is on the characters and not on them being contiguous.
for width in 40 60 100; do
  out="$(RUN "$width" list --all 2>&1 || true)"
  msg="$(printf '%s\n' "$out" | check_contains "$PKG@2.0.0")" \
    || fail "S2: at $width cols, $msg
$out"
done

# S1 already proved the line fits, so the remaining risk is that it fits
# because the *name* gave way and the description survived whole.
out="$(RUN 60 list --all 2>&1 || true)"
if grep -qF "while the name above must survive intact" <<<"$out"; then
  fail "S2: the full description fit at 60 cols — the fixture is not exercising the case"
fi

log "S2: the name column stays a column (help rows align)"
out="$(RUN 60 --help 2>&1 || true)"
msg="$(printf '%s\n' "$out" | python3 "$BIN_DIR/check_help_align.py")" \
  || fail "S2: $msg
$out"

# ── S3: a pipe is a pipe ────────────────────────────────────────────
log "S3: piped output carries no NUL / CR / trailing space / ANSI"
for cmd in "${COMMANDS[@]}"; do
  # shellcheck disable=SC2086
  RUN 0 $cmd > "$RUNTIME_DIR/out.bin" 2>/dev/null || true
  msg="$(check_pipe < "$RUNTIME_DIR/out.bin")" || fail "S3: \`xlings $cmd\`: $msg"
done

# ── S4: NO_COLOR is honored on a real terminal ──────────────────────
log "S4: NO_COLOR silences color on a terminal"
# SGR specifically, not "any escape": restoring cursor visibility on exit is
# a cursor control and stays, NO_COLOR governs color.
has_color() { grep -qE $'\033\[[0-9;]*m'; }

colored="$(python3 "$BIN_DIR/run_pty.py" 0 "$HOME_DIR" "$XLINGS_BIN" config || true)"
printf '%s' "$colored" | has_color \
  || fail "S4: a terminal got no color at all — the check would prove nothing"
plain_out="$(python3 "$BIN_DIR/run_pty.py" 1 "$HOME_DIR" "$XLINGS_BIN" config || true)"
if printf '%s' "$plain_out" | has_color; then
  fail "S4: NO_COLOR=1 still emitted color:
$plain_out"
fi

# ── S5: active version is a glyph, and the next step is stated ──────
log "S5: the active version is marked without color, and a hint follows"
out="$(RUN 100 use widthfixture 2>&1 || true)"
grep -q 'xlings use widthfixture <version>' <<<"$out" \
  || fail "S5: no next-step hint:
$out"
msg="$(printf '%s\n' "$out" | python3 "$BIN_DIR/check_active_marker.py")" \
  || fail "S5: $msg
$out"

# ── S6: one glyph table ─────────────────────────────────────────────
log "S6: glyphs come only from src/core/glyph.cppm"
# Two glyphs stand for the rule, in both spellings (literal and \x-escaped):
#
#   U+24D8 ⓘ — the coverage bet the table explicitly refuses, and what
#               `self doctor` used to spell inline;
#   U+26A0 ⚠ — the other label glyph doctor owned, and the one a second,
#               dead icon table in src/platform/ also carried.
#
# Neither appears in ordinary prose, so a hit outside the table is a real
# regression rather than a comment. The other marks (→, ·) are left out on
# purpose: they are common in comments and checking them is all noise.
GOVERNED='ⓘ|\\xe2\\x93\\x98|⚠|\\xe2\\x9a\\xa0'
if grep -rnE "$GOVERNED" "$ROOT_DIR/src" --include='*.cppm' --include='*.cpp' \
     | grep -v 'src/core/glyph.cppm' | grep -q .; then
  grep -rnE "$GOVERNED" "$ROOT_DIR/src" --include='*.cppm' --include='*.cpp' \
    | grep -v 'src/core/glyph.cppm'
  fail "S6: a governed glyph was spelled outside src/core/glyph.cppm"
fi

log "PASS: terminal output contract holds"
