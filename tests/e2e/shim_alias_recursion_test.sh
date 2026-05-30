#!/usr/bin/env bash
# E2E test: verify shim alias self-reference does not cause recursion
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/shim_alias_recursion"

source "$ROOT_DIR/tests/e2e/project_test_lib.sh"
BIN_SRC="$(find_xlings_bin)"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -x "$BIN_SRC" ]] || fail "built xlings binary not found at $BIN_SRC"

# --- Setup portable home ---
rm -rf "$RUNTIME_DIR"
mkdir -p "$RUNTIME_DIR/portable/bin"
mkdir -p "$RUNTIME_DIR/portable/subos/default/bin"

cp "$BIN_SRC" "$RUNTIME_DIR/portable/bin/xlings"

# Create xlings config
cat > "$RUNTIME_DIR/portable/.xlings.json" <<EOF
{
  "version": "0.4.0",
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "subos": { "default": { "dir": "" } }
}
EOF

# Init so shims are created
pushd "$RUNTIME_DIR/portable" >/dev/null
env -u XLINGS_HOME ./bin/xlings self init >/dev/null 2>&1 || fail "self init failed"
popd >/dev/null

SHIM_DIR="$RUNTIME_DIR/portable/subos/default/bin"

# --- Create a fake package with alias that references itself ---
FAKE_PKG_DIR="$RUNTIME_DIR/portable/subos/default/pkgs/fakegcc/1.0"
mkdir -p "$FAKE_PKG_DIR/bin"

# Create a real "fakegcc" binary in the package dir
cat > "$FAKE_PKG_DIR/bin/fakegcc" <<'SCRIPT'
#!/bin/sh
echo "real-fakegcc-output"
SCRIPT
chmod +x "$FAKE_PKG_DIR/bin/fakegcc"

# Create version database entry with alias that references the same program name
cat > "$RUNTIME_DIR/portable/subos/default/versions.json" <<EOF
{
  "fakegcc": {
    "versions": {
      "1.0": {
        "path": "pkgs/fakegcc/1.0",
        "alias": ["fakegcc --fake-flag"]
      }
    },
    "active": "1.0"
  }
}
EOF

# Create shim symlink for fakegcc
ln -sf "../../../bin/xlings" "$SHIM_DIR/fakegcc"

# --- Test 1: alias with resolvable binary should NOT recurse ---
# The alias "fakegcc --fake-flag" should resolve to the real binary at
# pkgs/fakegcc/1.0/bin/fakegcc, not loop back through the shim
OUTPUT=$(env -u XLINGS_HOME XLINGS_SHIM_DEPTH=0 \
  "$SHIM_DIR/fakegcc" 2>&1) || true

# Should not contain recursion error
if echo "$OUTPUT" | grep -q "shim recursion detected"; then
  fail "shim recursion detected when real binary exists — alias was not resolved to full path"
fi

echo "PASS: shim alias self-reference resolved correctly (no recursion)"

# --- Test 2: alias without resolvable binary should give clear error ---
FAKE_PKG_DIR2="$RUNTIME_DIR/portable/subos/default/pkgs/nobinary/1.0"
mkdir -p "$FAKE_PKG_DIR2"

# Update versions.json to add a package with alias but no real binary
cat > "$RUNTIME_DIR/portable/subos/default/versions.json" <<EOF
{
  "fakegcc": {
    "versions": {
      "1.0": {
        "path": "pkgs/fakegcc/1.0",
        "alias": ["fakegcc --fake-flag"]
      }
    },
    "active": "1.0"
  },
  "nobinary": {
    "versions": {
      "1.0": {
        "path": "pkgs/nobinary/1.0",
        "alias": ["nobinary --flag"]
      }
    },
    "active": "1.0"
  }
}
EOF

ln -sf "../../../bin/xlings" "$SHIM_DIR/nobinary"

OUTPUT2=$(env -u XLINGS_HOME XLINGS_SHIM_DEPTH=0 \
  "$SHIM_DIR/nobinary" 2>&1) || true

if echo "$OUTPUT2" | grep -q "shim recursion detected"; then
  fail "got recursion error instead of clear self-reference error"
fi

if echo "$OUTPUT2" | grep -q "references itself but real binary not found"; then
  echo "PASS: clear error message when alias self-references without real binary"
else
  echo "WARN: expected self-reference error message, got: $OUTPUT2"
fi

# --- Cleanup ---
rm -rf "$RUNTIME_DIR"

echo "PASS: all shim alias recursion tests passed"
