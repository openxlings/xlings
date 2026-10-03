#!/usr/bin/env bash
# E2E-104: `self doctor --fix` walks every subos that owns a finding,
# without the user first running `xlings subos use <name>` by hand
# (2026.9.12.1).
#
# Before this, a broken payload owned by another subos carried the remedy
# `xlings subos use <name> && xlings self doctor --fix` -- 38 such findings
# on a measured 111-subos home. `--fix` from the CURRENT subos now walks
# every subos a ForeignPayload/OtherSubos finding names, in its own
# subprocess (`self doctor --fix --subos <name>`), so one `--fix` call
# repairs the whole home.
#
#   I3  from `default`, a broken payload `other` owns is reported (not
#       repaired) by plain `self doctor`; `self doctor --fix` (no
#       `subos use` first) repairs it via the cross-subos walk and exits 0;
#       a second `--fix --dry-run` has nothing left to plan.
#   I9  an UNCLAIMED registration -- no subos anywhere references it, and
#       its payload is gone -- is pruned by `--fix --dry-run`, not queued
#       for reinstall (D2): the plan says `prune`, never `would run ...
#       install` for that entry.
#   I9b the same entry, once one subos here cannot even be read: an
#       unreadable subos is a possible claimant, not a non-claimant, so
#       D2 must not prune past it (2026.9.12, F2) -- the plan now says
#       `would run ... install` instead.
#   I5  a cross-subos child that genuinely FAILS (`self doctor --fix
#       --subos <name>` exits non-zero) must be visible in the PARENT's
#       verdict, not swallowed as a note nobody's exit code reflects: the
#       parent exits 1, `verifiedBy` is not written, and the report names
#       the failed subos and the exact command to re-run it directly.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/doctor_cross_subos_fix"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true; rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

# Every command names the subos it runs in, the same convention
# self_doctor_multi_subos_test.sh uses -- this whole test is about which
# subos a repair happens in.
RUN_IN() {
  local subos="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS="$subos" \
      "$XLINGS_BIN" "$@" )
}

OUT_FILE="$RUNTIME_DIR/last-output.txt"
run_capture() {
  local subos="$1"; shift
  rc=0
  RUN_IN "$subos" "$@" >"$OUT_FILE" 2>&1 || rc=$?
  out=$(tr -d '\0' < "$OUT_FILE")
}

mkdir -p "$HOME_DIR/subos/default/bin" "$RUNTIME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/x"

