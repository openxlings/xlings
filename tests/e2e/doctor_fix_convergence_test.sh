#!/usr/bin/env bash
# E2E: `xlings self doctor --fix` must leave the home better than it found it.
#
# The 2026.8.1.1 regression: the activation repair added in that release ran
# `use` on an unreachable release; that stranded members of a SECOND release;
# the deactivation repair (plan_incoherent_deactivation) then took those
# members down; and the resulting "installed but inactive" entries came back as
# new findings for the next run to activate again. `--fix` ended with MORE
# issues than it started with, having printed a hundred lines of `deactivated`,
# and it left `gcc` and `ld` with no active version on a real machine.
#
# E2E-52 could not catch it: every one of its scenarios is a single release or
# two packages with one shared name. The shape that breaks is MULTI-VERSION
# plus CROSS-RELEASE NAME OVERLAP, which is what this fixture builds.
#
# Scenarios:
#   S1  a second installed release of an ACTIVE package is not a finding
#   S2  a release that cannot be activated without breaking an active one is
#       reported as a conflict and NOT auto-activated
#   S3  --fix twice: issues must not grow and the workspace must stop moving
#   S4  a foreign-platform payload is not reported as "programs not on PATH"
#   S5  several inactive releases of one root collapse into ONE finding
#   S6  the plain unreachable release (#465's node shape) is still auto-fixed

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/doctor_fix_convergence"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

issue_count() { RUN self doctor 2>&1 | grep -c '✗' || true; }

ws_digest() {
  python3 - "$HOME_DIR" <<'PY'
import hashlib, json, pathlib, sys
p = pathlib.Path(sys.argv[1], "subos", "default", ".xlings.json")
ws = json.loads(p.read_text())["workspace"]
active = {k: v.get("active") for k, v in sorted(ws.items())}
print(hashlib.sha256(json.dumps(active, sort_keys=True).encode()).hexdigest())
PY
}

mkdir -p "$HOME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/c"

# ── Fixtures ───────────────────────────────────────────────────────
#
# `alpha` ships two releases, each registering `alpha`, `overlap` and
# `alpha-only`. `beta` ships one release registering `beta`, `overlap` and
# `beta-only`. `overlap` is the crossing name — the `ar`/`nm` of the measured
# home, where binutils and llvm both provide it.

