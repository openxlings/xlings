#!/usr/bin/env bash
# E2E: the routing table is derived, owned and reclaimable — and a name it
# carries for a project's sake changes nothing outside that project.
#
# WHY THIS EXISTS. A project's own bin is never on PATH and cmd.exe has no cd
# hook that could put it there, so a project's command names have to appear in
# the bin directory that IS on PATH. Before the routing table that was done by
# `mirror_shim_to_global_bin`, which copied a file into the global bin from a
# project-scope decision and recorded nothing: measured on a real home, 23 such
# files, none reachable, none reported by doctor, none reclaimable.
#
# The four assertions below are the whole difference, and each one fails on a
# pre-2026.9.3.1 binary:
#
#   A. the project is RECORDED (`knownProjects`), not just mirrored
#   B. deleting the project reclaims its names on the next `doctor --fix`
#   C. outside the project the name behaves as if xlings had never put a file
#      there — it runs the host's copy
#   D. a name this scope CLAIMS but cannot serve is still an error; passthrough
#      must not paper over it (the silent substitution pyenv still ships)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT_DIR/tests/e2e/project_test_lib.sh"

BIN_SRC="$(find_xlings_bin)"
[[ -x "$BIN_SRC" ]] || fail "built xlings binary not found"

root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT

home="$root/home"
proj="$root/work/api"
hostbin="$root/hostbin"
mkdir -p "$home/bin" "$home/subos/default/bin" "$proj" "$hostbin"
cp "$BIN_SRC" "$home/bin/xlings"

# Mirror: GLOBAL by default so CI (which cannot reach the CN endpoints) is
# unchanged; set XLINGS_TEST_MIRROR=CN when running locally from China, or a
# command that touches the index sits on an unreachable host long enough to
# look like a hang. Nothing here downloads, but `self doctor` syncs.
MIRROR="${XLINGS_TEST_MIRROR:-GLOBAL}"
cat > "$home/.xlings.json" <<EOF
{
  "version": "0.4.0",
  "mirror": "$MIRROR",
  "activeSubos": "default",
  "subos": { "default": { "dir": "" } }
}
EOF

# A host program the passthrough must reach. Distinguishable output, so a
# passing assertion cannot be an artefact of xlings printing something.
cat > "$hostbin/demo-tool" <<'EOS'
#!/bin/sh
echo "HOST-DEMO-TOOL"
EOS
chmod +x "$hostbin/demo-tool"

# The entry binary's own shim in the subos bin, the way `self init` places it.
# Nothing in the workspace justifies this file, so it is the one a rebuild is
# most likely to read as stale and delete -- taking out the file every other
# shim points at.
ln -sf "$home/bin/xlings" "$home/subos/default/bin/xlings"

xl() { env -u XLINGS_PROJECT_DIR XLINGS_HOME="$home" "$home/bin/xlings" "$@"; }

# ── Seed a project subos by hand ────────────────────────────────────
#
# Hand-written rather than driven through `install`: this test is about what
# the TABLE does with a project's declared commands, and going through a real
# install would make it a test of the fixture index's download path instead.
mkdir -p "$proj/.xlings/subos/_/bin"
cat > "$proj/.xlings.json" <<'EOF'
{ "workspace": { "demo-tool": "1.0.0" } }
EOF
cat > "$proj/.xlings/.xlings.json" <<EOF
{
  "versions": {
    "demo-tool": {
      "type": "program",
      "versions": { "1.0.0": { "path": "$root/payload" } }
    }
  },
  "workspace": { "demo-tool": { "active": "1.0.0", "installed": ["1.0.0"] } }
}
EOF
mkdir -p "$root/payload"
cp "$hostbin/demo-tool" "$root/payload/demo-tool"

# Register the project the way a project-scope install does.
python3 - "$home/.xlings.json" "$proj" <<'PY'
import json,sys
p,proj=sys.argv[1],sys.argv[2]
j=json.load(open(p))
j.setdefault("knownProjects",{})[proj]={"lastSeen":"2026-09-03T00:00:00Z"}
json.dump(j,open(p,"w"),indent=2)
PY

# ── A. the project's command name reaches the global bin, by derivation ──
xl self doctor --fix >/dev/null 2>&1 || true

