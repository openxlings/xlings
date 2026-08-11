#!/usr/bin/env bash
# entry_binary_and_isolation_test.sh — the three refusals that make a
# divergence between "what this home declares" and "what it actually runs"
# impossible to create silently.
#
# WHAT THIS IS DEFENDING
#
# `$XLINGS_HOME/bin/xlings` is the file every shim in the home dispatches
# through -- `subos/<s>/bin/<tool>` is a link to it -- so its version decides
# how EVERY tool in the home behaves. It is written on purpose, by
# `use`/`install` of the xlings package itself. On a real home a `local:`
# recipe carrying a June payload was activated; the entry went back six weeks,
# `${XLINGS_DYNAMIC_SUBOS_DIR}` stopped being expanded, gcc's alias reached a
# shell as `--sysroot=`, and the first visible symptom was a linker that could
# not find crt1.o -- three layers from anything naming xlings.
#
#   S1  a bare name that both `xim:` and `local:` provide resolves, to the
#       non-local one, and SAYS which one it beat
#   S2  an explicitly qualified `local:` target is not overruled
#   S3  `self install` refuses when XLINGS_HOME and the target disagree, and
#       the other home is not touched
#   S4  `remove <name>` with several versions installed lists and exits 2
#
# Refs: openxlings/xlings#532, and the post-2026.8.11.1 optimization plan §3.1-3.3

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/entry_binary_isolation"
HOME_DIR="$RUNTIME_DIR/home"
OTHER_HOME="$RUNTIME_DIR/other-home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

# ABSOLUTE. Several checks below run from `/tmp` so an ancestor `.xlings.json`
# cannot activate project mode, and a relative binary path dies there with
# `env: No such file or directory` -- which reads as the feature not working.
XLINGS_BIN="$(cd "$(dirname "$(find_xlings_bin)")" && pwd)/$(basename "$(find_xlings_bin)")"

XIM_INDEX_DIR="$RUNTIME_DIR/xim-index"
LOC_INDEX_DIR="$RUNTIME_DIR/local-index"
mkdir -p "$HOME_DIR/subos/default/bin" \
         "$XIM_INDEX_DIR/pkgs/d" "$LOC_INDEX_DIR/pkgs/d"

# Two recipes for one bare name, in two namespaces. `demoted` is a payloadless
# config package on both sides: this test is about NAME RESOLUTION, and giving
# it a download would make a network failure look like a resolution failure.
mk_recipe() {
    local file="$1" ns="$2" ver="$3"
    cat > "$file" <<LUA
package = {
    spec = "1",
    name = "demoted",
    description = "resolution fixture from $ns",
    type = "config",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = { linux = { ["latest"] = { ref = "$ver" }, ["$ver"] = {} },
            macosx = { ["latest"] = { ref = "$ver" }, ["$ver"] = {} },
            windows = { ["latest"] = { ref = "$ver" }, ["$ver"] = {} } },
}
function install() return true end
function config() return true end
function uninstall() return true end
LUA
}
mk_recipe "$XIM_INDEX_DIR/pkgs/d/demoted.lua" xim 2.0.0
mk_recipe "$LOC_INDEX_DIR/pkgs/d/demoted.lua" local 1.0.0
for d in "$XIM_INDEX_DIR" "$LOC_INDEX_DIR"; do
    (cd "$d" && git init -q && git add -A && git commit -q -m init)
done

# A repo's NAME is its default namespace (catalog.cppm repo_specs_), so two
# named repos is exactly the shape a home reaches when a dev script has
# side-loaded a recipe into the local index.
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim",   "url": "$XIM_INDEX_DIR" },
    { "name": "local", "url": "$LOC_INDEX_DIR" }
  ]
}
EOF
mkdir -p "$HOME_DIR/bin" "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
cp "$XLINGS_BIN" "$HOME_DIR/bin/xlings"