emit_pkg() {
  local name="$1" versions="$2" extra="$3"
  cat > "$LOCAL_INDEX_DIR/pkgs/c/$name.lua" <<LUA
package = {
    spec = "1",
    name = "$name",
    description = "Local fixture for doctor_fix_convergence_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    programs = {"$name", "overlap", "$extra"},
    xpm = {
        linux   = { $versions },
        macosx  = { $versions },
        windows = { $versions },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    for _, n in ipairs({"$name", "overlap", "$extra"}) do
        io.writefile(path.join(bindir, n),
                     "#!/bin/sh\necho " .. n .. "@" .. pkginfo.version() .. "\n")
        if os.host() ~= "windows" then
            os.exec("chmod +x " .. path.join(bindir, n))
        end
    end
    return true
end

function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    local root = "$name@" .. pkginfo.version()
    xvm.add("$name", { bindir = bindir })
    xvm.add("overlap", { bindir = bindir, binding = root })
    xvm.add("$extra", { bindir = bindir, binding = root })
    return true
end

function uninstall()
    xvm.remove("$name")
    xvm.remove("overlap")
    xvm.remove("$extra")
    return true
end
LUA
}

emit_pkg alpha '["1.0.0"] = {}, ["2.0.0"] = {}' alpha-only
# A version of its own: two providers may not own the same exact
# (name, version), and both packages register `overlap`.
emit_pkg beta  '["3.0.0"] = {}'                 beta-only

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
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

STATE="$HOME_DIR/subos/default/.xlings.json"

# ── S1: the release you did not pick is not a defect ───────────────
log "S1: two releases of one package, one active → the other is not reported"
RUN install alpha@1.0.0 -y >/dev/null 2>&1 || fail "S1 setup: alpha@1.0.0 failed"
RUN install alpha@2.0.0 -y >/dev/null 2>&1 || fail "S1 setup: alpha@2.0.0 failed"
RUN use alpha 2.0.0 >/dev/null 2>&1 || fail "S1 setup: use alpha 2.0.0 failed"

rc=0
out=$(RUN self doctor 2>&1) || rc=$?
echo "$out" | grep -q "alpha@1.0.0" \
  && fail "S1: alpha@1.0.0 is the release you did NOT pick, not a broken one; got:\n$out"
[[ $rc -eq 0 ]] || fail "S1: a home with a chosen release is healthy; expected exit 0, got $rc"

# ── S2: a conflicting activation is reported, not performed ────────
#
# beta registers `overlap`, which alpha@2.0.0 currently owns. Activating beta
# would take it, leaving alpha's release incoherent — which is precisely what
# the deactivation repair would then tear down.
log "S2: install beta → uncontested members activate, overlap stays with alpha"
RUN install beta@3.0.0 -y >/dev/null 2>&1 || fail "S2 setup: beta@3.0.0 install failed"

active_of() {
  python3 - "$STATE" "$1" <<'PY'
import json, sys
ws = json.loads(open(sys.argv[1]).read())["workspace"]
print((ws.get(sys.argv[2]) or {}).get("active", ""))
PY
}
[[ "$(active_of overlap)" == "2.0.0" ]] \
  || fail "S2: installing beta took 'overlap' from alpha (now $(active_of overlap))"
[[ -n "$(active_of beta-only)" ]] \
  || fail "S2: 'beta-only' is uncontested and must be usable"

# Now strand beta's own root so it becomes an unreachable release whose
# activation WOULD break alpha.
python3 - "$STATE" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text())
for name in ("beta", "beta-only"):
    d["workspace"][name].pop("active", None)
p.write_text(json.dumps(d, indent=2))
PY
rm -f "$HOME_DIR/subos/default/bin/beta" "$HOME_DIR/subos/default/bin/beta-only"

rc=0
out=$(RUN self doctor 2>&1) || rc=$?
echo "$out" | grep -q "no active version" \
  || fail "S2: beta is unreachable and must be reported; got:\n$out"
# Activating beta takes `overlap` from alpha. That is what `use` means, and the
# finding has to say so before --fix does it.
echo "$out" | grep -q "also moves overlap" \
  || fail "S2: the finding must disclose that repair moves a name alpha holds; got:\n$out"
[[ $rc -eq 1 ]] || fail "S2: an unreachable release is an error; expected exit 1, got $rc"

# THE REGRESSION, directly.
#
# --fix activates beta, which moves `overlap` off alpha's release. Before
# 2026.8.1.2 the deactivation repair then declared alpha incoherent and took
# `alpha` and `alpha-only` down with it — on the measured home that is how
# `gcc` and `ld` lost their active versions. A name held by another PROVIDER is
# not this release breaking step, and the teardown planner now knows that.
RUN self doctor --fix >/dev/null 2>&1 || true
[[ -n "$(active_of beta)" ]] \
  || fail "S2: nothing prevented it, so --fix should have activated beta"
[[ "$(active_of alpha)" == "2.0.0" ]] \
  || fail "S2: activating beta demolished alpha's release — alpha is now '$(active_of alpha)'"
[[ "$(active_of alpha-only)" == "2.0.0" ]] \
  || fail "S2: alpha-only was collateral of a repair it had nothing to do with"

# ── S3: convergence ────────────────────────────────────────────────
#
# The assertion the 2026.8.1.1 regression would have failed on its first run.
log "S3: --fix twice → issues never grow and the workspace stops moving"
n1="$(issue_count)"
RUN self doctor --fix >/dev/null 2>&1 || true
after1_digest="$(ws_digest)"
n2="$(issue_count)"
RUN self doctor --fix >/dev/null 2>&1 || true
after2_digest="$(ws_digest)"
n3="$(issue_count)"

[[ "$n2" -le "$n1" ]] \
  || fail "S3: --fix made it worse — $n1 issue(s) before, $n2 after"
[[ "$n3" -le "$n2" ]] \
  || fail "S3: the second --fix made it worse — $n2 issue(s) before, $n3 after"
[[ "$after1_digest" == "$after2_digest" ]] \
  || fail "S3: two --fix runs left different workspaces — the repairs are trading state"

# The regression also has to be visible when it happens, not just prevented.
RUN self doctor --fix 2>&1 | grep -qi "not converging" \
  && fail "S3: the convergence assertion fired on a converging home"

# ── S4: a foreign-platform payload has no programs to miss ─────────
log "S4: a Windows payload in a Linux store is not reported"
FOREIGN="$HOME_DIR/data/xpkgs/xim-x-foreign-fixture/9.9.9"
mkdir -p "$FOREIGN/bin"
# PE header: 'MZ' is what classify_payload_content reads.
printf 'MZ\x90\x00' > "$FOREIGN/bin/winonly.exe"
chmod +x "$FOREIGN/bin/winonly.exe"
python3 - "$HOME_DIR" "$FOREIGN" <<'PY'
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
state = home / ".xlings.json"
d = json.loads(state.read_text())
d["versions"]["winonly.exe"] = {
    "filename": "winonly.exe",
    "type": "program",
    "versions": {"9.9.9": {"kind": "program", "path": sys.argv[2] + "/bin"}},
}
state.write_text(json.dumps(d, indent=2))
sub = home / "subos" / "default" / ".xlings.json"
s = json.loads(sub.read_text())
s["workspace"]["winonly.exe"] = {"installed": ["9.9.9"]}
sub.write_text(json.dumps(s, indent=2))
PY
out=$(RUN self doctor 2>&1) || true
echo "$out" | grep -q "winonly" \
  && fail "S4: a payload built for another platform can never be on PATH here; got:\n$out"

# ── S5: several inactive releases of one root → one finding ────────
log "S5: two inactive releases of one root collapse into a single finding"
python3 - "$STATE" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text())
for name in ("alpha", "alpha-only", "overlap"):
    d["workspace"][name].pop("active", None)
