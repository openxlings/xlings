#!/usr/bin/env bash
# E2E: upgrading a home must not re-declare its SubOS against a runtime it was
# never on.
#
# Measured on a real home before this test existed: of ~25 SubOS, three carried
# a `subos_info.runtime` and the rest predated the block entirely. Every one of
# those was declared against DEFAULT_RUNTIME the first time anything rebuilt
# its block. Once the runtime declaration became authoritative, that stamp made
# `self doctor` report an error and `use` refuse to activate the runtime the
# SubOS was already running -- for a state the user never chose.
#
# Three states, and the difference between them is the whole contract:
#
#   S1 legacy      no block, workspace runs 2.39   -> declare 2.39, not the default
#   S2 mis-stamped block says 2.44, workspace 2.39 -> Error, --fix adopts 2.39
#   S3 cold intent block says 2.44, nothing active -> Warning, --fix leaves it
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$(runtime_home_dir subos_runtime_declaration_upgrade)"
cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

XLINGS_BIN="$(find_xlings_bin)"
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"

# $1 home dir, $2 the subos_info block (or empty for none), $3 workspace body
mk_home() {
  local H="$1" block="$2" ws="$3"
  rm -rf "$H"
  mkdir -p "$H/subos/default/bin" "$H/data/xpkgs/xim-x-glibc/2.39/lib64" \
           "$H/data/xpkgs/xim-x-glibc/2.44/lib64"
  # Many payloads coexisting is the supported shape, not the defect.
  local v
  for v in 2.39 2.44; do
    : > "$H/data/xpkgs/xim-x-glibc/$v/lib64/libc.so.6"
    : > "$H/data/xpkgs/xim-x-glibc/$v/.xpkg-install.json"
  done
  cat > "$H/.xlings.json" <<JSON
{
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "subos": {"default": {"dir": ""}},
  "versions": {
    "glibc": {
      "versions": {
        "2.39": {"kind": "program", "sourceName": "glibc",
                 "destinationName": "glibc",
                 "bindingGroup": {"group": "glibc", "provider": "xim:glibc",
                                  "rootTarget": "glibc", "rootVersion": "2.39",
                                  "version": "2.39"},
                 "path": "$H/data/xpkgs/xim-x-glibc/2.39"},
        "2.44": {"kind": "program", "sourceName": "glibc",
                 "destinationName": "glibc",
                 "bindingGroup": {"group": "glibc", "provider": "xim:glibc",
                                  "rootTarget": "glibc", "rootVersion": "2.44",
                                  "version": "2.44"},
                 "path": "$H/data/xpkgs/xim-x-glibc/2.44"}
      }
    }
  }
}
JSON
  if [[ -n "$block" ]]; then
    printf '{%s,"workspace":{%s}}\n' "$block" "$ws" > "$H/subos/default/.xlings.json"
  else
    printf '{"workspace":{%s}}\n' "$ws" > "$H/subos/default/.xlings.json"
  fi
  ln -sfn "$H/subos/default" "$H/subos/current"
}

declared_runtime() {
  "$XLINGS_BIN" --version >/dev/null 2>&1   # keep the binary on the record
  /usr/bin/env python3 -c '
import json,sys
try: d=json.load(open(sys.argv[1]))
except Exception: print("UNREADABLE"); raise SystemExit
b=d.get("subos_info")
print("ABSENT" if not isinstance(b,dict) else b.get("runtime","NOKEY"))' "$1"
}

BLOCK_2_44='"subos_info":{"schema_version":1,"runtime":"glibc@2.44","envs":{},"created_at":"2026-01-01T00:00:00Z","created_by":"xlings 2026.8.9.2"}'
WS_39='"glibc":{"active":"2.39","installed":["2.39"]}'
WS_NONE=''

# ── S1: a home that predates subos_info ─────────────────────────────────
log "S1: a legacy home is declared against the runtime it actually runs"
H1="$RUNTIME_DIR/legacy"
mk_home "$H1" "" "$WS_39"
[[ "$(declared_runtime "$H1/subos/default/.xlings.json")" == "ABSENT" ]] \
  || fail "S1 fixture already has a runtime declaration"
XLINGS_HOME="$H1" "$XLINGS_BIN" self doctor --fix > "$RUNTIME_DIR/s1.fix" 2>&1 || true
S1_RUNTIME="$(declared_runtime "$H1/subos/default/.xlings.json")"
[[ "$S1_RUNTIME" == "glibc@2.39" ]] \
  || fail "S1 declared '$S1_RUNTIME'; the SubOS runs 2.39 and must be declared against it"

