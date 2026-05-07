#!/usr/bin/env bash
# subos_workspace_c2_schema_test.sh — verifies the 0.4.19 subos workspace
# schema migration:
#
#   1. Pre-0.4.19 string-form workspace value is read without loss.
#   2. The first save_workspace after upgrade rewrites the file in
#      `{active, installed[]}` (C2) form.
#   3. Hand-crafted C2-form input is read correctly (active and
#      installed both populated).
#   4. `xlings use` switching the active version preserves installed[].
#   5. Save invariant: `active` always appears in `installed[]`.
#
# This is a parser/schema test only — it pre-populates the global
# versions DB by hand so it can exercise `xlings use` without needing a
# real package payload or pkgindex network access.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

HOME_DIR="$(runtime_home_dir subos_workspace_c2_schema_home)"
SUBOS_FILE="$HOME_DIR/subos/default/.xlings.json"
XLINGS_BIN="$(find_xlings_bin)"

cleanup() { rm -rf "$HOME_DIR" /tmp/c2_schema_fakebin; }
trap cleanup EXIT

rm -rf "$HOME_DIR"
mkdir -p "$HOME_DIR/subos/default/bin" \
         "$HOME_DIR/subos/default/lib" \
         "$HOME_DIR/subos/default/usr"
mkdir -p /tmp/c2_schema_fakebin/v1 /tmp/c2_schema_fakebin/v2 /tmp/c2_schema_fakebin/v3
for v in v1 v2 v3; do
  : > /tmp/c2_schema_fakebin/$v/tt
  chmod +x /tmp/c2_schema_fakebin/$v/tt
done

cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "activeSubos": "default",
  "versions": {
    "tt": {
      "type": "program",
      "filename": "tt",
      "versions": {
        "1.0.0": {"path": "/tmp/c2_schema_fakebin/v1", "envs": {}, "alias": []},
        "2.0.0": {"path": "/tmp/c2_schema_fakebin/v2", "envs": {}, "alias": []},
        "3.0.0": {"path": "/tmp/c2_schema_fakebin/v3", "envs": {}, "alias": []}
      }
    }
  }
}
JSON

run_x() {
  (
    unset XLINGS_PROJECT_DIR XLINGS_ACTIVE_SUBOS
    XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@"
  )
}

# Tiny python helper — read the workspace value for "tt" out of the
# subos file as JSON. Robust to either shape.
read_tt() {
  python3 - "$SUBOS_FILE" <<'PY'
import json, sys, pathlib
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
print(json.dumps((data.get("workspace") or {}).get("tt")))
PY
}

# ------------------------------------------------------------------ Test 1
log "T1: legacy string-form input survives load + lazy-migrates on save"
cat > "$SUBOS_FILE" <<JSON
{
  "workspace": {
    "tt": "1.0.0"
  }
}
JSON

run_x use tt 2.0.0 >/dev/null
RAW="$(read_tt)"
echo "  after use 2.0.0: $RAW"
case "$RAW" in
  '{"active": "2.0.0", "installed": ["2.0.0"]}'|\
  '{"active":"2.0.0","installed":["2.0.0"]}')
    ;;
  *)
    fail "T1: expected {active:2.0.0, installed:[2.0.0]} after lazy migration; got: $RAW" ;;
esac

# ------------------------------------------------------------------ Test 2
log "T2: hand-crafted C2 input parses; switching active preserves installed[]"
cat > "$SUBOS_FILE" <<JSON
{
  "workspace": {
    "tt": {
      "active": "2.0.0",
      "installed": ["1.0.0", "2.0.0", "3.0.0"]
    }
  }
}
JSON

run_x use tt 1.0.0 >/dev/null
RAW="$(read_tt)"
echo "  after use 1.0.0: $RAW"
ACTIVE="$(echo "$RAW" | python3 -c 'import sys,json; print(json.loads(sys.stdin.read())["active"])')"
INSTALLED="$(echo "$RAW" | python3 -c 'import sys,json; print(",".join(json.loads(sys.stdin.read())["installed"]))')"
[[ "$ACTIVE" == "1.0.0" ]] || fail "T2: active should be 1.0.0, got '$ACTIVE'"
[[ "$INSTALLED" == "1.0.0,2.0.0,3.0.0" ]] \
  || fail "T2: installed[] should preserve all three versions, got '$INSTALLED'"

# ------------------------------------------------------------------ Test 3
log "T3: save invariant — active is always present in installed[]"
# Hand-craft a file where active is NOT in installed[]. After save, the
# serializer should add it back.
cat > "$SUBOS_FILE" <<JSON
{
  "workspace": {
    "tt": {
      "active": "3.0.0",
      "installed": ["1.0.0"]
    }
  }
}
JSON

run_x use tt 3.0.0 >/dev/null
RAW="$(read_tt)"
echo "  after use 3.0.0 (re-saving): $RAW"
INSTALLED="$(echo "$RAW" | python3 -c 'import sys,json; print(",".join(json.loads(sys.stdin.read())["installed"]))')"
case "$INSTALLED" in
  *3.0.0*) ;;
  *) fail "T3: active 3.0.0 should be present in installed[] after save; got: $INSTALLED" ;;
esac

# ------------------------------------------------------------------ Test 4
log "T4: read path tolerates a 'bare object' (no active, only installed[])"
cat > "$SUBOS_FILE" <<JSON
{
  "workspace": {
    "tt": {
      "installed": ["1.0.0", "2.0.0"]
    }
  }
}
JSON

# `xlings use tt 2.0.0` should set active without erroring on the
# missing-active-on-load case.
run_x use tt 2.0.0 >/dev/null
RAW="$(read_tt)"
echo "  after use 2.0.0 from active-less input: $RAW"
ACTIVE="$(echo "$RAW" | python3 -c 'import sys,json; print(json.loads(sys.stdin.read())["active"])')"
[[ "$ACTIVE" == "2.0.0" ]] || fail "T4: expected active=2.0.0, got '$ACTIVE'"

log "PASS: subos_workspace_c2_schema"