p.write_text(json.dumps(d, indent=2))
PY
rm -f "$HOME_DIR/subos/default/bin/alpha" "$HOME_DIR/subos/default/bin/alpha-only"
out=$(RUN self doctor 2>&1) || true
count=$(echo "$out" | grep -c "no active version.*alpha" || true)
[[ "$count" -eq 1 ]] \
  || fail "S5: alpha 1.0.0 and 2.0.0 are one decision, not two findings; got $count:\n$out"
echo "$out" | grep -q "also installed" \
  || fail "S5: the finding must name the other version so the choice is visible; got:\n$out"

# ── S6: the plain unreachable release is still auto-fixed ──────────
#
# The preflight must not have closed the path #465 opened. Nothing contests
# beta's names once alpha is entirely inactive.
log "S6: an unreachable release with nothing contesting it is still repaired"
RUN self doctor --fix >/dev/null 2>&1 || true
[[ -n "$(active_of alpha)" ]] \
  || fail "S6: nothing contested alpha and --fix left it unreachable"
[[ -e "$HOME_DIR/subos/default/bin/alpha" ]] \
  || fail "S6: --fix activated alpha but wrote no shim"

n_final="$(issue_count)"
[[ "$n_final" -le "$n1" ]] \
  || fail "S6: the run ended worse than S3 started ($n1 → $n_final)"


# ── S7: an Error-level subos finding reaches the exit code, and healed
#        accounts for it ──────────────────────────────────────────────
#
# Three findings were Error-level and absent from count_(): SubosManifest,
# SubosEnvOrphan and SubosEnvUnresolved. doctor printed `✗ subos env orphan …`
# and then exited 0, so every script wrapping it saw success.
#
# The second consequence is quieter and is what this scenario pins: `healed` is
# before-minus-after over that same count, so --fix repairing an orphan reported
# "healed 0" — the repairer acted and the reporter said nothing. Asserting
# `healed > 0` checks BOTH ends at once, and those two ends are exactly where
# this repo has drifted three times.
log "S7: an Error-level subos finding fails the run, and --fix reports healing it"

