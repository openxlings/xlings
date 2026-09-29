#!/usr/bin/env bash
# E2E: a failed download is reported once, with its reason; `info` shows one
# dependency row for a recipe that declared one list.
#
# Measured on 2026.9.29.1 (a real home, `install chatgpt@26.917.71314`): one
# failed download printed three errors -- `[<pkg>] failed:` with no reason, in
# the middle of the progress frames; the reason; and `download artifact missing`,
# which is only the consequence -- and the progress bar was drawn again after
# them.
#
#   F1  a payload whose bytes do not match the recipe's sha256: the install
#       fails, the reason (sha256 mismatch) is printed, no error line is
#       reasonless, and `download artifact missing` does not appear
#   F2  the same on the NDJSON interface: exactly one `error` event for the
#       package, with a non-empty message
#   F3  `info` of a recipe written with `deps = { ... }` prints one `deps` row,
#       not a `runtime deps` and a `build deps` row holding the same list
#
# The local-disk half of the same change (a truncated file is the disk's
# fault, not the source's) is covered by the unit tests DownloadAttribution.*
# and XimDownloaderTest.ALocalWriteFailureIsReportedAsDiskFullNotAsTheSource:
# the download's lock and staging files share one directory, so no permission
# change can fail the one without the other here.
#
# Hermetic: the payload is served by a local HTTPS server on 127.0.0.1 with a
# certificate made for the run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./project_test_lib.sh
source "$SCRIPT_DIR/project_test_lib.sh"

require_fixture_index
XLINGS_BIN="$(find_xlings_bin)"
command -v python3 >/dev/null 2>&1 || fail "python3 is required to serve the fixture"
command -v openssl >/dev/null 2>&1 || fail "openssl is required to make the fixture certificate"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xlings-dlfail.XXXXXX")"
SERVER_PID=""
cleanup() {
  [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

SERVE="$WORK/serve"
mkdir -p "$SERVE"
printf '[req]\ndistinguished_name = dn\n[dn]\n' > "$WORK/openssl.cnf"
openssl req -config "$WORK/openssl.cnf" -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" \
  -keyout "$WORK/key.pem" -out "$WORK/cert.pem" >"$WORK/openssl.log" 2>&1 \
  || fail "openssl could not make the fixture certificate: $(tail -2 "$WORK/openssl.log")"
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

# The payload the server has, and a digest that is not its digest.
mkdir -p "$WORK/payload/dlfail/bin"
printf 'payload\n' > "$WORK/payload/dlfail/bin/dlfail"
tar -czf "$SERVE/dlfail-1.0.0.tar.gz" -C "$WORK/payload" dlfail
WRONG_SHA="$(printf '%064d' 0)"

HOME_DIR="$WORK/home"
LOCAL_INDEX_DIR="$WORK/xim-pkgindex"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/d"
cat > "$LOCAL_INDEX_DIR/pkgs/d/dlfail.lua" <<LUA
package = {
    spec = "1", name = "dlfail", description = "download-failure fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "aarch64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux  = { deps = { "dlfaildep" },
                   ["1.0.0"] = { url = "$BASE/dlfail-1.0.0.tar.gz", sha256 = "$WRONG_SHA" } },
        macosx = { deps = { "dlfaildep" },
                   ["1.0.0"] = { url = "$BASE/dlfail-1.0.0.tar.gz", sha256 = "$WRONG_SHA" } },
    },
}
function install() return true end
LUA
cat > "$LOCAL_INDEX_DIR/pkgs/d/dlfaildep.lua" <<'LUA'
package = {
    spec = "1", name = "dlfaildep", description = "download-failure fixture (dep)",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "aarch64"}, status = "stable", categories = {"test-fixture"},
    xpm = { linux = { ["1.0"] = {} }, macosx = { ["1.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
function install()
    io.writefile(path.join(pkginfo.install_dir(), "marker"), "x\n")
    return true
end
LUA
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"

mkdir -p "$HOME_DIR/subos/default/bin"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_NON_INTERACTIVE=1 \
      SSL_CERT_FILE="$WORK/cert.pem" \
      "$XLINGS_BIN" "$@" )
}

RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ── F1 ───────────────────────────────────────────────────────────────
log "F1: a payload that fails its sha256 is reported once, with the reason"
set +e
OUT="$(RUN install dlfail@1.0.0 -y 2>&1)"
RC=$?
set -e
[[ "$RC" != "0" ]] || fail "F1: the install succeeded with a wrong sha256:
$OUT"
grep -q "sha256 mismatch" <<<"$OUT" || fail "F1: the reason is missing:
$OUT"
if grep -q "download artifact missing" <<<"$OUT"; then
  fail "F1: the consequence was reported as a second error:
$OUT"
fi
# Every error line names something after `failed:`.
if grep -E 'failed:[[:space:]]*$' <<<"$OUT" >/dev/null; then
  fail "F1: an error line has no reason:
$OUT"
fi
N="$(grep -c 'sha256 mismatch' <<<"$OUT" || true)"
[[ "$N" == "1" ]] || fail "F1: the reason was printed $N times, expected once:
$OUT"
log "  ok"

# ── F2 ───────────────────────────────────────────────────────────────
log "F2: the interface carries exactly one error event, with a message"
IFACE="$(RUN interface install_packages \
          --args '{"targets":["dlfail@1.0.0"],"yes":true}' 2>/dev/null || true)"
[[ -n "$IFACE" ]] || fail "F2: the interface printed nothing"
python3 - "$IFACE" <<'PY' || fail "F2: see above"
import json, sys
errors = []
for line in sys.argv[1].splitlines():
    if not line.strip():
        continue
    ev = json.loads(line)
    if ev.get("kind") == "error":
        errors.append(ev)
pkg = [e for e in errors if "dlfail" in e.get("message", "")]
if len(pkg) != 1:
    print(f"F2: expected one error event for dlfail, got {len(pkg)}: {errors}")
    sys.exit(1)
msg = pkg[0]["message"]
if "sha256 mismatch" not in msg:
    print(f"F2: the event does not carry the reason: {msg}")
    sys.exit(1)
PY
log "  ok"

# ── F3 ───────────────────────────────────────────────────────────────
log "F3: info prints one deps row for a recipe that declared one list"
INFO="$(RUN info dlfail 2>&1 || true)"
grep -qE '^[[:space:]]*deps[[:space:]]+xim:dlfaildep|^[[:space:]]*deps[[:space:]]+dlfaildep' <<<"$INFO" \
  || fail "F3: no single deps row:
$INFO"
if grep -qE 'runtime deps|build deps' <<<"$INFO"; then
  fail "F3: the one list is printed as two rows:
$INFO"
fi
log "  ok"

log "PASS: download failure report"
