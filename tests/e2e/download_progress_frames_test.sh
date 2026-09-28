#!/usr/bin/env bash
# E2E: download progress is data, and the renderer owns its frames
# (interface protocol 1.3).
#
# Before 1.3 the index download of `xlings update` emitted one
# `download_progress` event per received chunk with `prevLines = 0`, so the CLI
# appended a new frame for every chunk: dozens of blocks for one index on a
# terminal, and one block per chunk in a CI log. The install path kept a frame
# count itself and drew at the downloader's cadence.
#
# Assertions, for `xlings update` (an index artifact over HTTP) and for
# `xlings install` (a package payload over HTTP):
#   P1  off a terminal, each download prints exactly two lines: one when it
#       starts and one when it finishes;
#   P2  on a pseudo-terminal, the number of frames drawn is at most the
#       command's elapsed time over 100 ms, plus two (the first and the last),
#       and the last one is followed by a blank line; an index artifact draws
#       no frame at all (its start and finish lines only), and neither does
#       anything under `--ui-mode cli`;
#   P3  on the NDJSON interface, one stream's events number at most its elapsed
#       time over 100 ms plus two, and every event names its stream.
#
# Hermetic: both files are served by a local HTTPS server on 127.0.0.1 with a
# certificate made for the run (the downloader speaks HTTPS only, and reads the
# trust anchor from SSL_CERT_FILE), and each file is several megabytes of
# incompressible bytes, so a transfer arrives in many chunks. P2 needs `script` (util-linux or BSD); where it is missing the leg
# reports NOT RUN and the test fails, because a criterion that did not run did
# not pass.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./project_test_lib.sh
source "$SCRIPT_DIR/project_test_lib.sh"

XLINGS_BIN="$(find_xlings_bin)"
command -v python3 >/dev/null 2>&1 || fail "python3 is required to serve the fixtures"
command -v openssl >/dev/null 2>&1 || fail "openssl is required to make the fixture certificate"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xlings-progress-frames.XXXXXX")"
SERVER_PID=""
cleanup() {
  [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

SERVE="$WORK/serve"
mkdir -p "$SERVE"

# ── the HTTPS server, on a port the kernel picks ────────────────────
# A config of its own: `req` reads one, and an openssl whose compiled-in path
# does not exist (a relocated payload) fails before making anything.
printf '[req]\ndistinguished_name = dn\n[dn]\n' > "$WORK/openssl.cnf"
openssl req -config "$WORK/openssl.cnf" -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" \
  -keyout "$WORK/key.pem" -out "$WORK/cert.pem" >"$WORK/openssl.log" 2>&1 \
  || fail "openssl could not make the fixture certificate: $(tail -2 "$WORK/openssl.log")"
export SSL_CERT_FILE="$WORK/cert.pem"
python3 - "$SERVE" "$WORK/cert.pem" "$WORK/key.pem" > "$WORK/port" 2> "$WORK/server.log" <<'PY' &
import functools, http.server, socketserver, ssl, sys
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=sys.argv[1])
class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
with Server(("127.0.0.1", 0), handler) as srv:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(sys.argv[2], sys.argv[3])
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(srv.server_address[1], flush=True)
    srv.serve_forever()
PY
SERVER_PID=$!
for _ in $(seq 1 50); do [[ -s "$WORK/port" ]] && break; sleep 0.1; done
PORT="$(head -1 "$WORK/port")"
[[ -n "$PORT" ]] || fail "the fixture HTTPS server did not start: $(cat "$WORK/server.log")"
BASE="https://127.0.0.1:$PORT"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

# ── the package payload: 6 MiB of random bytes ──────────────────────
mkdir -p "$WORK/payload/progress-fixture/bin"
head -c 6291456 /dev/urandom > "$WORK/payload/progress-fixture/bin/blob.bin"
tar -czf "$SERVE/progress-fixture-1.0.0.tar.gz" -C "$WORK/payload" progress-fixture
PAYLOAD_SHA="$(sha256_of "$SERVE/progress-fixture-1.0.0.tar.gz")"

# ── the index: one package, plus 6 MiB that makes the artifact large ──
SRC="$WORK/index-src"
mkdir -p "$SRC/pkgs/p" "$SRC/assets"
head -c 6291456 /dev/urandom > "$SRC/assets/blob.bin"
cat > "$SRC/pkgs/p/progress-fixture.lua" <<LUA
package = {
    spec = "1",
    name = "progress-fixture",
    description = "Fixture for tests/e2e/download_progress_frames_test.sh",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["1.0.0"] = { url = "$BASE/progress-fixture-1.0.0.tar.gz", sha256 = "$PAYLOAD_SHA" } },
        macosx  = { ["1.0.0"] = { url = "$BASE/progress-fixture-1.0.0.tar.gz", sha256 = "$PAYLOAD_SHA" } },
        windows = { ["1.0.0"] = { url = "$BASE/progress-fixture-1.0.0.tar.gz", sha256 = "$PAYLOAD_SHA" } },
    },
}
function install() return true end
LUA
bash "$ROOT_DIR/tools/build_xim_index_artifact.sh" --version 9.9.9 --out "$SERVE" --src "$SRC" >/dev/null
python3 - "$SERVE"/xim-index-9.9.9.manifest.json "$SERVE/xim-index-pointers.json" <<'PY'
import sys, json
m = json.load(open(sys.argv[1]))
json.dump({"format_version": 1, "indexes": {"xim": m}}, open(sys.argv[2], "w"))
PY

