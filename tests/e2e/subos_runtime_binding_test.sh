#!/usr/bin/env bash
# E2E: the subos↔runtime binding is a creation-time property, and moving the
# built-in default must only affect NEW subos (C1 of the closure contract,
# .agents/docs/2026-08-09-ecosystem-closure-design.md).
#
# Three claims, each falsifiable:
#
#   S1  a fresh subos records the CURRENT default runtime, and records the
#       host's glibc version next to it (rule A needs the right-hand side as
#       of creation, not as of whenever someone later asks)
#   S2  an explicit --runtime is recorded verbatim -- the default is a
#       fallback, not a policy
#   S3  repairing a broken subos_info block PRESERVES a valid recorded
#       runtime. Before this round `--fix` rewrote the block with
#       DEFAULT_RUNTIME unconditionally, so a default bump would silently
#       re-declare every repaired subos against a libc its payloads were
#       never built for.
#
# The expected default IS pinned here ("glibc@2.44"). This is deliberate and
# is not the "assert only invariants" trap: that lesson is about asserting
# version PROPAGATION across repos, while this file tests this repo's own
# contract -- "what runtime does a fresh subos get" is exactly the behavior
# that changed. A future default bump updates one line here, knowingly.

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

EXPECTED_DEFAULT="glibc@2.44"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_runtime_binding"
HOME_DIR="$RUNTIME_DIR/home"
EMPTY_INDEX="$RUNTIME_DIR/empty-index"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$HOME_DIR" "$EMPTY_INDEX/pkgs"

BIN="$(find_xlings_bin)"
log "client: $("$BIN" --version 2>&1 | head -1)"

# An explicit --runtime makes `subos new` INSTALL that runtime. This test is
# about the manifest, not the install, and an isolated home with no index
# configured would fall back to cloning the default index from the network --
# which is a multi-minute hang, not a failure. An empty local index makes the
# resolve fail instantly instead; the manifest is written before the install
# step, so every assertion below still holds.
printf 'xim_indexrepos = {}\n' > "$EMPTY_INDEX/xim-indexrepos.lua"
mkdir -p "$HOME_DIR/data/xim-index-repos"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$EMPTY_INDEX" }
  ]
}
EOF
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$BIN" "$@" ) }

x self init >/dev/null 2>&1 || true

read_block() {  # <subos-dir> <python-expr over blk>
  python3 - "$1" "$2" <<'PY'
import json, sys, pathlib
d = json.loads(pathlib.Path(sys.argv[1], ".xlings.json").read_text())
blk = d.get("subos_info", {})
print(eval(sys.argv[2], {}, {"blk": blk}))
PY
}

# ── S1: fresh default subos gets the current default + host_glibc ───────
DEFAULT_DIR="$HOME_DIR/subos/default"
[ -f "$DEFAULT_DIR/.xlings.json" ] || fail "self init produced no default subos manifest"

RUNTIME="$(read_block "$DEFAULT_DIR" 'blk.get("runtime")')"
[ "$RUNTIME" = "$EXPECTED_DEFAULT" ] \
  || fail "S1: fresh subos runtime is '$RUNTIME', expected '$EXPECTED_DEFAULT'"
log "  ✓ S1a: fresh subos records runtime $EXPECTED_DEFAULT"

HOST_GLIBC_EXPECTED="$(/usr/bin/getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')"
HOST_GLIBC_RECORDED="$(read_block "$DEFAULT_DIR" 'blk.get("host_glibc", "")')"
if [ -n "$HOST_GLIBC_EXPECTED" ]; then
  [ "$HOST_GLIBC_RECORDED" = "$HOST_GLIBC_EXPECTED" ] \
    || fail "S1: host_glibc recorded as '$HOST_GLIBC_RECORDED', host says '$HOST_GLIBC_EXPECTED'"
  log "  ✓ S1b: host_glibc recorded ($HOST_GLIBC_RECORDED)"
else
  # Non-glibc host: absent is the documented spelling of unknown.
  [ -z "$HOST_GLIBC_RECORDED" ] \
    || fail "S1: host has no glibc but manifest claims host_glibc='$HOST_GLIBC_RECORDED'"
  log "  ✓ S1b: non-glibc host, host_glibc absent as documented"
fi

# ── S2: explicit --runtime is recorded verbatim ─────────────────────────
# The trailing install of glibc@2.39 fails against the empty index (see
# above); the binding is a creation-time property and is already on disk.
OUT="$(x subos new t39 --runtime glibc@2.39 2>&1)" || true
[ -f "$HOME_DIR/subos/t39/.xlings.json" ] \
  || { echo "$OUT" >&2; fail "S2: subos new left no manifest at all"; }
RUNTIME39="$(read_block "$HOME_DIR/subos/t39" 'blk.get("runtime")')"
[ "$RUNTIME39" = "glibc@2.39" ] \
  || fail "S2: explicit --runtime glibc@2.39 recorded as '$RUNTIME39'"
log "  ✓ S2: explicit --runtime recorded verbatim"

# ── S3: repair preserves a valid recorded runtime ───────────────────────
python3 - "$DEFAULT_DIR/.xlings.json" <<'PY'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text())
# Invalid block (schema + envs), valid runtime that is NOT the default.
d["subos_info"] = {"schema_version": 99, "runtime": "glibc@2.39", "envs": 42}
p.write_text(json.dumps(d, indent=2))
PY

FIX_OUT="$(x self doctor --fix 2>&1)" || true
printf '%s\n' "$FIX_OUT" | strip_ansi | grep -q "described subos" \
  || fail "S3: --fix did not rebuild the broken block at all:
$FIX_OUT"

FIXED_RUNTIME="$(read_block "$DEFAULT_DIR" 'blk.get("runtime")')"
[ "$FIXED_RUNTIME" = "glibc@2.39" ] \
  || fail "S3: --fix reset runtime to '$FIXED_RUNTIME' -- a repair changed what the subos IS"
log "  ✓ S3a: repair preserved runtime glibc@2.39 (not reset to default)"

FIXED_SCHEMA="$(read_block "$DEFAULT_DIR" 'blk.get("schema_version")')"
[ "$FIXED_SCHEMA" = "1" ] || fail "S3: block not actually repaired (schema_version=$FIXED_SCHEMA)"
FIXED_ENVS="$(read_block "$DEFAULT_DIR" 'type(blk.get("envs")).__name__')"
[ "$FIXED_ENVS" = "dict" ] || fail "S3: envs not repaired (type=$FIXED_ENVS)"
log "  ✓ S3b: the rest of the block was repaired (schema=1, envs={})"

log "E2E subos-runtime-binding: PASS"