gshim="$home/subos/default/bin/demo-tool"
[[ -e "$gshim" ]] \
  || fail "A: the project's command name never reached the global bin"
log "A ok: derived entry present at $gshim"

# It must be a link to the entry binary, not a copy of the host tool.
if [[ "$(readlink -f "$gshim")" != "$(readlink -f "$home/bin/xlings")" ]]; then
  fail "A: the entry is not a link to the entry binary"
fi

# ── C. outside the project, the name runs the HOST's copy ───────────
#
# Ordered before B because B destroys the project. Run from a neutral cwd with
# the host dir after the xlings bin on PATH, which is exactly the shape a user
# has: xlings first, the system after.
out="$(cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$home" \
        PATH="$home/subos/default/bin:$hostbin:/usr/bin:/bin" \
        demo-tool 2>/dev/null)" || fail "C: passthrough exited non-zero"
[[ "$out" == "HOST-DEMO-TOOL" ]] \
  || fail "C: expected the host's copy, got: $out"
log "C ok: outside the project the name resolves to the host"

# ── D. a claim this scope cannot serve stays an error ───────────────
#
# `installed[]` carries the name, nothing is active. That is a claim, and
# running the host's copy instead would be a silent substitution.
python3 - "$home/subos/default/.xlings.json" <<'PY'
import json,os,sys
p=sys.argv[1]
j=json.load(open(p)) if os.path.exists(p) else {}
# Per-target shape: the workspace maps a TARGET to {active, installed},
# not a field name to a map. Getting this backwards parses as an empty
# workspace with no error at all.
j.setdefault("workspace",{})["claimed-tool"]={"installed":["1.0.0"]}
json.dump(j,open(p,"w"),indent=2)
PY
cp "$home/bin/xlings" "$home/subos/default/bin/claimed-tool" 2>/dev/null \
  || ln -sf "$home/bin/xlings" "$home/subos/default/bin/claimed-tool"
cat > "$hostbin/claimed-tool" <<'EOS'
#!/bin/sh
echo "HOST-CLAIMED-TOOL"
EOS
chmod +x "$hostbin/claimed-tool"

set +e
cout="$(cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$home" \
         PATH="$home/subos/default/bin:$hostbin:/usr/bin:/bin" \
         claimed-tool 2>&1)"
crc=$?
set -e
[[ $crc -ne 0 ]] \
  || fail "D: a name with installed[] but no active version must not pass through (got: $cout)"
[[ "$cout" != *"HOST-CLAIMED-TOOL"* ]] \
  || fail "D: passed through to the host despite this scope claiming the name"
log "D ok: an unmet claim is an error, not a substitution"

# ── E. a shim NAMED BY PATH must not pass through ───────────────────
#
# The granularity that separates C from a silent substitution. In C the user
# typed a bare name and PATH happened to hit xlings's file, so handing the name
# back to PATH is exactly right. Here the caller points at ONE installation --
# a project's own bin, which is never on PATH and so was never reached by
# accident -- and running a different program instead would be substituting for
# the thing they named.
projbin="$proj/.xlings/subos/_/bin"
mkdir -p "$projbin"
ln -sf "$home/bin/xlings" "$projbin/demo-tool"

set +e
eout="$(cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$home" \
         PATH="$hostbin:/usr/bin:/bin" \
         "$projbin/demo-tool" 2>&1)"
erc=$?
set -e
[[ $erc -ne 0 ]] \
  || fail "E: a shim named by path must not fall through to the host (got: $eout)"
[[ "$eout" != *"HOST-DEMO-TOOL"* ]] \
  || fail "E: passed through despite being named by an explicit path"
log "E ok: naming a shim by path is not a routing lookup"

# ── B. deleting the project reclaims its names ──────────────────────
rm -rf "$proj"
xl self doctor --fix >/dev/null 2>&1 || true

[[ ! -e "$gshim" ]] \
  || fail "B: the deleted project's command name survived a rebuild"
log "B ok: a deleted project's names are reclaimed"

# The entry binary's own shim survived every rebuild above. Seeded before the
# first `doctor --fix` precisely so this is an observation, not a tautology.
[[ -e "$home/subos/default/bin/xlings" ]] \
  || fail "B: the rebuild removed the entry binary's own shim"

log "shim table routing: all assertions passed"