MANIFEST7="$HOME_DIR/subos/default/.xlings.json"
python3 - "$MANIFEST7" <<'PY'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text()) if p.exists() else {"workspace": {}}
blk = d.setdefault("subos_info", {
    "schema_version": 1, "runtime": "glibc@2.39", "envs": {},
    "created_at": "2026-08-06T00:00:00Z", "created_by": "e2e",
})
blk.setdefault("envs", {})
# A provider whose package is not installed here: the orphan case.
blk["envs"]["neverinstalled@9.9.9"] = [
    {"var": "E2E_S7_DIRS", "op": "prepend", "value": "${pkgdir}/share"}
]
p.write_text(json.dumps(d, indent=2))
PY

# `rc=0` then `|| rc=$?`, not `; rc=$?`: the shared lib sets `set -e`, so an
# assignment whose command exits non-zero kills the script before the next
# statement runs -- and a non-zero exit is precisely what this scenario is
# asserting.
rc7=0
out7="$(RUN self doctor 2>&1)" || rc7=$?
[[ $rc7 -ne 0 ]] \
  || fail "S7: doctor printed an Error-level finding and exited 0 — every
script wrapping it sees success:
$out7"
echo "$out7" | strip_ansi | grep -q "subos env orphan" \
  || fail "S7: the orphan was not reported at all:
$out7"
log "  ✓ an Error-level subos finding fails the run"

out7fix="$(RUN self doctor --fix 2>&1)" || true
healed7="$(echo "$out7fix" | strip_ansi | sed -n 's/.*healed[^0-9]*\([0-9][0-9]*\).*/\1/p' | tail -1)"
[[ -n "$healed7" && "$healed7" -gt 0 ]] \
  || fail "S7: --fix repaired the orphan but reported healed='${healed7:-<none>}'.
The repairer acted and the reporter said nothing — the same split, arrived at
from the counting side:
$out7fix"
log "  ✓ --fix reports healed=$healed7"

RUN self doctor >/dev/null 2>&1 \
  || fail "S7: doctor still fails after --fix cleared the finding"
log "  ✓ doctor is clean afterwards"

log "all scenarios passed"

# ── S8: `--fix` repairs what was reported, and says it did not deep-audit ───
#
# `--fix` used to imply `--deep`, so every repair paid for a full ELF walk over
# every package at every version. Measured on a real 124-package / 71 GB home:
# `self doctor` 0.75s, `--fix --dry-run` 148s WITHOUT repairing anything — and
# the four findings it was about to repair had been known since 0.75s.
#
# The two are orthogonal: `--deep` decides what gets REPORTED, `--fix` repairs
# what WAS reported. What is asserted here is not the speed (a fixture home is
# small enough that timing proves nothing) but the two things that make the
# change safe:
#
#   1. the narrowing is ANNOUNCED — a silently smaller scope trades a slow
#      problem for a quiet correctness one, which is worse
#   2. `--deep --fix` still audits, so nothing was removed, only decoupled
log "S8: --fix does not imply --deep, and says so"

out8="$(RUN self doctor --fix --dry-run 2>&1)" || true
# The BEHAVIOUR first: the deep audit announces itself, so its announcement
# absent is the audit not having run. Asserting only on the notice was vacuous
# — the notice was gated on the flags rather than on what happened, so it
# printed either way and the test passed against the old behaviour too.
echo "$out8" | strip_ansi | grep -q "deep audit scope" \
  && fail "S8: --fix still ran the payload audit:
$out8"
echo "$out8" | strip_ansi | grep -q "payload/runtime audit is not run" \
  || fail "S8: --fix narrowed the scope without saying so:
$out8"
echo "$out8" | strip_ansi | grep -q -- "--deep" \
  || fail "S8: the notice does not tell the user how to get the audit back:
$out8"
log "  ✓ --fix announces that the payload audit was skipped, and how to get it"

out8d="$(RUN self doctor --fix --deep --dry-run 2>&1)" || true
echo "$out8d" | strip_ansi | grep -q "deep audit scope" \
  || fail "S8: --deep --fix no longer runs the payload audit:
$out8d"
echo "$out8d" | strip_ansi | grep -q "payload/runtime audit is not run" \
  && fail "S8: --deep --fix still claims it skipped the audit:
$out8d"
log "  ✓ --deep --fix still audits payloads"
