#!/usr/bin/env bash
# E2E-110: a cross-subos `--fix` child that hangs is a FAILURE, not an
# indefinite wait (2026.9.12, F8).
#
# `repair_other_subos_walk_` (doctor.cpp) used to run each
# `self doctor --fix --subos <name>` child via `platform::exec` with no
# bound at all -- a child stuck on (say) a stalled download hung the
# PARENT right along with it, indistinguishable from any other slow
# repair. `XLINGS_DOCTOR_CHILD_TIMEOUT` (seconds; default 30 min) now
# bounds it, in process (platform::run_shell_unattended; 2026.10.14 dropped
# timeout(1), which put the child in a background process group).
#
# Deterministic by construction: the fixture's install() hook `os.sleep`s
# for longer than the configured timeout, every time, so there is nothing
# to race -- either the bound works or the test itself hangs.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index


RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/doctor_cross_subos_child_timeout"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN_IN() {
  local subos="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS="$subos" \
      XLINGS_DOCTOR_CHILD_TIMEOUT=1 \
      "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR/subos/default/bin" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/s"

# Sleeps far past the 1s timeout on EVERY call -- there is no "eventually
# heals" path here, which is the point: the parent must not wait forever,
# and must not read a killed child as anything other than a failure.
cat > "$LOCAL_INDEX_DIR/pkgs/s/sleepy.lua" <<'LUA'
package = {
    spec = "1",
    name = "sleepy",
    description = "Local fixture: install() always hangs, for doctor_cross_subos_child_timeout_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    categories = {"test-fixture"},
    programs = {"sleepy"},
    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}

function install()
    os.exec("sleep 6")
    return false
end

function uninstall()
    return true
end
LUA

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF

log "init sandbox, second subos"
RUN_IN default self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
RUN_IN default subos new other >/dev/null 2>&1 || fail "subos new other failed"

# Hand-register `sleepy@1.0.0` as broken and active in 'other' -- claimed
# (not unclaimed), owned by 'other', so `default`'s cross-subos walk finds
# a ForeignPayload finding and shells into `self doctor --fix --subos
# other`, which is the child this test needs to hang.
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
state = home / ".xlings.json"
d = json.loads(state.read_text())
d.setdefault("versions", {})["sleepy"] = {
    "filename": "sleepy",
    "type": "program",
    "versions": {
        "1.0.0": {
            "kind": "program",
            "path": str(home / "data" / "xpkgs" / "xim-x-sleepy" / "1.0.0" / "bin"),
        }
    },
}
state.write_text(json.dumps(d, indent=2))

sub = home / "subos" / "other" / ".xlings.json"
s = json.loads(sub.read_text()) if sub.exists() else {"workspace": {}}
s.setdefault("workspace", {})["sleepy"] = {"active": "1.0.0", "installed": ["1.0.0"]}
sub.write_text(json.dumps(s, indent=2))
PY

log "self doctor from default sees the foreign, broken payload in 'other'"
# A ForeignPayload finding is reported but does not gate THIS subos's own
# exit code (it belongs to 'other', not 'default') -- only `--fix`'s
# cross-subos walk actually reaches into it, which is what this test is
# about.
out="$(RUN_IN default self doctor 2>&1)" || true
echo "$out" | strip_ansi | grep -qi "sleepy" \
  || fail "the finding must name sleepy; got:\n$out"

log "self doctor --fix -y from default: the 'other' child hangs and must be killed within the 1s bound"
start_ms=$(( $(date +%s%N) / 1000000 ))
rc=0; fixout="$(RUN_IN default self doctor --fix -y 2>&1)" || rc=$?
end_ms=$(( $(date +%s%N) / 1000000 ))
elapsed_ms=$(( end_ms - start_ms ))
echo "$fixout" | tail -20 | sed 's/^/    | /'
log "  elapsed: ${elapsed_ms}ms"

# Generous relative to the 1s bound (the child also pays for catalog load
# etc. before it ever reaches the sleep), but a tiny fraction of what an
# unbounded wait for a `sleep 5` hook would take.
if [[ $elapsed_ms -gt 30000 ]]; then
  fail "--fix took ${elapsed_ms}ms with a 1s child timeout configured -- looks like the hung child was never killed"
fi

[[ $rc -ne 0 ]] \
  || fail "a --fix whose only cross-subos child hung must not exit 0; got rc=0:\n$fixout"
echo "$fixout" | strip_ansi | grep -qi "timed out" \
  || fail "the report must say the child timed out; got:\n$fixout"
echo "$fixout" | strip_ansi | grep -q "other" \
  || fail "the report must name the subos whose child hung; got:\n$fixout"

verified="$(python3 -c "
import json, pathlib
print(json.loads(pathlib.Path('$HOME_DIR/.xlings.json').read_text()).get('verifiedBy', ''))
")"
[[ -z "$verified" ]] \
  || fail "verifiedBy must not be stamped while a cross-subos child hung and was killed; got '$verified'"

log "all scenarios passed -- a hung cross-subos child is a failure, not a wait"
