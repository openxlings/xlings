#!/usr/bin/env bash
# E2E-35: a recipe declaring a spec this build does not implement is refused.
#
# `spec` used to be compared only against the literal "2", so any other value
# fell through to V1 semantics without a word. That is fail-open on a gate
# built to fail closed -- spec "2" is precisely the revision that made `archs`
# enforcement mandatory, so a client ignoring it does not skip a nicety, it
# installs the wrong architecture silently.
#
# Two properties, and the second is the one a unit test cannot show:
#
#   1. a spec above the cap is refused, with a message naming the cap
#   2. the refusal is scoped to that package -- a well-formed sibling in the
#      same install command still installs
#
# (2) matters because the alternative implementation, aborting the
# transaction, would turn one bad recipe in a shared index into an outage for
# everything installed alongside it.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/xpkg_spec_gate"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

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
mkdir -p "$LOCAL_INDEX_DIR/pkgs/f" "$LOCAL_INDEX_DIR/pkgs/s"

# Identical recipes but for `spec`. Keeping everything else the same is what
# makes the comparison mean anything.
write_fixture() {  # <name> <spec> <path>
  cat > "$3" <<LUA
package = {
    spec = "$2",
    name = "$1",
    description = "Local fixture for tests/e2e/xpkg_spec_gate_test.sh",
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
    io.writefile(path.join(dir, "bin", "$1"), "#!/bin/sh\necho $1\n")
    os.exec("chmod +x " .. path.join(dir, "bin", "$1"))
    return true
end

function config()
    xvm.add("$1", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall() return true end
LUA
}

# Distinct names per scenario: once a package has been through an install
# command it is recorded as handled and drops out of the next plan, so
# reusing one would make scenario 2 assert against an empty plan.
write_fixture "specfuture"  "3" "$LOCAL_INDEX_DIR/pkgs/s/specfuture.lua"
write_fixture "specfuture2" "3" "$LOCAL_INDEX_DIR/pkgs/s/specfuture2.lua"
write_fixture "specfine"    "1" "$LOCAL_INDEX_DIR/pkgs/f/specfine.lua"

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

# ── Scenario 1: the refusal happens and says why ─────────────────────────
log "scenario 1: a spec above the cap is refused"
OUT="$(RUN install specfuture@1.0.0 -y 2>&1 || true)"
if ! grep -qi 'spec "3" is newer' <<<"$OUT"; then
  echo "$OUT" >&2
  fail "installing a spec-3 recipe was not refused with a spec message"
fi
grep -qi 'self update' <<<"$OUT" \
  || fail "the refusal gave the user no way forward"

if RUN list 2>/dev/null | grep -q 'specfuture'; then
  fail "a refused package was registered anyway"
fi
log "  ✓ refused, and nothing was registered"

# ── Scenario 2: the refusal does not take the sibling down with it ───────
log "scenario 2: a well-formed package installs alongside the refused one"
OUT="$(RUN install specfuture2@1.0.0 specfine@1.0.0 -y 2>&1 || true)"
if ! grep -qi 'spec "3" is newer' <<<"$OUT"; then
  echo "$OUT" >&2
  fail "the spec-3 recipe was not refused in the combined install"
fi
if ! RUN list 2>/dev/null | grep -q 'specfine'; then
  echo "$OUT" >&2
  fail "one unreadable recipe blocked a healthy one — the gate is too coarse"
fi
log "  ✓ per-package skip, transaction survived"

log "E2E-35 xpkg spec gate: PASS"
