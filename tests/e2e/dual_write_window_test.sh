#!/usr/bin/env bash
# E2E test: the dual-write window between an OLD client (which edits only
# the home config's `versions` field) and a NEW client (which prefers
# data/versions.json). This is the shape PR-review P0 reproduced: a client
# that has never heard of the DB file installs or removes a package, and
# afterwards the new client's shim dispatch must agree with the config.
#
# The old client is simulated at the data level — write the home config the
# way every ≤2026.9.30.1 client does (edit `versions` + `dbIndex`, never
# touch data/versions.json) — because the alternative is pinning a released
# xlings into CI, which the fresh-install rules forbid. What the simulation
# cannot fake is the stat change, and the write below IS a real write, so
# the config's size+mtime genuinely move, exactly as an old client's write
# would.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/dual_write_window"
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
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [ { "name": "xim", "url": "$LOCAL_INDEX_DIR" } ]
}
EOF
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

run_xlings "$HOME_DIR" "$ROOT_DIR" self init >/dev/null 2>&1 || fail "self init failed"

# A home id marker: the shim dispatch on PATH anchors to the home that owns
# the shim file, recognized by this marker (every home created since
# 2026.9.28.2 has one; `self init` on older layouts writes it on first CLI
# command too). Without it the shim below would resolve against whatever
# home the RUNNING binary came from, not this isolated one.
touch "$HOME_DIR/.xlings-home"

# ── S1: new client installs → DB file exists and is trusted ──────────
run_xlings "$HOME_DIR" "$ROOT_DIR" install subos:py-demo@1.0.0 -y >/dev/null 2>&1 \
  || fail "new-client install failed"
[[ -f "$HOME_DIR/data/versions.json" ]] || fail "DB file not written by install"
python3 - "$HOME_DIR/data/versions.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("format") == 1, f"DB wrapper missing format: {list(d)}"
assert "stamp" in d and "versions" in d, f"DB wrapper missing stamp/versions"
assert "py-demo" in d["versions"], f"py-demo missing from DB file"
PY
log "S1: install writes a stamped DB wrapper: ok"

# ── S2: OLD client installs another package (config-only write) ──────
#
# Every client ≤2026.9.30.1 does exactly this: read the config, add to
# `versions` AND to the subos's workspace (an install also activates), write
# both files. It does NOT touch `dbIndex` -- it has never heard of it, and
# carries the key through untouched, now stale. The DB file is left with a stamp that no
# longer matches. The workspace entry is what makes the package real for
# dispatch; the versions entry is what find_vinfo must resolve it against.
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
p = home / ".xlings.json"
d = json.loads(p.read_text())
d.setdefault("versions", {})["oldpkg"] = {
    "type": "program", "filename": "oldpkg",
    "versions": {"1.0": {"path": str(home / "data" / "xpkgs" / "xim-x-oldpkg" / "1.0" / "bin"),
                          "kind": "program"}}}
p.write_text(json.dumps(d, indent=2))
ws = home / "subos" / "default" / ".xlings.json"
w = json.loads(ws.read_text())
entry = w.setdefault("workspace", {}).setdefault("oldpkg", {})
entry["active"] = "1.0"
entry["installed"] = ["1.0"]
w.setdefault("installed", {})["oldpkg"] = ["1.0"]
ws.write_text(json.dumps(w, indent=2))
PY
# The DB file still names only py-demo. The new client must see BOTH.
python3 -c "
import json
db = json.load(open('$HOME_DIR/data/versions.json'))['versions']
assert 'oldpkg' not in db, 'oldpkg should not be in the (stale) DB file'
"
python3 -c "
import json
cfg = json.load(open('$HOME_DIR/.xlings.json'))
assert 'oldpkg' in cfg['versions'] and 'py-demo' in cfg['versions']
"
log "S2: old-client write landed in the config only: ok"

# ── S3: new client dispatch reads the old client's entry ─────────────
#
# The workspace (S2's write) says oldpkg@1.0 is ACTIVE. Dispatch must
# resolve its vdata from the home config — the DB file, stale, names no
# oldpkg. With the stamp bypass working, dispatch gets past "not
# installed" and fails downstream (the payload directory does not exist,
# exec fails); with the bug this scenario reproduces, the stale DB file
# shadows the config and dispatch reports "not installed in this subos".
ln -sf xlings "$HOME_DIR/subos/default/bin/oldpkg"
OUT=$("$HOME_DIR/subos/default/bin/oldpkg" --version 2>&1 || true)
if echo "$OUT" | grep -q "not installed in this subos"; then
  fail "dispatch read the STALE DB file (oldpkg invisible); got:\n$OUT"
fi
log "S3: dispatch sees the old client's active entry (stamp bypass works): ok"

# ── S4: old client REMOVES → new client must not run a dead payload ──
#
# An old client's remove edits the config (versions + workspace) and leaves
# both the DB file and the config's own `dbIndex` naming py-demo. The new
# client's dispatch must agree with the config: py-demo is gone.
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
p = home / ".xlings.json"
d = json.loads(p.read_text())
d["versions"].pop("py-demo", None)
p.write_text(json.dumps(d, indent=2))
ws = home / "subos" / "default" / ".xlings.json"
w = json.loads(ws.read_text())
w.get("workspace", {}).pop("py-demo", None)
w.get("installed", {}).pop("py-demo", None)
ws.write_text(json.dumps(w, indent=2))
PY
python3 -c "
import json
db = json.load(open('$HOME_DIR/data/versions.json'))['versions']
assert 'py-demo' in db, 'py-demo should still be in the (stale) DB file'
cfg = json.load(open('$HOME_DIR/.xlings.json'))
assert 'py-demo' not in cfg.get('versions', {}), 'py-demo should be gone from the config'
assert 'py-demo' in cfg.get('dbIndex', {}), 'an old client leaves dbIndex stale'
"
# Keep a shim for the removed name and DISPATCH it: the stale DB file and the
# stale dbIndex both still name py-demo, and neither may make it runnable.
# The new client must agree with the config: nothing is active, so the
# dispatch fails (a "not installed" diagnostic, or the name handed back to
# PATH where nothing provides it) -- it must never resolve a payload.
ln -sf xlings "$HOME_DIR/subos/default/bin/py-demo"
rc=0
OUT=$("$HOME_DIR/subos/default/bin/py-demo" --version 2>&1) || rc=$?
[[ $rc -ne 0 ]] \
  || fail "dispatch still succeeded for py-demo after the old client removed it; got:\n$OUT"
log "S4: removed name does not dispatch (rc=$rc): ok"
log "PASS: dual_write_window (old-client writes stay visible to new-client reads)"
