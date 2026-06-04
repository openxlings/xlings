#!/usr/bin/env bash
# shim_owner_anchoring_test.sh — owner-anchored shim dispatch (0.4.48).
#
# Design: .agents/docs/2026-06-04-shim-owner-anchoring-design.md
#
# A shim is an artifact of exactly one home: invoking it must resolve
# against that home (owner), regardless of ambient XLINGS_HOME. The env
# var stays as a deprecated lower-priority fallback ("borrowing") for one
# transition window, and XLINGS_SHIM_ANCHOR=legacy restores the pre-0.4.48
# env-first behavior.
#
# Scenarios:
#   1. owner wins: tool installed in home A (shim's owner) AND in env home
#      B with a different payload → A's payload runs + ambiguity warning.
#   2. borrowing fallback: shim's owner does not know the tool, env home
#      does → env home's payload runs + deprecation warning.
#   3. legacy mode: XLINGS_SHIM_ANCHOR=legacy restores env-first → B wins.
#   4. redirected-env regression (the mcpp case): tool ONLY in owner home,
#      XLINGS_HOME points at a fresh foreign home → tool still runs.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/shim_owner_anchoring"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

HOME_A="$RUNTIME_DIR/home-a"
HOME_B="$RUNTIME_DIR/home-b"
HOME_FRESH="$RUNTIME_DIR/home-fresh"   # fresh foreign home (mcpp registry shape)
WORK_DIR="$RUNTIME_DIR/work"

# make_home <dir> — fabricate a structural home: .xlings.json + bin/xlings
# + subos/default/bin (the owner-anchoring signature).
make_home() {
  local home="$1"
  mkdir -p "$home/subos/default/bin" "$home/bin"
  cp "$XLINGS_BIN" "$home/bin/xlings"
  cat > "$home/.xlings.json" <<JSON
{ "activeSubos": "default", "mirror": "GLOBAL", "versions": {} }
JSON
  cat > "$home/subos/default/.xlings.json" <<JSON
{ "workspace": {} }
JSON
}

# install_tool <home> <tool> <marker> — register <tool>@1.0.0 in <home>'s
# versions DB + workspace, with a payload script that prints <marker>.
install_tool() {
  local home="$1" tool="$2" marker="$3"
  local payload="$home/data/xpkgs/$tool/1.0.0"
  mkdir -p "$payload/bin"
  cat > "$payload/bin/$tool" <<SCRIPT
#!/usr/bin/env bash
echo "$marker"
SCRIPT
  chmod +x "$payload/bin/$tool"

  python3 - "$home/.xlings.json" "$tool" "$payload" <<'PY'
import json, sys
path, tool, payload = sys.argv[1:4]
with open(path) as f:
    cfg = json.load(f)
cfg.setdefault("versions", {})[tool] = {
    "type": "program",
    "versions": {"1.0.0": {"path": payload}},
}
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
PY

  python3 - "$home/subos/default/.xlings.json" "$tool" <<'PY'
import json, sys
path, tool = sys.argv[1:3]
with open(path) as f:
    cfg = json.load(f)
cfg.setdefault("workspace", {})[tool] = "1.0.0"
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
PY

  # The shim: the home's own xlings binary, named after the tool
  # (matches xself::create_shim's symlink form on Unix).
  ln -sf "$home/bin/xlings" "$home/subos/default/bin/$tool"
}

run_shim() {
  # run_shim <shim-path> [env overrides as VAR=val ...]
  local shim="$1"; shift
  (cd "$WORK_DIR" && env -u XLINGS_PROJECT_DIR -u XLINGS_ACTIVE_SUBOS \
      XLINGS_SHIM_DEPTH=0 "$@" "$shim" 2>"$WORK_DIR/stderr.txt")
}

mkdir -p "$WORK_DIR"
make_home "$HOME_A"
make_home "$HOME_B"
make_home "$HOME_FRESH"

install_tool "$HOME_A" "anchortool" "payload-from-A"
install_tool "$HOME_B" "anchortool" "payload-from-B"
install_tool "$HOME_B" "borrowtool" "borrow-from-B"

SHIM_A="$HOME_A/subos/default/bin/anchortool"

# ── 1. owner wins over env (both homes know the tool) ────────────────
out="$(run_shim "$SHIM_A" XLINGS_HOME="$HOME_B")"
[[ "$out" == "payload-from-A" ]] \
  || fail "owner anchoring: expected payload-from-A, got '$out'"
grep -q "deprecated" "$WORK_DIR/stderr.txt" \
  || fail "owner anchoring: expected ambiguity warning on stderr"
log "1. owner wins over env XLINGS_HOME — ok"

# ── 2. borrowing fallback (owner doesn't know, env does) ─────────────
ln -sf "$HOME_A/bin/xlings" "$HOME_A/subos/default/bin/borrowtool"
out="$(run_shim "$HOME_A/subos/default/bin/borrowtool" XLINGS_HOME="$HOME_B")"
[[ "$out" == "borrow-from-B" ]] \
  || fail "borrowing fallback: expected borrow-from-B, got '$out'"
grep -q "deprecated" "$WORK_DIR/stderr.txt" \
  || fail "borrowing fallback: expected deprecation warning on stderr"
log "2. env borrowing fallback (deprecated) — ok"

# ── 3. legacy mode restores env-first dispatch ───────────────────────
out="$(run_shim "$SHIM_A" XLINGS_HOME="$HOME_B" XLINGS_SHIM_ANCHOR=legacy)"
[[ "$out" == "payload-from-B" ]] \
  || fail "legacy mode: expected payload-from-B, got '$out'"
log "3. XLINGS_SHIM_ANCHOR=legacy restores env-first — ok"

# ── 4. redirected-env regression (the mcpp case) ─────────────────────
# A fresh foreign home (like mcpp's registry) knows nothing; pre-0.4.48
# this failed with "'anchortool' is not installed".
out="$(run_shim "$SHIM_A" XLINGS_HOME="$HOME_FRESH")"
[[ "$out" == "payload-from-A" ]] \
  || fail "redirected env: expected payload-from-A, got '$out'"
log "4. shim works under redirected XLINGS_HOME (mcpp regression) — ok"

log "PASS: shim owner anchoring"
