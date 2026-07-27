#!/usr/bin/env bash
# E2E-36: corrupt binding metadata survives being saved, and can be discarded.
#
# The dead end: an entry whose `bindingGroup` is malformed makes `xlings use`
# refuse, `doctor` could name it but not repair it, and hand-editing
# versions.json was the only way out.
#
# Scenario 2 is the one only an end-to-end run can show. Every xlings command
# rewrites the state file, and the serializer used to rebuild each entry from
# the parsed model -- so the first command after the corruption quietly
# replaced the malformed value with whatever had parsed, and the original was
# gone before anyone could ask to reset it. A unit test round-trips one
# object; only the real CLI proves that running unrelated commands does not
# erode the file.
#
# Scenarios:
#   1. corrupt the entry            → `use` refuses, doctor names it
#   2. run unrelated commands       → the malformed value is still verbatim
#   3. doctor --fix (without the flag) → deliberately does NOT touch it
#   4. doctor --reset-metadata      → entry cleared, `use` works again

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/xvm_metadata_reset"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
STATE="$HOME_DIR/.xlings.json"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/m"

cat > "$LOCAL_INDEX_DIR/pkgs/m/mdfixture.lua" <<'LUA'
package = {
    spec = "1",
    name = "mdfixture",
    description = "Local fixture for tests/e2e/xvm_metadata_reset_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    io.writefile(path.join(dir, "bin", "mdfixture"), "#!/bin/sh\necho mdfixture\n")
    os.exec("chmod +x " .. path.join(dir, "bin", "mdfixture"))
    return true
end

function config()
    xvm.add("mdfixture", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall() return true end
LUA

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$STATE" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF

log "Initializing sandbox XLINGS_HOME at $HOME_DIR"
RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

RUN install mdfixture@1.0.0 -y >/dev/null 2>&1 || fail "fixture install failed"

# The exact malformed value, kept in one place so scenario 2 compares against
# the same literal the corruption wrote.
CORRUPT_GROUP='{"provider": "xim:mdfixture", "version": 7, "group": "mdfixture", "rootTarget": "mdfixture", "rootVersion": "1.0.0"}'

python3 - "$STATE" "$CORRUPT_GROUP" <<'PY'
import json, sys
path, group = sys.argv[1], json.loads(sys.argv[2])
state = json.load(open(path))
versions = state.get("versions") or state.get("data") or {}
entry = versions["mdfixture"]["versions"]["1.0.0"]
entry["bindingGroup"] = group
json.dump(state, open(path, "w"), indent=2)
PY

read_group() {
  python3 - "$STATE" <<'PY'
import json, sys
state = json.load(open(sys.argv[1]))
versions = state.get("versions") or state.get("data") or {}
entry = versions.get("mdfixture", {}).get("versions", {}).get("1.0.0", {})
print(json.dumps(entry.get("bindingGroup"), sort_keys=True))
PY
}

BEFORE="$(read_group)"
[[ "$BEFORE" != "null" ]] || fail "corruption fixture did not apply"

# ── Scenario 1: the symptom, and doctor naming it ────────────────────────
log "scenario 1: use refuses and doctor names the entry"
if RUN use mdfixture 1.0.0 >/dev/null 2>&1; then
  fail "the dead end under test no longer reproduces — use succeeded"
fi
OUT="$(RUN self doctor 2>&1 || true)"
grep -q 'xvm-binding-metadata-corrupt' <<<"$OUT" \
  || { echo "$OUT" >&2; fail "doctor did not report the corrupt entry"; }
grep -q -- '--reset-metadata' <<<"$OUT" \
  || { echo "$OUT" >&2; fail "doctor named the problem but not the remedy"; }
log "  ✓ refused, reported, remedy named"

# ── Scenario 2: unrelated commands must not erode the file ───────────────
log "scenario 2: the malformed value survives commands that rewrite state"
RUN list >/dev/null 2>&1 || true
RUN self doctor >/dev/null 2>&1 || true
RUN use mdfixture 1.0.0 >/dev/null 2>&1 || true
AFTER="$(read_group)"
if [[ "$AFTER" != "$BEFORE" ]]; then
  echo "before: $BEFORE" >&2
  echo "after:  $AFTER"  >&2
  fail "saving rewrote the corrupt entry — the original is unrecoverable"
fi
log "  ✓ byte-for-byte identical after three state-writing commands"

# ── Scenario 3: --fix alone must not discard it ──────────────────────────
log "scenario 3: --fix does not silently discard metadata"
RUN self doctor --fix >/dev/null 2>&1 || true
[[ "$(read_group)" == "$BEFORE" ]] \
  || fail "--fix discarded release metadata without being asked"
log "  ✓ untouched by --fix"

# ── Scenario 4: the reset, and the dead end lifting ──────────────────────
log "scenario 4: --reset-metadata clears the entry and use works again"
OUT="$(RUN self doctor --reset-metadata 2>&1 || true)"
grep -q 'metadata reset' <<<"$OUT" \
  || { echo "$OUT" >&2; fail "reset ran without reporting what it discarded"; }

[[ "$(read_group)" == "null" ]] \
  || fail "the corrupt group survived the reset: $(read_group)"

RUN use mdfixture 1.0.0 >/dev/null 2>&1 \
  || fail "the entry is still unusable after the reset"

# And the corruption must not return on the next save.
RUN list >/dev/null 2>&1 || true
[[ "$(read_group)" == "null" ]] \
  || fail "the discarded metadata came back on the next save"

OUT="$(RUN self doctor 2>&1 || true)"
grep -q 'xvm-binding-metadata-corrupt' <<<"$OUT" \
  && fail "doctor still reports the entry it just repaired"
log "  ✓ cleared, switchable, and it stays cleared"

log "E2E-36 xvm metadata reset: PASS"