# A fresh home per leg, so every leg downloads both files.
fresh_home() {
  local home="$WORK/home-$1"
  rm -rf "$home"
  mkdir -p "$home"
  XLINGS_HOME="$home" XLINGS_INDEX_BASE_URL="$BASE" "$XLINGS_BIN" self init >/dev/null 2>&1 \
    || fail "self init failed ($1)"
  printf '{"mirror":"GLOBAL","index_repos":[]}\n' > "$home/.xlings.json"
  # No sub-indexes: the only downloads are the two this test serves.
  mkdir -p "$home/data/xim-index-repos"
  printf '{}\n' > "$home/data/xim-index-repos/xim-indexrepos.json"
  echo "$home"
}

run_in() {  # $1=home, rest=command; stdout+stderr to the caller
  local home="$1"; shift
  XLINGS_HOME="$home" XLINGS_INDEX_BASE_URL="$BASE" XLINGS_NON_INTERACTIVE=1 \
    "$XLINGS_BIN" "$@"
}

now_ms() { python3 -c 'import time; print(int(time.monotonic() * 1000))'; }

# Milestone lines: four spaces, the downloading, done or failed glyph (U+2193,
# U+2713, U+2717), a space, the item's name, and optionally two spaces and the
# size or time. The install plan's own listing uses another glyph and does not
# count.
count_milestones() {  # $1=file $2=item name, as an extended regular expression
  LC_ALL=C grep -cE "^    ($(printf '\xe2\x86\x93')|$(printf '\xe2\x9c\x93')|$(printf '\xe2\x9c\x97')) $2(  .*)?\$" "$1" || true
}

# ── P1: off a terminal, two lines per download ──────────────────────
log "P1: update and install off a terminal print one start and one finish line"
H="$(fresh_home p1)"
run_in "$H" update > "$WORK/p1-update.log" 2>&1 || { cat "$WORK/p1-update.log"; fail "P1: update failed"; }
n="$(count_milestones "$WORK/p1-update.log" xim)"
[[ "$n" -eq 2 ]] || { cat "$WORK/p1-update.log"; fail "P1: the index download printed $n progress lines off a terminal, expected 2"; }
if LC_ALL=C grep -q $'\033\\[' "$WORK/p1-update.log"; then
  fail "P1: an escape sequence reached a destination that is not a terminal"
fi
run_in "$H" install progress-fixture@1.0.0 -y > "$WORK/p1-install.log" 2>&1 \
  || { cat "$WORK/p1-install.log"; fail "P1: install failed"; }
n="$(count_milestones "$WORK/p1-install.log" 'xim:progress-fixture@1\.0\.0')"
[[ "$n" -eq 2 ]] || { cat "$WORK/p1-install.log"; fail "P1: the package download printed $n progress lines off a terminal, expected 2"; }
log "  ok: two lines per download, for update and for install"

# ── P2: on a pseudo-terminal, frames bounded by elapsed time ────────
frames_bound_ok() {  # $1=transcript $2=elapsed ms $3=label
  local frames bound
  frames="$(LC_ALL=C grep -o $'\033\\[J' "$1" | wc -l | tr -d ' ')"
  bound=$(( $2 / 100 + 2 ))
  [[ "$frames" -ge 1 ]] || fail "P2 ($3): no progress frame was drawn on the terminal"
  [[ "$frames" -le "$bound" ]] \
    || fail "P2 ($3): $frames frames in ${2} ms; at most $bound allowed (one per 100 ms, plus the first and the last)"
  log "  ok: $3 drew $frames frame(s) in ${2} ms (bound $bound)"
}
in_pty() {  # $1=transcript, rest=command
  local out="$1"; shift
  local cmd
  cmd="$(printf '%q ' "$@")"
  if script -qfec true /dev/null >/dev/null 2>&1; then
    script -qfec "$cmd" /dev/null > "$out" 2>&1
  elif script -q /dev/null true >/dev/null 2>&1; then
    script -q /dev/null sh -c "$cmd" > "$out" 2>&1
  else
    return 127
  fi
}
log "P2: update and install on a pseudo-terminal draw a bounded number of frames"
if ! command -v script >/dev/null 2>&1; then
  fail "P2 NOT RUN: no 'script' command to provide a pseudo-terminal"
fi
H="$(fresh_home p2)"
t0="$(now_ms)"
in_pty "$WORK/p2-update.log" env XLINGS_HOME="$H" XLINGS_INDEX_BASE_URL="$BASE" \
  XLINGS_NON_INTERACTIVE=1 TERM=xterm-256color "$XLINGS_BIN" update \
  || { cat "$WORK/p2-update.log"; fail "P2: update failed on a pseudo-terminal"; }