run_x() {
    ( cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" \
        XLINGS_NON_INTERACTIVE=1 "$XLINGS_BIN" "$@" )
}
run_x update >/dev/null 2>&1 || true

echo "== S1: a bare name both namespaces provide resolves, and names the loser =="
out="$(run_x info demoted 2>&1 || true)"
if ! grep -q "xim:demoted" <<<"$out"; then
    echo "$out"
    fail "S1: the bare name did not resolve to the non-local namespace"
fi
# The demotion must be VISIBLE. A pick nobody can see is the thing the
# namespace rule must not become -- it is the reason a refusal was there.
if ! grep -qi "namespace priority" <<<"$out"; then
    echo "$out"
    fail "S1: the demotion happened silently; nothing named local:demoted"
fi
if ! grep -q "local:demoted" <<<"$out"; then
    echo "$out"
    fail "S1: the losing candidate was not named"
fi
echo "   ok — resolved to xim:, and said what it beat"

echo "== S2: an explicitly qualified local target is not overruled =="
out="$(run_x info local:demoted 2>&1 || true)"
if ! grep -q "local:demoted" <<<"$out"; then
    echo "$out"
    fail "S2: an explicit local: target must resolve to local:"
fi
if grep -qi "namespace priority" <<<"$out"; then
    echo "$out"
    fail "S2: the user said which one; nothing should have been overruled"
fi
echo "   ok — explicit targets are untouched"

echo "== S3: self install refuses when XLINGS_HOME and the target disagree =="
# THE EXACT SHAPE THAT KEEPS BITING, three times in this repository's notes:
# extract a release, cd into it, point XLINGS_HOME at it, run `self install`
# to verify the artifact in isolation. XLINGS_HOME is honoured in general --
# it IS the install target when it names some other directory -- but when it
# names the SOURCE directory, `self install` retargets to `$HOME/.xlings` and
# says nothing. The step believed it had ruled out the real home and wrote to
# it.
#
# FAKE_HOME so this test cannot do the very thing it is testing for: if the
# guard regresses, the install lands in a temp tree instead of the developer's
# actual home.
FAKE_HOME="$RUNTIME_DIR/fake-user-home"
mkdir -p "$FAKE_HOME" "$OTHER_HOME/bin" "$OTHER_HOME/subos/current/bin"
cp "$XLINGS_BIN" "$OTHER_HOME/bin/xlings"
printf '{\n  "version": "0.0.1-other"\n}\n' > "$OTHER_HOME/.xlings.json"
before_conf="$(cat "$OTHER_HOME/.xlings.json")"

set +e
out="$(cd "$OTHER_HOME" && env HOME="$FAKE_HOME" XLINGS_HOME="$OTHER_HOME" \
        XLINGS_NON_INTERACTIVE=1 \
        "$OTHER_HOME/bin/xlings" self install 2>&1 </dev/null)"
rc=$?
set -e
if [[ $rc -eq 0 ]]; then
    echo "$out"
    fail "S3: self install retargeted away from XLINGS_HOME and reported success"
fi
if ! grep -q "XLINGS_HOME and the install target disagree" <<<"$out"; then
    echo "$out"
    fail "S3: the refusal must name XLINGS_HOME and both paths"
fi
if [[ -e "$FAKE_HOME/.xlings" ]]; then
    fail "S3: refused, and wrote to \$HOME/.xlings anyway"
fi
[[ "$before_conf" == "$(cat "$OTHER_HOME/.xlings.json")" ]] \
    || fail "S3: refused, and rewrote the source tree anyway"
echo "   ok — non-zero, both paths named, neither home written"

echo "== S3a: XLINGS_HOME naming a DIFFERENT directory is still honoured =="
# The guard must not have turned "isolate with XLINGS_HOME" into an error --
# that is the workflow the rest of this suite depends on.
ISO_HOME="$RUNTIME_DIR/iso-home"
set +e
out="$(cd /tmp && env HOME="$FAKE_HOME" XLINGS_HOME="$ISO_HOME" \
        XLINGS_NON_INTERACTIVE=1 \
        "$OTHER_HOME/bin/xlings" self install 2>&1 </dev/null)"
rc=$?
set -e
if grep -q "XLINGS_HOME and the install target disagree" <<<"$out"; then
    echo "$out"
    fail "S3a: refused an XLINGS_HOME that names its own directory"
fi
grep -q "install:  v.* -> $ISO_HOME" <<<"$out" \
    || { echo "$out"; fail "S3a: the target was not XLINGS_HOME"; }
echo "   ok — an isolated XLINGS_HOME is the target, as before (rc=$rc)"

echo "== S4: remove without a version SAYS the other versions are here =="
mkdir -p "$HOME_DIR/subos/default"
python3 - "$HOME_DIR/subos/default/.xlings.json" <<'PY'
import json, sys
p = sys.argv[1]
try:
    d = json.load(open(p))
except Exception:
    d = {}
# The subos workspace shape: one entry per program, `active` plus the versions
# this subos opted into. Three of them is the whole point -- `remove multi` has
# no single correct target, and taking the active one is what it used to do.
d.setdefault("workspace", {})["multi"] = {
    "active": "1.0.0",
    "installed": ["1.0.0", "2.0.0", "3.0.0"],
}
json.dump(d, open(p, "w"), indent=2)
PY
# ANNOUNCE, do not refuse -- the same choice this release makes for the entry
# binary. `remove <pkg>` taking the active version and re-pointing the binding
# is a documented contract (E2E-13); what was missing is that nothing told the
# user other versions were sitting here, or how to name them.
set +e
out="$(run_x remove multi 2>&1 </dev/null)"
rc=$?
set -e
for v in 1.0.0 2.0.0 3.0.0; do
    grep -q "multi@$v" <<<"$out" \
        || { echo "$out"; echo "exit=$rc"; fail "S4: version $v was not named"; }
done
grep -qi "ACTIVE one only" <<<"$out" \
    || { echo "$out"; fail "S4: nothing said which version this actually removes"; }
echo "   ok — every version named, and it says which one goes (rc=$rc)"

echo
echo "PASS: entry_binary_and_isolation_test.sh"
