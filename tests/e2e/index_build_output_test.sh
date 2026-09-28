#!/usr/bin/env bash
# E2E: an index build script's progress is rendered by xlings, not written to
# fd 1 (openxlings/xlings#629).
#
# The index repositories' own pkgindex-build.lua draw a self-refreshing line
# ("\r[i/n] ns::file\033[K"). They run in-process and cannot know where this
# process's output goes; written to a file or a pipe those frames arrived as
# carriage returns and escape sequences. libxpkg now hands the script's output
# to xlings (build_index's BuildOutput), which decides.
#
#   B1  `update` to a file: no '\r', no ESC, and the build named once
#   B2  a line the script prints that is not a step still reaches the user
#   B3  the interface still turns each step into an `index_rebuild` progress
#       event, spelled as before
#
# Offline: a local index whose build script only prints.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/index_build_output"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() {
  chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
      XLINGS_NON_INTERACTIVE=1 \
      "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR/subos/default/bin"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
# The shape of xim-pkgindex-*/pkgindex-build.lua's progress, plus one line of
# its own words -- what a failing build prints before it stops.
cat > "$LOCAL_INDEX_DIR/pkgindex-build.lua" <<'LUA'
package = { name = "pkgindex-update", xpm = { linux = { ["latest"] = {} } } }
function install()
    local names = { "alpha.lua", "beta.lua", "gamma.lua" }
    for i, name in ipairs(names) do
        io.write(string.format("\r[%d/%d] fx::%s\027[K", i, #names, name))
        io.flush()
    end
    print("")
    print("fixture build says hello")
    return true
end
LUA
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON
RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ── B1 / B2 ──────────────────────────────────────────────────────────
log "B1: update to a file carries no carriage return and no escape sequence"
OUT_FILE="$RUNTIME_DIR/update.log"
RUN update > "$OUT_FILE" 2>&1 || { cat "$OUT_FILE"; fail "B1: update failed"; }
if LC_ALL=C grep -q $'\r' "$OUT_FILE"; then
  cat -v "$OUT_FILE"; fail "B1: a carriage return reached the file"
fi
if LC_ALL=C grep -q $'\033' "$OUT_FILE"; then
  cat -v "$OUT_FILE"; fail "B1: an escape sequence reached the file"
fi
n="$( { grep -c '\[index\] built .* (3 files)' "$OUT_FILE" || true; } )"
[[ "$n" -eq 1 ]] || { cat "$OUT_FILE"; fail "B1: the build was named $n time(s), expected once"; }
if grep -q 'fx::alpha' "$OUT_FILE"; then
  cat "$OUT_FILE"; fail "B1: a progress step was printed as a line"
fi
log "  ok"

log "B2: the script's own words still reach the user"
grep -q 'fixture build says hello' "$OUT_FILE" \
  || { cat "$OUT_FILE"; fail "B2: a non-step line was swallowed"; }
log "  ok"

# ── B3 ───────────────────────────────────────────────────────────────
log "B3: the interface reports each step as an index_rebuild progress event"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
NDJSON="$(RUN interface update_packages --args '{}' 2>/dev/null)" \
  || fail "B3: update_packages failed"
python3 - "$NDJSON" <<'PY' || fail "B3: see above"
import json, sys
steps = []
for line in sys.argv[1].splitlines():
    if not line.startswith("{"):
        continue
    e = json.loads(line)
    if e.get("kind") == "progress" and e.get("phase") == "index_rebuild":
        steps.append(e.get("message", ""))
want = [f"rebuilding index cache {i}/3: fx::{n}.lua"
        for i, n in enumerate(["alpha", "beta", "gamma"], start=1)]
missing = [w for w in want if w not in steps]
if missing:
    sys.exit(f"missing index_rebuild steps: {missing}; got {steps}")
PY
log "  ok"

log "PASS: index_build_output"