t1="$(now_ms)"
no_frames_ok() {  # $1=transcript $2=label $3=milestone name
  local frames n
  # `|| true`: no match is the expected answer here, and under pipefail a
  # failing grep in an assignment ends the script without a word.
  frames="$( { LC_ALL=C grep -o $'\033\\[J' "$1" || true; } | wc -l | tr -d ' ')"
  [[ "$frames" -eq 0 ]] || { cat "$1"; fail "P2 ($2): $frames frame(s) drawn, expected none"; }
  n="$(count_milestones "$1" "$3")"
  [[ "$n" -eq 2 ]] || { cat "$1"; fail "P2 ($2): $n start/finish line(s), expected 2"; }
  log "  ok: $2 drew no frame, and said when it started and finished"
}
# An index artifact is a few kilobytes, announced again by "[index] updated
# from artifact" right after: no frame for it, on a terminal either.
no_frames_ok "$WORK/p2-update.log" "update (index artifact)" xim
t0="$(now_ms)"
in_pty "$WORK/p2-install.log" env XLINGS_HOME="$H" XLINGS_INDEX_BASE_URL="$BASE" \
  XLINGS_NON_INTERACTIVE=1 TERM=xterm-256color "$XLINGS_BIN" install progress-fixture@1.0.0 -y \
  || { cat "$WORK/p2-install.log"; fail "P2: install failed on a pseudo-terminal"; }
t1="$(now_ms)"
frames_bound_ok "$WORK/p2-install.log" $(( t1 - t0 )) "install"
# What follows the progress block starts a paragraph of its own: the frame
# that finishes the block shows the cursor again and then ends one blank line.
python3 - "$WORK/p2-install.log" <<'PY' || fail "P2 (install): no blank line after the last frame"
import sys
data = open(sys.argv[1], "rb").read()
# The LAST frame, then the cursor-show that closes it. Not simply the last
# cursor-show in the transcript: an exit handler writes one more at the very
# end, with nothing after it.
last_frame = data.rfind(b"\x1b[J")
if last_frame < 0:
    sys.exit("no frame in the transcript")
at = data.find(b"\x1b[?25h", last_frame)
if at < 0:
    sys.exit("the cursor was never shown again")
tail = data[at + len(b"\x1b[?25h"):]
if not (tail.startswith(b"\n") or tail.startswith(b"\r\n")):
    sys.exit("after the last frame: %r" % tail[:40])
PY
log "  ok: a blank line follows the last frame"
# `--ui-mode cli` is plain text: the same download, no frame (`self update`
# runs its child installs this way).
H="$(fresh_home p2cli)"
run_in "$H" update >/dev/null 2>&1 || fail "P2: update failed (p2cli)"
in_pty "$WORK/p2-cli.log" env XLINGS_HOME="$H" XLINGS_INDEX_BASE_URL="$BASE" \
  XLINGS_NON_INTERACTIVE=1 TERM=xterm-256color "$XLINGS_BIN" --ui-mode cli install progress-fixture@1.0.0 -y \
  || { cat "$WORK/p2-cli.log"; fail "P2: --ui-mode cli install failed on a pseudo-terminal"; }
no_frames_ok "$WORK/p2-cli.log" "install --ui-mode cli" 'xim:progress-fixture@1\.0\.0'

# ── P3: the interface stream is bounded and names its streams ───────
log "P3: interface update_packages sends a bounded, named stream"
H="$(fresh_home p3)"
t0="$(now_ms)"
run_in "$H" interface update_packages --args '{}' > "$WORK/p3.ndjson" 2> "$WORK/p3.err" \
  || { cat "$WORK/p3.err"; fail "P3: update_packages failed"; }
t1="$(now_ms)"
python3 - "$WORK/p3.ndjson" $(( t1 - t0 )) <<'PY' || fail "P3: see above"
import json, sys
path, elapsed = sys.argv[1], int(sys.argv[2])
events = []
for line in open(path):
    line = line.strip()
    if not line:
        continue
    obj = json.loads(line)
    if obj.get("kind") == "data" and obj.get("dataKind") == "download_progress":
        events.append(obj["payload"])
if not events:
    sys.exit("P3: no download_progress event on the wire")
streams = {}
for p in events:
    if "stream" not in p:
        sys.exit("P3: a download_progress event names no stream")
    streams.setdefault(p["stream"], []).append(p)
bound = elapsed // 100 + 2
for name, evs in streams.items():
    if len(evs) > bound:
        sys.exit(f"P3: stream {name} sent {len(evs)} events in {elapsed} ms; at most {bound}")
    if not all(f["finished"] for f in evs[-1]["files"]):
        sys.exit(f"P3: the last event of stream {name} is not final")
print(f"[project-e2e]   ok: {len(events)} event(s) in {len(streams)} stream(s) within {elapsed} ms")
PY

log "PASS: download progress frames (P1, P2, P3)"
