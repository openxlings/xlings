#!/usr/bin/env bash
# E2E test: a non-versions write must not resurrect a STALE versions DB.
#
# During the dual-write window the DB file (<home>/data/versions.json) is
# trusted only while its stamp names the home config's current stat. Every
# config RMW of some other key (`xlings config`, `subos use`, hints, doctor's
# verified stamp) carries that stamp forward -- and once did so
# unconditionally. Reproduced on PR #639: an older client added `oldpkg` to
# the config only; a new client's `config --mirror` re-stamped the stale DB;
# the next new-client save_versions loaded that DB and wrote it back, and
# `oldpkg` vanished from BOTH copies. The re-stamp now requires the DB's
# versions map to equal the one just written.
#
# The old client is simulated at the data level (see dual_write_window_test.sh
# for why a released xlings is not pinned here): it edits the config's
# `versions` and nothing else -- not `dbIndex`, not the DB file.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/dual_write_restamp"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

mkdir -p "$HOME_DIR/subos/default/bin"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/p"
cp "$ROOT_DIR/tests/e2e/fixtures/subos_xpkg/py-demo.lua" \
   "$LOCAL_INDEX_DIR/pkgs/p/py-demo.lua"
cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "mirror": "GLOBAL",
  "index_repos": [ { "name": "xim", "url": "$LOCAL_INDEX_DIR" } ]
}
JSON
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

run_xlings "$HOME_DIR" "$ROOT_DIR" self init >/dev/null 2>&1 || fail "self init failed"

stamp_of_db() {
  python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['stamp'])" \
    "$HOME_DIR/data/versions.json"
}
has_key() {  # has_key <file> <dotted-wrapper-or-empty> <name>
  python3 - "$@" <<'PY'
import json, sys
path, wrapper, name = sys.argv[1:4]
d = json.load(open(path))
if wrapper:
    d = d[wrapper]
print("yes" if name in d.get("versions", {}) else "no")
PY
}

# ── S1: new client installs → DB written and stamped ─────────────────
run_xlings "$HOME_DIR" "$ROOT_DIR" install subos:py-demo@1.0.0 -y >/dev/null 2>&1 \
  || fail "new-client install failed"
[[ -f "$HOME_DIR/data/versions.json" ]] || fail "S1: DB file not written by install"
log "S1: install writes a stamped DB: ok"

# ── S2: old client installs oldpkg into the config only ──────────────
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
p = home / ".xlings.json"
d = json.loads(p.read_text())
d.setdefault("versions", {})["oldpkg"] = {
    "type": "program", "filename": "oldpkg",
    "versions": {"1.0": {"path": str(home / "data" / "xpkgs" / "xim-x-oldpkg" / "1.0" / "bin")}}}
p.write_text(json.dumps(d, indent=2))
PY
stale_stamp="$(stamp_of_db)"
[[ "$(has_key "$HOME_DIR/data/versions.json" "" oldpkg)" == no ]] \
  || fail "S2 setup: oldpkg must not be in the DB file"
log "S2: old-client write landed in the config only: ok"

# ── S3: new client RMWs another key → the stale DB stays stale ───────
# Equal-length rewrites inside one timestamp tick are the stamp's known
# residual; step past it so this scenario tests the guard, not the clock.
sleep 1.1
run_xlings "$HOME_DIR" "$ROOT_DIR" config --mirror GLOBAL >/dev/null 2>&1 \
  || fail "config --mirror failed"
[[ "$(stamp_of_db)" == "$stale_stamp" ]] \
  || fail "S3: a non-versions RMW re-stamped a DB that no longer mirrors the config"
log "S3: stale DB not re-stamped by a non-versions write: ok"

# ── S4: new client saves versions → the old client's record survives ─
run_xlings "$HOME_DIR" "$ROOT_DIR" remove py-demo -y >/dev/null 2>&1 \
  || fail "new-client remove failed"
[[ "$(has_key "$HOME_DIR/.xlings.json" "" oldpkg)" == yes ]] \
  || fail "S4: the old client's oldpkg was deleted from the home config"
[[ "$(has_key "$HOME_DIR/data/versions.json" "" oldpkg)" == yes ]] \
  || fail "S4: the rewritten DB file does not carry oldpkg"
log "S4: save_versions kept the old client's record in both copies: ok"

# ── S5: a fresh DB IS carried forward by a non-versions write ────────
# The guard must not cost the healthy case: after S4 both copies agree, so
# an RMW re-stamps and the DB stays trusted.
sleep 1.1
before="$(stamp_of_db)"
run_xlings "$HOME_DIR" "$ROOT_DIR" config --mirror GLOBAL >/dev/null 2>&1 \
  || fail "config --mirror failed"
after="$(stamp_of_db)"
cfg_size="$(wc -c < "$HOME_DIR/.xlings.json" | tr -d ' ')"
[[ "$after" != "$before" && "$after" == "$cfg_size:"* ]] \
  || fail "S5: a matching DB was not re-stamped (before=$before after=$after cfg=$cfg_size)"
log "S5: matching DB re-stamped by a non-versions write: ok"

# ── S6: every other home-config writer carries the stamp too ─────────
# `self init` rewrites the home config directly (not through an RMW
# helper). A writer that forgot the stamp would not lose data -- readers
# fall back -- but would drop the home off the shim fast path until the
# next install.
sleep 1.1
before="$(stamp_of_db)"
run_xlings "$HOME_DIR" "$ROOT_DIR" self init >/dev/null 2>&1 || fail "self init failed"
after="$(stamp_of_db)"
# The write always moves the config's mtime, so a carried-forward stamp
# always changes; an unchanged one is a stale one (equal size alone would
# not show it: init can rewrite identical bytes).
[[ "$after" != "$before" ]] || fail "S6: self init left the versions DB stamp stale ($after)"
cfg_size="$(wc -c < "$HOME_DIR/.xlings.json" | tr -d ' ')"
python3 - "$HOME_DIR" "$after" <<'PY2' || fail "S6: self init left the versions DB stamp stale ($after)"
import os, sys
home, stamp = sys.argv[1], sys.argv[2]
st = os.stat(os.path.join(home, ".xlings.json"))
size, ticks = stamp.split(":")
assert int(size) == st.st_size, (size, st.st_size)
PY2
log "S6: self init keeps the versions DB stamped: ok"

log "PASS: dual_write_restamp (stale DB never re-trusted, fresh DB carried forward)"
