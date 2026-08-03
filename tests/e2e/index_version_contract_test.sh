#!/usr/bin/env bash
# E2E-59 (#476): the index declares which client versions it needs, and the
# client routes itself to the newest snapshot it satisfies.
#
#   A. newest snapshot is reachable        -> it is used
#   B. newest snapshot is out of reach     -> the newest COMPATIBLE one is used,
#                                             and the reason is printed
#   C. nothing is compatible               -> hard error, local tree untouched
#   D. pin to a published snapshot         -> exactly that one
#   E. pin to an unpublished version       -> hard error listing what exists
#   F. XLINGS_INDEX_PIN=newest             -> reaches the newest anyway
#                                             (this is what `self update` uses,
#                                              and what keeps a routed-back
#                                              client from being stranded)
#
# Hermetic: the MAIN index is served from a local XLINGS_INDEX_BASE_URL dir, so
# this exercises the same path xlings itself walks -- not a custom-source
# special case.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/project_test_lib.sh"

XLINGS_BIN="${1:-$(find_xlings_bin)}"
[[ -x "$XLINGS_BIN" ]] || { echo "[test] FAIL: no xlings binary found" >&2; exit 1; }

pass() { echo "[test] OK: $*"; }
fail() { echo "[test] FAIL: $*" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xim-vcontract.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

SELF_VER="$("$XLINGS_BIN" --version | awk '{print $2}')"
[[ -n "$SELF_VER" ]] || fail "could not read the binary's own version"
echo "[test] binary under test: $SELF_VER"

SERVE="$WORK/serve"; mkdir -p "$SERVE"

# Three snapshots, each carrying a marker package that names it, so "which one
# landed" is answered by the tree on disk rather than by a log line.
build_snapshot() {  # <version> <marker>
  local ver="$1" marker="$2" src="$WORK/src-$1"
  mkdir -p "$src/pkgs/m"
  printf 'package({name="%s"})\n' "$marker" > "$src/pkgs/m/$marker.lua"
  bash "$PROJECT_DIR/tools/build_xim_index_artifact.sh" \
    --version "$ver" --out "$SERVE" --src "$src" >/dev/null
}
build_snapshot old0001 marker-old
build_snapshot mid0002 marker-mid
build_snapshot new0003 marker-new

# Assemble a pointer with history. `requires` on the newest is the knob every
# scenario below turns.
write_pointer() {  # <newest-min> <mid-min> [old-min]
  python3 - "$SERVE" "$1" "$2" "${3:-}" <<'PY'
import json, sys, pathlib
serve, newest_min, mid_min, old_min = sys.argv[1:5]
def man(v):
    return json.load(open(pathlib.Path(serve) / f"xim-index-{v}.manifest.json"))
def entry(v, req):
    m = man(v)
    e = {"index_version": m["index_version"], "generated_at": m.get("generated_at", ""),
         "artifact": m["artifact"]}
    if req: e["requires"] = {"xlings": {"min": req}}
    return e
head = man("new0003")
if newest_min: head["requires"] = {"xlings": {"min": newest_min}}
head["history"] = [entry("new0003", newest_min), entry("mid0002", mid_min),
                   entry("old0001", old_min)]
head["history_truncated"] = False
out = {"format_version": 2, "indexes": {"xim": head},
       "client_latest": {"xlings": "9999.1.1.1"}}
json.dump(out, open(pathlib.Path(serve) / "xim-index-pointers.json", "w"), indent=2)
PY
}

HOME_DIR="$WORK/home"; USER_DIR="$WORK/user"; mkdir -p "$HOME_DIR" "$USER_DIR"
cat > "$HOME_DIR/.xlings.json" <<EOF
{ "mirror": "GLOBAL" }
EOF

run() {
  HOME="$USER_DIR" XLINGS_HOME="$HOME_DIR" NO_COLOR=1 XLINGS_NON_INTERACTIVE=1 \
  XLINGS_INDEX_BASE_URL="$SERVE" XLINGS_INDEX_SOURCE=artifact \
    "$XLINGS_BIN" "$@" 2>&1
}
landed() { ls "$HOME_DIR/data/xim-pkgindex/pkgs/m/" 2>/dev/null | head -1; }

# ── A: the newest snapshot is within reach ───────────────────────
write_pointer "" ""
out="$(run update)" || fail "A: update failed: $out"
[[ "$(landed)" == "marker-new.lua" ]] \
  || fail "A: expected the newest snapshot, got '$(landed)'"
grep -q "instead of" <<<"$out" && fail "A: reported routing back when it did not"
pass "A: unconstrained pointer uses the newest snapshot"

# ── B: newest is out of reach -> newest COMPATIBLE, and say why ──
write_pointer "9999.1.1.1" ""
out="$(run update)" || fail "B: update failed: $out"
[[ "$(landed)" == "marker-mid.lua" ]] \
  || fail "B: expected to route back to mid0002, got '$(landed)'"
grep -q "new0003" <<<"$out" || fail "B: did not name the snapshot it skipped: $out"
grep -q "9999.1.1.1" <<<"$out" || fail "B: did not state the requirement: $out"
grep -qE "self update|upgrade" <<<"$out" || fail "B: did not say how to fix it: $out"
pass "B: routes back to the newest compatible snapshot and explains why"

# ── C: nothing compatible -> hard error, tree left alone ─────────
before="$(landed)"
write_pointer "9999.1.1.1" "9999.1.1.1" "9999.1.1.1"
set +e; out="$(run update)"; rc=$?; set -e
[[ $rc -ne 0 ]] || fail "C: no compatible snapshot was not an error: $out"
grep -q "E_INDEX_NO_COMPATIBLE_SNAPSHOT" <<<"$out" \
  || fail "C: error was not the causal one: $out"
[[ "$(landed)" == "$before" ]] \
  || fail "C: local index tree was replaced during a failed resolve"
pass "C: no compatible snapshot fails loudly and leaves the tree alone"

# ── D: pin selects exactly, contract or not ──────────────────────
write_pointer "9999.1.1.1" ""
out="$(XLINGS_INDEX_PIN=old0001 run update)" || fail "D: pinned update failed: $out"
[[ "$(landed)" == "marker-old.lua" ]] \
  || fail "D: pin did not select old0001, got '$(landed)'"
pass "D: a pin selects exactly the snapshot named"

# ── E: pin to something unpublished -> error naming what exists ──
before="$(landed)"
set +e; out="$(XLINGS_INDEX_PIN=deadbee run update)"; rc=$?; set -e
[[ $rc -ne 0 ]] || fail "E: unknown pin was accepted: $out"
grep -q "E_INDEX_VERSION_NOT_FOUND" <<<"$out" || fail "E: wrong error: $out"
grep -q "new0003" <<<"$out" || fail "E: did not list available versions: $out"
[[ "$(landed)" == "$before" ]] || fail "E: tree replaced on a failed pin"
pass "E: an unpublished pin errors and lists what is available"

# ── F: the escape hatch reaches the newest regardless ────────────
#
# This is the one that keeps a routed-back client from being stranded: without
# it, an old client sits on an old snapshot whose own xlings recipe names an old
# `latest`, is told it is current, and can never move.
out="$(XLINGS_INDEX_PIN=newest run update)" || fail "F: escape hatch failed: $out"
[[ "$(landed)" == "marker-new.lua" ]] \
  || fail "F: XLINGS_INDEX_PIN=newest did not reach the newest, got '$(landed)'"
pass "F: the upgrade path reaches the newest snapshot despite the contract"

# ── G: `xlings index list` reports what routing would pick ───────
write_pointer "9999.1.1.1" ""
run update >/dev/null || fail "G: update failed"
out="$(run index list xim)"
grep -q "mid0002" <<<"$out" || fail "G: list did not mention the routed snapshot: $out"
grep -qE '^\s*\*\s*mid0002' <<<"$out" \
  || fail "G: list did not mark the CURRENT snapshot: $out"
grep -q "new0003" <<<"$out" || fail "G: list hid the newer snapshot: $out"

json="$(run index list xim --json)"
python3 - "$json" <<'PYJ'
import json, sys
data = json.loads(sys.argv[1])
xim = next(e for e in data if e["name"] == "xim")
assert xim["current"] == "mid0002", xim["current"]
rows = {s["index_version"]: s for s in xim["snapshots"]}
assert rows["mid0002"]["current"] is True
assert rows["new0003"]["current"] is False
# The contract is handed back verbatim -- xlings must not normalise a document
# it does not own.
assert rows["new0003"]["requires"] == {"xlings": {"min": "9999.1.1.1"}}, \
    rows["new0003"]["requires"]
PYJ
pass "G: index list reports the routed snapshot, and --json carries the contract"

# ── H: `index use` pins, and refuses a version that does not exist ──
run index use xim old0001 >/dev/null || fail "H: pinning failed"
run update >/dev/null || fail "H: update after pin failed"
[[ "$(landed)" == "marker-old.lua" ]] || fail "H: pin did not take effect"

set +e; out="$(run index use xim deadbee)"; rc=$?; set -e
[[ $rc -ne 0 ]] || fail "H: `index use` accepted an unpublished snapshot"
grep -q "available" <<<"$out" || fail "H: refusal did not list what exists: $out"

run index use xim latest >/dev/null || fail "H: unpinning failed"
run update >/dev/null || fail "H: update after unpin failed"
[[ "$(landed)" == "marker-mid.lua" ]] \
  || fail "H: unpin did not return to automatic routing, got '$(landed)'"
pass "H: index use pins, refuses unknown versions, and unpins"

echo "[test] index version contract: all scenarios passed"
