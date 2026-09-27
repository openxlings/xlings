#!/usr/bin/env bash
# interface_update_packages_progress_test.sh — regression test for the
# `interface update_packages` NDJSON defects found while wiring mcpp's
# index refresh through the interface protocol instead of the bare CLI:
#
#   Defect 1: a refresh that rebuilds the index cache emits NOTHING but
#   `heartbeat` and `result` events -- no `progress` at all, so a client
#   has no way to show the user anything is happening.
#
#   Defect 2 (the more serious one): when the index cache IS rebuilt, the
#   rebuild runs a downloaded `pkgindex-build.lua` in-process through
#   libxpkg's Lua sandbox, and that script's own progress indicator writes
#   raw terminal text -- "\r[i/n] ns::name\x1b[K" -- straight onto the
#   process's real stdout. The interface protocol allows nothing but one
#   JSON object per line there (docs/spec/interface-ndjson-v1.md §5); a
#   client reading NDJSON hits a line that is not JSON and the stream is
#   no longer parseable from that point on.
#
# This fixture reproduces defect 2 without any network access: a local
# index repo whose own pkgindex-build.lua writes exactly that shape of
# text (mirroring xim-pkgindex-awesome's pkgindex-update.lua) is enough to
# force the cache rebuild, since `update_packages` always calls
# PackageCatalog::rebuild(/*force=*/true).
#
# Assertions:
#   A1: every stdout line from `interface update_packages` parses as JSON.
#   A2: at least one `{"kind":"progress"}` line appears in that stream.
#
# Fully self-contained/offline: builds its own local global index under the
# runtime dir; never mutates tests/fixtures.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/interface_update_packages_progress"
HOME_DIR="$RUNTIME_DIR/home"
GLOBAL_DIR="$RUNTIME_DIR/global_index"   # the local global (xim) index — offline

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

mkdir -p "$HOME_DIR/subos/default/bin" "$GLOBAL_DIR/pkgs/f"

# Two package files: enough for the noisy scan's "[i/n]" counter to be
# meaningful (n > 1), not merely 1/1.
emit_pkg() { # $1=path $2=name
  cat > "$1" <<LUA
package = {
    spec = "1",
    name = "$2",
    description = "Test fixture package (interface update_packages progress)",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}
function install() return true end
LUA
}
emit_pkg "$GLOBAL_DIR/pkgs/f/toolone.lua" "toolone"
emit_pkg "$GLOBAL_DIR/pkgs/f/tooltwo.lua" "tooltwo"

# Neutralise sub-index discovery so the refresh stays offline.
printf 'xim_indexrepos = {}\n' > "$GLOBAL_DIR/xim-indexrepos.lua"

# The fixture's own pkgindex-build.lua: runs on every FORCED rebuild
# (build_index calls run_pkgindex_build() before parsing pkgs/), through
# libxpkg's plain-Lua build sandbox (full io library, no cprintf) -- the
# same runtime xim-pkgindex-awesome's pkgindex-update.lua runs under. Its
# install() reproduces that script's progress() verbatim: a self-refreshing
# "\r[i/n] ns::name\x1b[K" line, real io.write + io.flush.
cat > "$GLOBAL_DIR/pkgindex-build.lua" <<'LUA'
package = {
    name = "pkgindex-update",
    namespace = "fixture",
}

local pkgsdir = path.join(os.scriptdir(), "pkgs")

local function progress(i, n, name)
    if io and type(io.write) == "function" then
        io.write(string.format("\r[%d/%d] fixture::%s", i, n, name) .. "\27[K")
        if type(io.flush) == "function" then io.flush() end
    else
        print(string.format("[%d/%d] fixture::%s", i, n, name))
    end
end

function installed()
    return false
end

function install()
    local files = os.files(path.join(pkgsdir, "**.lua"))
    local total = #files
    for i, file in ipairs(files) do
        progress(i, total, path.filename(file))
    end
    if total > 0 then print("") end
    return true
end

function uninstall()
    return true
end
LUA

cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "subos": {"default": {"dir": ""}},
  "index_repos": [
    {"name": "xim", "url": "$GLOBAL_DIR"}
  ]
}
JSON

RUN_HOME() {
  ( cd /tmp && env -i HOME="$HOME" USER="${USER:-ci}" SHELL="${SHELL:-/bin/sh}" \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" XLINGS_INDEX_SOURCE=git \
      "$XLINGS_BIN" "$@" )
}

RUN_HOME self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ════════════════════════════════════════════════════════════════════
# The refresh: this is what mcpp will run in place of `xlings update`.
# ════════════════════════════════════════════════════════════════════
log "update_packages: forced rebuild runs the fixture's pkgindex-build.lua"

STDOUT_FILE="$RUNTIME_DIR/stdout.ndjson"
STDERR_FILE="$RUNTIME_DIR/stderr.log"

set +e
RUN_HOME interface update_packages --args '{}' \
  >"$STDOUT_FILE" 2>"$STDERR_FILE"
rc=$?
set -e

echo "---- stdout ----"; sed 's/^/    /' "$STDOUT_FILE"
echo "---- stderr ----"; sed 's/^/    /' "$STDERR_FILE"

[[ "$rc" -eq 0 ]] || fail "update_packages exited $rc"

# ── A1: every stdout line parses as JSON (protocol §5: nothing else may
#    reach stdout in interface mode) ──────────────────────────────────
if command -v python3 >/dev/null 2>&1; then
  bad_lines=0
  while IFS= read -r line; do
    [[ -n "$line" ]] || continue
    if ! python3 -c 'import sys, json; json.loads(sys.argv[1])' "$line" \
        >/dev/null 2>&1; then
      bad_lines=$((bad_lines + 1))
      echo "[project-e2e]   not JSON: $line" >&2
    fi
  done < "$STDOUT_FILE"
  [[ "$bad_lines" -eq 0 ]] \
    || fail "A1: $bad_lines stdout line(s) did not parse as JSON -- raw terminal text reached stdout"
  log "  ✓ A1: every stdout line parses as JSON"
else
  log "  SKIP A1 detail check: no python3 to validate JSON (grep fallback below still runs)"
  ! grep -qF $'\x1b[' "$STDOUT_FILE" \
    || fail "A1 (fallback): an ANSI escape sequence reached stdout"
fi

# ── A2: at least one progress event was emitted for the rebuild ───────
grep -q '"kind":"progress"' "$STDOUT_FILE" \
  || fail "A2: no progress event on the wire for a refresh that rebuilds the index"
log "  ✓ A2: at least one progress event emitted"

log "PASS: interface update_packages progress + no stray stdout text"
