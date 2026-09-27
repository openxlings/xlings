#!/usr/bin/env bash
# home_identity_test.sh -- a home is declared by `.xlings-home`, not inferred
# from its shape (#617, #624).
#
#   H1  `self init` writes <home>/.xlings-home; a SubOS never receives one.
#   H2  a home without the marker (as 2026.9.28.1 left it) gains it on the first
#       command of this version, and keeps it.
#   H3  a home on read-only storage keeps working without the marker, and
#       `self doctor` says the marker is missing. Root ignores permission bits,
#       so under root this leg reports NOT RUN rather than a pass.
#   H4  #624: a home nested under another home's SubOS runs a script package
#       it installed. The package's alias names a file under the inner home,
#       and the outer home's `subos/<name>/` segment precedes it in the path;
#       before the marker the path was re-rooted at the inner home's own
#       `subos/default/` and the script was "not found".
#
# Offline: the index is a local directory, and the one package is a script.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

XLINGS_BIN="$(find_xlings_bin)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xlings-home-identity.XXXXXX")"
cleanup() { chmod -R u+w "$WORK" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

INDEX="$WORK/index"
mkdir -p "$INDEX/pkgs/n"
printf 'xim_indexrepos = {}\n' > "$INDEX/xim-indexrepos.lua"
cat > "$INDEX/pkgs/n/nested-tool.lua" <<'LUA'
package = {
    spec = "1",
    name = "nested-tool",
    description = "Fixture for tests/e2e/home_identity_test.sh",
    type = "script",
    programs = {"nested-tool"},
    status = "stable",
    xpm = {
        linux   = { ["0.0.1"] = {} },
        macosx  = { ["0.0.1"] = {} },
        windows = { ["0.0.1"] = {} },
    },
}

function xpkg_main(...)
    print("NESTED_TOOL_RAN")
    return true
end
LUA

# A home at $1, initialised against the local index.
make_home() {
  local home="$1"
  mkdir -p "$home/subos/default/bin"
  cp "$XLINGS_BIN" "$home/xlings"
  printf '{ "mirror": "GLOBAL", "index_repos": [{ "name": "xim", "url": "%s" }] }\n' \
    "$INDEX" > "$home/.xlings.json"
  RUN_IN "$home" self init >/dev/null 2>&1 || fail "self init failed for $home"
  mkdir -p "$home/data/xim-index-repos"
  printf '{}\n' > "$home/data/xim-index-repos/xim-indexrepos.json"
}

RUN_IN() {  # $1=home, rest=arguments
  local home="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$home" \
      XLINGS_LOCK_TIMEOUT=30 "$XLINGS_BIN" "$@" )
}

# ── H1 ────────────────────────────────────────────────────────────────
log "H1: self init writes the marker, and a SubOS never has one"
H="$WORK/h1"
make_home "$H"
[[ -f "$H/.xlings-home" ]] || fail "H1: self init wrote no $H/.xlings-home"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["schema"]==1 and len(d["id"])==32' \
  "$H/.xlings-home" || fail "H1: the marker is not {schema, id, created_at}: $(cat "$H/.xlings-home")"
RUN_IN "$H" subos new s1 >/dev/null 2>&1 || fail "H1: subos new failed"
[[ ! -e "$H/subos/s1/.xlings-home" ]] || fail "H1: a SubOS received a home marker"
log "  ok"

# ── H2 ────────────────────────────────────────────────────────────────
log "H2: a home without the marker gains it on its first command"
rm -f "$H/.xlings-home"
RUN_IN "$H" list >/dev/null 2>&1 || fail "H2: list failed on a marker-less home"
[[ -f "$H/.xlings-home" ]] || fail "H2: the first command did not write the marker"
first="$(cat "$H/.xlings-home")"
RUN_IN "$H" list >/dev/null 2>&1 || fail "H2: the second command failed"
[[ "$(cat "$H/.xlings-home")" == "$first" ]] || fail "H2: the marker changed on a later command"
[[ ! -e "$H/subos/s1/.xlings-home" ]] || fail "H2: adoption wrote a marker into a SubOS"
log "  ok"

# ── H3 ────────────────────────────────────────────────────────────────
log "H3: a read-only home keeps working, and doctor names the missing marker"
if [[ "$(id -u)" -eq 0 ]]; then
  log "  NOT RUN: root ignores permission bits, so a read-only home cannot be made here"
else
  R="$WORK/h3"
  make_home "$R"
  rm -f "$R/.xlings-home"
  chmod a-w "$R"
  RUN_IN "$R" list >/dev/null 2>&1 || { chmod u+w "$R"; fail "H3: a read-only home failed a command"; }
  [[ ! -e "$R/.xlings-home" ]] || { chmod u+w "$R"; fail "H3: a marker appeared in a read-only home"; }
  out="$(RUN_IN "$R" self doctor 2>&1 || true)"
  chmod u+w "$R"
  grep -q "home marker" <<<"$out" || fail "H3: self doctor did not report the missing marker: $out"
  log "  ok"
fi

# ── H4 ────────────────────────────────────────────────────────────────
log "H4: a home nested under another home's SubOS runs its own script (#624)"
OUTER="$WORK/nest/.xlings"
make_home "$OUTER"
INNER="$OUTER/subos/eco/work/mcpphome/registry"
make_home "$INNER"
RUN_IN "$INNER" install nested-tool@0.0.1 -y > "$WORK/h4-install.log" 2>&1 \
  || { cat "$WORK/h4-install.log"; fail "H4: install into the nested home failed"; }
SHIM="$INNER/subos/default/bin/nested-tool"
[[ -e "$SHIM" ]] || { cat "$WORK/h4-install.log"; fail "H4: no shim at $SHIM"; }
out="$(cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$INNER" "$SHIM" 2>&1 || true)"
if grep -q "package file not found" <<<"$out"; then
  fail "H4: the inner home's script path was re-rooted by the outer SubOS: $out"
fi
grep -q "NESTED_TOOL_RAN" <<<"$out" || fail "H4: the nested home's script did not run: $out"
log "  ok"

log "PASS: home identity (H1-H4)"
