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
# S1's expectation is READ from the source, not written here.
#
# It used to be a pinned literal, on the reasoning that "what runtime does a
# fresh subos get" is this repo's own contract. That was true while the answer
# was a constant. It no longer is: the version now comes from the index, and
# the constant below is only what an unanswerable index falls back to. This
# home is built with an EMPTY index on purpose (see below), so the fallback is
# the path under test here -- and pinning its value would mean editing this
# file every time the fallback moves, which is the coupling the change was
# about removing.
#
# S4 covers the other half: an index that CAN answer, answering with something
# no constant in the tree contains.

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

EXPECTED_DEFAULT="$(sed -n 's/.*DEFAULT_RUNTIME_FALLBACK = "\(.*\)".*/\1/p' \
    "$ROOT_DIR/src/core/subos/manifest.cppm" | head -1)"
# Guarded, because an empty expectation does not fail -- it makes S1a compare
# "" against a subos that also recorded nothing, and passes. A constant renamed
# out from under this sed would then be reported as the feature working.
[ -n "$EXPECTED_DEFAULT" ] \
  || fail "could not read DEFAULT_RUNTIME_FALLBACK out of manifest.cppm; \
every assertion below would compare against an empty string"

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

# The index here is empty, so this binding came from the FALLBACK, and the
# manifest has to say so. Without this the two paths are byte-identical on
# disk and nobody -- doctor included -- can tell a subos that was pinned
# because the index said so from one pinned because the index could not be
# read.
SRC1="$(read_block "$DEFAULT_DIR" 'blk.get("runtime_source", "")')"
[ "$SRC1" = "fallback" ] \
  || fail "S1: empty index, so runtime_source should be 'fallback', got '$SRC1'"
log "  ✓ S1c: fallback path recorded as runtime_source=fallback"

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

# ── S4: an index that CAN answer decides, and no constant can fake it ───
#
# The claim S1 cannot make: that the version comes from the index at all. With
# an empty index, "fallback" and "resolved to the same value" are the same
# reading -- which is exactly the shape this whole change exists to remove, so
# it cannot be how the change is verified.
#
# The index below says 9.9.9. That string appears in no constant, no test
# fixture and no payload anywhere in the tree, so a manifest carrying it can
# only have been resolved. Install fails (there is no such tarball); the
# binding is written before the install step, which is what S2 already relies
# on.
# A COPY of the fixture index, with glibc's `latest` rewritten.
#
# Hand-rolling a two-file index does not work: the catalog declines to load it
# and answers "package index not available", which sends S4 down the FALLBACK
# path -- so a broken harness and a broken feature would read the same, which
# is the shape this test exists to rule out. The fixture is a real index that
# the rest of the suite already loads.
S4_INDEX="$RUNTIME_DIR/answering-index"
cp -a "$ROOT_DIR/tests/fixtures/xim-pkgindex" "$S4_INDEX"
python3 "$ROOT_DIR/tests/e2e/support/rewrite_glibc_latest.py" \
    "$S4_INDEX/pkgs/g/glibc.lua" 9.9.9 \
  || fail "S4: could not rewrite the fixture glibc recipe"

# A SEPARATE home. S1-S3 ran against the empty index in $HOME_DIR and the
# catalog cache on disk remembers that answer; pointing the same home at a new
# index leaves "package not found" behind, which would read as the feature
# failing rather than the cache being stale.
S4_HOME="$RUNTIME_DIR/home-s4"
mkdir -p "$S4_HOME/data/xim-index-repos"
printf '{}\n' > "$S4_HOME/data/xim-index-repos/xim-indexrepos.json"
cat > "$S4_HOME/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$S4_INDEX" }
  ]
}
EOF

x4() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
         XLINGS_HOME="$S4_HOME" "$BIN" "$@" ) }
x4 self init >/dev/null 2>&1 || true
# `update` is what actually syncs an index repo into a home -- without it the
# catalog answers "package index not available", which S1's empty-index setup
# also produces, so the two would be indistinguishable.
x4 update >/dev/null 2>&1 || true

# The index has to actually answer, or S4's real assertion below is measuring
# the fallback and calling it a pass.
INFO4="$(x4 info glibc 2>&1)" || true
printf '%s\n' "$INFO4" | grep -q '9\.9\.9' \
  || fail "S4: the test index does not answer for glibc at all:
$INFO4"

OUT4="$(x4 subos new t94 2>&1)" || true
[ -f "$S4_HOME/subos/t94/.xlings.json" ] \
  || { echo "$OUT4" >&2; fail "S4: subos new left no manifest at all"; }
RUNTIME4="$(read_block "$S4_HOME/subos/t94" 'blk.get("runtime")')"
[ "$RUNTIME4" = "glibc@9.9.9" ] \
  || fail "S4: index says 9.9.9 but the subos recorded '$RUNTIME4' -- the
    default is still coming from a constant, so it will drift from the index
    again the next time glibc is republished"
log "  ✓ S4a: the index decided the default (glibc@9.9.9)"

SRC4="$(read_block "$S4_HOME/subos/t94" 'blk.get("runtime_source", "")')"
[ "$SRC4" = "index" ] \
  || fail "S4: resolved from the index but runtime_source is '$SRC4'"
log "  ✓ S4b: recorded as runtime_source=index"

log "E2E subos-runtime-binding: PASS"