cat > "$LOCAL_INDEX_DIR/pkgs/x/xsf-plain.lua" <<'LUA'
package = {
    spec = "1", name = "xsf-plain",
    description = "cross-subos doctor fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        -- 9.9.9 is declared but never installed: I9/I9b hand-write a DB
        -- entry for it (unclaimed by any workspace) precisely so the
        -- catalog CAN still resolve the coordinate -- the case D2 (and
        -- F2's unreadable-subos guard on it) is about. Without a real
        -- declared version here, `owning_coordinate_` would fail to
        -- resolve it at all and the finding would take the SEPARATE
        -- "unowned" path instead, which prints a near-identical `prune`
        -- line and would make I9/I9b pass without ever exercising D2.
        linux   = { ["1.0.0"] = {}, ["9.9.9"] = {} },
        macosx  = { ["1.0.0"] = {}, ["9.9.9"] = {} },
        windows = { ["1.0.0"] = {}, ["9.9.9"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "xsf-plain"),
                 "#!/bin/sh\necho xsf-plain@" .. pkginfo.version() .. "\n")
    return true
end
function config()
    xvm.add("xsf-plain", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall()
    xvm.remove("xsf-plain")
    return true
end
LUA

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN_IN default self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
RUN_IN default subos new other >/dev/null 2>&1 || fail "subos new other failed"

RUN_IN other install xsf-plain@1.0.0 -y >/dev/null 2>&1 \
  || fail "setup: other install xsf-plain failed"

PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-xsf-plain/1.0.0"
WS_OTHER="$HOME_DIR/subos/other/.xlings.json"
[[ -d "$PAYLOAD" ]] || fail "setup: payload should exist"

has_ws_entry() {
  python3 - "$1" "$2" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
ws = data.get("workspace") or {}
sys.exit(0 if sys.argv[2] in ws else 1)
PY
}

# ── I3: a broken payload owned by 'other' → reported from 'default',
#        repaired by --fix WITHOUT `subos use` first ─────────────────────
log "I3: break the payload owned by 'other'"
rm -rf "$PAYLOAD"

run_capture default self doctor
[[ $rc -eq 0 ]] \
  || fail "I3: another subos's broken payload must not fail this subos (rc=$rc):\n$out"
grep -q "broken payload \[subos: other\]" <<<"$out" \
  || fail "I3: default's report must attribute the finding to 'other'; got:\n$out"

log "I3: self doctor --fix from 'default' repairs it — no 'subos use' needed"
run_capture default self doctor --fix
[[ $rc -eq 0 ]] || fail "I3: default's --fix should exit 0; got $rc:\n$out"
[[ -f "$PAYLOAD/bin/xsf-plain" ]] \
  || fail "I3: --fix should have repaired the payload via the cross-subos walk:\n$out"
has_ws_entry "$WS_OTHER" "xsf-plain" \
  || fail "I3: the repair must land in 'other's own workspace"
# Smoke, not a contract on rc (I5 below covers the failure shape with a
# hand-crafted subos instead): a REAL child subprocess for 'other' must
# actually be invoked, not just imagined by the plan — the announce line
# names it.
grep -q "repairing subos other" <<<"$out" \
  || fail "I3: expected the cross-subos walk to announce running the 'other' child; got:\n$out"

log "I3: a second --fix --dry-run has nothing left to plan for this entry"
run_capture default self doctor --fix --dry-run
[[ $rc -eq 0 ]] || fail "I3: converged home's --fix --dry-run should exit 0; got $rc:\n$out"
grep -qi "would run" <<<"$out" \
  && fail "I3: nothing should be left to plan after convergence; got:\n$out"

# ── I9: an unclaimed registration is pruned, not queued for reinstall ────
#
# A version no subos anywhere references (not active, not in any
# workspace's installed[]) and whose payload is gone. The index can still
# resolve the coordinate (the fixture package is right there), so before D2
# the ladder would have reinstalled it; D2 prunes it instead, because
# nothing will ever use it again.
log "I9: an unclaimed registration is pruned by --fix --dry-run, not reinstalled"
python3 - "$HOME_DIR" <<'PY'
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
state = home / ".xlings.json"
d = json.loads(state.read_text())
d["versions"]["xsf-plain"]["versions"]["9.9.9"] = {
    "kind": "program",
    "path": str(home / "data" / "xpkgs" / "xim-x-xsf-plain" / "9.9.9" / "bin"),
}
state.write_text(json.dumps(d, indent=2))
PY
# No workspace anywhere lists 9.9.9 (default has nothing; other has 1.0.0
# active), and its payload directory was never created -- unclaimed and
# unreachable from the moment it was written.
drop_versions_db_copy "$HOME_DIR"

run_capture default self doctor --fix --dry-run
[[ $rc -ne 0 ]] \
  || fail "I9: the unclaimed broken entry should still count as an issue; got:\n$out"
grep -q "prune xsf-plain@9\.9\.9" <<<"$out" \
  || fail "I9: the plan must prune the unclaimed entry; got:\n$out"
# A genuine toInstall planned line has the "(N entries)" suffix
# repair_payloads_ appends only to THAT loop's own lines -- not the
# substring "install xim:xsf-plain@9.9.9", which also appears embedded
# inside the PRUNE line's own remedy text ("prune ... `xlings install ...`
# brings it back") now that the coordinate resolves. Matching on the
# suffix is what tells "queued for reinstall" apart from "prune line that
# happens to mention install".
grep -qE "install (xim:)?xsf-plain@9\.9\.9[[:space:]]+\([0-9]+ entr" <<<"$out" \
  && fail "I9: the unclaimed entry must not be queued for reinstall; got:\n$out"

# ── I9b: an unreadable subos is a possible claimant, so D2 prunes nothing
#         past it (2026.9.12, F2) ────────────────────────────────────────
#
# Same otherwise-unclaimed entry as I9. The only thing that changes is that
# one subos directory here now cannot be read at all -- and a scan that
# cannot open every subos cannot know every subos does NOT reference
# 9.9.9 either. Before this, `unclaimed` only asked the subos this scan
# COULD read, so a corrupted sibling subos that happened to be the one
# still using an entry would have it pruned out from under it, silently.
log "I9b: an unreadable subos present -> the same entry is no longer pruned"
mkdir -p "$HOME_DIR/subos/ghostly"
printf '{garbage' > "$HOME_DIR/subos/ghostly/.xlings.json"

run_capture default self doctor --fix --dry-run
[[ $rc -ne 0 ]] \
  || fail "I9b: the still-broken entry should still count as an issue; got:\n$out"
grep -q "prune xsf-plain@9\.9\.9" <<<"$out" \
  && fail "I9b: an unreadable subos might still reference this entry -- it must not be pruned; got:\n$out"
grep -qE "install (xim:)?xsf-plain@9\.9\.9[[:space:]]+\([0-9]+ entr" <<<"$out" \
  || fail "I9b: no longer unclaimed, so it must be queued for reinstall instead; got:\n$out"
grep -qi "ghostly" <<<"$out" \
  || fail "I9b: the unreadable subos itself must still be named in the report; got:\n$out"

rm -rf "$HOME_DIR/subos/ghostly"

# ── I5: an owning subos whose NAME is not a safe shell token fails the
#        whole run, and is never actually shelled out to ──────────────────
#
# Its own home, separate from I3/I9's.
#
# This used to break 'other's subos directory read-only, so the child's
# `self doctor --fix --subos other` could write the payload but not its
# own workspace, and genuinely exited non-zero. Task 5's repair ladder
# changed that mechanism's outcome: `remove --force` then reinstall now
# WITHDRAWS the broken registration before the read-only directory ever
# gets a chance to block a write; the reinstall then fails as before, but
# re-detection finds no entry left to be broken at all, and a pruned entry
# with nothing left to find is doctor's pre-existing, intentional "not
# outstanding" case -- so the parent came back 0. That read-only trick is
# no longer a deterministic child failure; it is a race against whichever
# rung of the ladder reacts first, decided in favor of the ladder now.
#
# This scenario keeps doctor's exit semantics for THAT case unchanged (a
# pruned, unclaimed entry is not outstanding -- see the note in the report
# it prints) and reaches the shape I5 actually needs a different way: a
# subos directory created BY HAND (not through `xlings subos new`, which
# -- like the CLI's own flag parser -- would never let a name like this
# through) whose NAME fails `is_shell_safe_token` (leading '-'), with a
# workspace claiming the broken payload's exact target@version, the same
# way a genuine owning subos's file would. The scan attributes the
# ForeignPayload finding to it, and `repair_other_subos_walk_`'s existing
# guard -- the same shell-safety check every other shelled-out value in
# doctor.cpp gets -- refuses to run a subprocess for it: the name goes
# straight into `failedSubos` with a hand-run remedy, never executed. No
# subprocess, no race, no read-only directory: deterministic by
# construction, and it still exercises exactly the contract I5 is about --
# a subos this run cannot repair must fail the parent and withhold the
# stamp rather than be silently skipped.
log "I5: an unsafe-named owning subos is reported and gates the stamp, but is never executed"
HOME5="$RUNTIME_DIR/i5-home"
mkdir -p "$HOME5/subos/default/bin"
cp "$XLINGS_BIN" "$HOME5/xlings"
cat > "$HOME5/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON
RUN5() {
  local subos="$1"; shift
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME5" XLINGS_ACTIVE_SUBOS="$subos" \
      "$XLINGS_BIN" "$@" )
}
RUN5 default self init >/dev/null 2>&1 || fail "I5 setup: self init failed"
mkdir -p "$HOME5/data/xim-index-repos"
printf '{}\n' > "$HOME5/data/xim-index-repos/xim-indexrepos.json"
RUN5 default install xsf-plain@1.0.0 -y >/dev/null 2>&1 \
  || fail "I5 setup: default install xsf-plain failed"

PAYLOAD5="$HOME5/data/xpkgs/xim-x-xsf-plain/1.0.0"
WS_DEFAULT5="$HOME5/subos/default/.xlings.json"
[[ -d "$PAYLOAD5" ]] || fail "I5 setup: payload should exist before breaking it"

# Strip default's OWN claim on the target: ownership must resolve entirely
# to the hand-crafted subos below, not to this (current, safe-named) one --
# default has to see this as a ForeignPayload it does not own itself,
# exactly the shape repair_other_subos_walk_ exists for. The shared
# versions DB lives in $HOME5/.xlings.json, untouched by this edit to
# default's own subos-scoped workspace file.
python3 - "$WS_DEFAULT5" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
data = json.loads(p.read_text())
data.get("workspace", {}).pop("xsf-plain", None)
p.write_text(json.dumps(data))
PY

rm -rf "$PAYLOAD5"

mkdir -p "$HOME5/subos/-bad"
cat > "$HOME5/subos/-bad/.xlings.json" <<JSON
{ "workspace": { "xsf-plain": "1.0.0" } }
JSON

i5_rc=0
i5_out=$(RUN5 default self doctor --fix 2>&1) || i5_rc=$?

[[ $i5_rc -eq 1 ]] \
  || fail "I5: an unsafe-named owning subos must fail the parent's run; got rc=$i5_rc:\n$i5_out"
grep -q -- "-bad" <<<"$i5_out" \
  || fail "I5: the report must name the offending subos '-bad'; got:\n$i5_out"
grep -qi "not a safe shell token" <<<"$i5_out" \
  || fail "I5: the report must say it was never run (unsafe name), not that it failed; got:\n$i5_out"
grep -q -- "--subos -bad" <<<"$i5_out" \
  && fail "I5: an unsafe subos name must never actually be shelled out to; got:\n$i5_out"

i5_verified=$(python3 -c "
import json, pathlib
print(json.loads(pathlib.Path('$HOME5/.xlings.json').read_text()).get('verifiedBy', ''))
")
[[ -z "$i5_verified" ]] \
  || fail "I5: verifiedBy must not be written while an owning subos could not be repaired; got '$i5_verified'"

# The separate, non-asserting-on-rc smoke that a REAL child subprocess for a
# genuinely reachable owning subos still runs (so the subprocess path stays
# covered, not just the unsafe-name skip path) lives in I3 above -- it
# already exercises `self doctor --fix` repairing 'other' for real and
# asserts the "repairing subos other" announce line appears, without this
# scenario's `-bad` entry anywhere nearby to conflate the two.

log "PASS: doctor_cross_subos_fix"