# Assert on the finding, not the exit code: this fixture has unrelated
# defects (no shims, synthetic binding manifests) and asserting exit 0 would
# make the test about those instead of about the declaration.
XLINGS_HOME="$H1" "$XLINGS_BIN" self doctor > "$RUNTIME_DIR/s1.doctor" 2>&1 || true
if grep -q "subos runtime" "$RUNTIME_DIR/s1.doctor"; then
  fail "S1 still reports a runtime finding after repair: $(cat "$RUNTIME_DIR/s1.doctor")"
fi

# ── S2: a home already mis-stamped by 2026.8.9.1/.2 ─────────────────────
log "S2: a mis-stamped declaration is an error, and --fix adopts what runs"
H2="$RUNTIME_DIR/misstamped"
mk_home "$H2" "$BLOCK_2_44" "$WS_39"
XLINGS_HOME="$H2" "$XLINGS_BIN" self doctor > "$RUNTIME_DIR/s2.doctor" 2>&1 || true
grep -q "subos runtime" "$RUNTIME_DIR/s2.doctor" \
  || fail "S2 doctor said nothing about a declaration nothing here ever activated"
grep -Fq "nothing here was ever activated against glibc@2.44" "$RUNTIME_DIR/s2.doctor" \
  || fail "S2 doctor did not say the declaration was never activated: $(cat "$RUNTIME_DIR/s2.doctor")"
# The remedy must name a way out that exists. "create a new SubOS" was never
# the only option: activating the DECLARED version is what the guard permits.
grep -Fq "self doctor --fix" "$RUNTIME_DIR/s2.doctor" \
  || fail "S2 remedy omitted the adopt path: $(cat "$RUNTIME_DIR/s2.doctor")"
grep -Fq "xlings use glibc 2.44" "$RUNTIME_DIR/s2.doctor" \
  || fail "S2 remedy omitted the migrate path: $(cat "$RUNTIME_DIR/s2.doctor")"
if grep -Fq "subos new" "$RUNTIME_DIR/s2.doctor"; then
  fail "S2 remedy still sends the user to a new SubOS"
fi

XLINGS_HOME="$H2" "$XLINGS_BIN" self doctor --fix > "$RUNTIME_DIR/s2.fix" 2>&1 || true
S2_RUNTIME="$(declared_runtime "$H2/subos/default/.xlings.json")"
[[ "$S2_RUNTIME" == "glibc@2.39" ]] \
  || fail "S2 --fix left the declaration at '$S2_RUNTIME'"
grep -Fq "never activated here" "$RUNTIME_DIR/s2.fix" \
  || fail "S2 --fix changed the declaration without saying so: $(cat "$RUNTIME_DIR/s2.fix")"

XLINGS_HOME="$H2" "$XLINGS_BIN" self doctor > "$RUNTIME_DIR/s2.after" 2>&1 || true
if grep -q "subos runtime" "$RUNTIME_DIR/s2.after"; then
  fail "S2 still reports a runtime finding after --fix: $(cat "$RUNTIME_DIR/s2.after")"
fi

# And the version it is already on is activatable again -- the refusal was the
# other half of the dead end.
set +e
XLINGS_HOME="$H2" "$XLINGS_BIN" use glibc 2.39 > "$RUNTIME_DIR/s2.use" 2>&1
set -e
if grep -Fq "runtime activation refused" "$RUNTIME_DIR/s2.use"; then
  fail "S2 use is still refused after --fix: $(cat "$RUNTIME_DIR/s2.use")"
fi

# ── S3: a deliberate cold intent must survive --fix ─────────────────────
log "S3: a declared-but-not-yet-active runtime is a decision, not a defect"
H3="$RUNTIME_DIR/coldintent"
mk_home "$H3" "$BLOCK_2_44" "$WS_NONE"
XLINGS_HOME="$H3" "$XLINGS_BIN" self doctor --fix > "$RUNTIME_DIR/s3.fix" 2>&1 || true
S3_RUNTIME="$(declared_runtime "$H3/subos/default/.xlings.json")"
[[ "$S3_RUNTIME" == "glibc@2.44" ]] \
  || fail "S3 --fix overwrote a cold intent with '$S3_RUNTIME'"

log "PASS: runtime declarations survive an upgrade"
