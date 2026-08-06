#!/usr/bin/env bash
# E2E: one package contributes one version's environment to a subos.
#
# The three-layer model has always said so: the xpkg store holds many versions
# by design, each consumer freezes one into its own RPATH/INTERP, and the subos
# in between is live at exactly one. Nothing enforced the middle line, so
# installing a second version appended a second provider section to the subos
# manifest and BOTH contributed.
#
# Measured on a real home before this test existed: mesa@25.0.7 and
# mesa@25.0.7.1 both bound in `default`, both on __EGL_VENDOR_LIBRARY_DIRS, EGL
# duly enumerating the device twice, and `xlings self doctor` reporting
# nothing. The two records agreed -- on an answer the model forbids. That is
# why "they agree" is never on its own evidence of anything here.
#
# The fix is NOT that a second install unbinds the first. `install` adds to the
# store and `use` selects; making install a second selector would be another
# answerer to a question that already has one. The fix is that activation reads
# xvm's answer, which lives in the same file as the declarations.
#
# What has to hold:
#   1. installing a second version KEEPS both provider sections -- the dormant
#      one is what lets `xlings use` switch back without reinstalling
#   2. but only the active version's declarations reach the environment
#   3. `xlings use` on the other version switches which one, with no reinstall
#      and no manifest rewrite
#   4. when a package has NO active version, nothing can choose, so a second
#      install supersedes at the one moment a human is naming a version
#   5. and a contested state already on disk is REPORTED, with a remedy that
#      makes the choice a decision someone took rather than a guess

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_single_version"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { [[ -n "${E2E_KEEP:-}" ]] || rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

BIN="$(find_xlings_bin)"
log "client: $("$BIN" --version 2>&1 | head -1)"

cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/o"

# Two versions of one package, each declaring a variable that names its own
# payload. Both must be installable for the test to mean anything: the point is
# not that the second install fails.
#
# xvm-registered WITH a version, the way glibc is. doctor asks xvm whether a
# package is installed here, and activation asks xvm which version is live.
cat > "$LOCAL_INDEX_DIR/pkgs/o/onlyonefixture.lua" <<'LUA'
package = {
    spec = "1",
    name = "onlyonefixture",
    description = "Local fixture for tests/e2e/subos_single_version_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.subos")
import("xim.libxpkg.xvm")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "share"))
    io.writefile(path.join(dir, "share", "version.txt"), pkginfo.version())
    return true
end

function config()
    -- No `binding` field: this IS the package's own node, and a node that
    -- binds to itself is rejected (xvm-self-binding).
    xvm.add(package.name, {
        version = pkginfo.version(),
        type = "lib",
        bindir = path.join(pkginfo.install_dir(), "share"),
        filename = "version.txt",
        alias = "version.txt",
    })
    if type(subos.env) == "function" then
        subos.env{ var = "E2E_ONLYONE_DIRS", op = "prepend",
                   value = "${pkgdir}/share",
                   binding = package.name .. "@" .. pkginfo.version() }
    end
    return true
end

function uninstall()
    xvm.remove(package.name, pkginfo.version())
    return true
end
LUA

mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/data/xim-index-repos"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$BIN" "$@" ) }

x self init >/dev/null 2>&1 || true

MANIFEST="$HOME_DIR/subos/default/.xlings.json"

providers() {
    python3 -c "
import json
d=json.load(open('$MANIFEST'))
print(' '.join(sorted(d.get('subos_info',{}).get('envs',{}))))
" 2>/dev/null
}

# What the variable would actually be, as the subos exports it.
exported() {
    x subos use default --cmd "echo \"VAL=[\$$1]\"" 2>&1 \
      | strip_ansi | sed -n 's/.*VAL=\[\(.*\)\].*/\1/p' | tail -1
}

# ── 1/2. a second version is recorded but dormant ─────────────────────
OUT="$(x install onlyonefixture@1.0.0 -y 2>&1)" \
  || { echo "$OUT" >&2; fail "install of 1.0.0 failed"; }
[[ "$(providers)" == "onlyonefixture@1.0.0" ]] \
  || fail "after installing 1.0.0 the subos should record it, records: $(providers)"

OUT="$(x install onlyonefixture@2.0.0 -y 2>&1)" \
  || { echo "$OUT" >&2; fail "install of 2.0.0 failed"; }

GOT="$(providers)"
[[ "$GOT" == "onlyonefixture@1.0.0 onlyonefixture@2.0.0" ]] \
  || fail "both provider sections must survive an install of a second version --
the dormant one is what lets \`xlings use\` switch back without reinstalling.
records: '$GOT'"
log "  ✓ both versions recorded"

VAL="$(exported E2E_ONLYONE_DIRS)"
COUNT="$(awk -F: '{print NF}' <<< "$VAL")"
[[ "$COUNT" == "1" ]] \
  || fail "the variable was exported $COUNT times over: '$VAL'.
Two versions recorded means two provider sections; only the ACTIVE one may
contribute. Exporting both is how one GPU came to be enumerated as two."
[[ "$VAL" == *"/xim-x-onlyonefixture/1.0.0/"* ]] \
  || fail "the exported value is not the active version's payload: '$VAL'
(xvm has 1.0.0 active -- install adds to the store, use selects)"
log "  ✓ only the active version contributes"

# ── 3. `use` switches which one, with no reinstall ─────────────────────
OUT="$(x use onlyonefixture@2.0.0 2>&1)" \
  || { echo "$OUT" >&2; fail "use onlyonefixture@2.0.0 failed"; }

VAL="$(exported E2E_ONLYONE_DIRS)"
[[ "$VAL" == *"/xim-x-onlyonefixture/2.0.0/"* ]] \
  || fail "\`xlings use\` did not change which version's environment is live:
'$VAL'"
COUNT="$(awk -F: '{print NF}' <<< "$VAL")"
[[ "$COUNT" == "1" ]] || fail "still exported $COUNT times over: '$VAL'"
log "  ✓ use switched the live version, no reinstall, no manifest rewrite"

[[ "$(providers)" == "onlyonefixture@1.0.0 onlyonefixture@2.0.0" ]] \
  || fail "switching rewrote the manifest; it should not have to -- the record
of what each version declares is not the record of which one is live"
log "  ✓ the manifest was not rewritten"

# ── 4/5. a contested state, the way a damaged home has it ─────────────
#
# NOT produced by installing: measured, a bare `xvm.add(name)` records an active
# version, so an ordinary install cannot reach this. It takes a manifest whose
# workspace record was lost while its declarations survived -- a payload pruned
# out from under it, a subos config copied between homes, a hand edit. There
# every provider contributes and nothing in the home can say which was meant,
# which is the state that exports every variable twice over.
python3 - "$MANIFEST" <<'PY'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
for v in ("1.0.0", "2.0.0"):
    d["subos_info"]["envs"]["lostws@" + v] = [
        {"var": "E2E_LOSTWS_DIRS", "op": "prepend", "value": "${pkgdir}/share"}
    ]
json.dump(d, open(p, "w"), indent=2)
PY

OUT="$(x self doctor 2>&1 || true)"
echo "$OUT" | strip_ansi | grep -q "double binding" \
  || fail "doctor did not report a package bound at two versions with no active
version, which is the state that exports every variable twice:
$OUT"
echo "$OUT" | strip_ansi | grep -q "xlings use lostws@" \
  || fail "the remedy does not tell the user how to decide. Nothing here CAN
decide -- that is why it is reported rather than repaired -- so the remedy has
to name the choice:
$OUT"
log "  ✓ contested binding reported, with a remedy that is a decision"

# The other package must NOT be reported: two versions with one active is
# ordinary, and the dormant section is the feature, not the defect.
if echo "$OUT" | strip_ansi | grep "double binding" | grep -q "onlyonefixture"; then
  fail "doctor reported a package that has an active version. Two versions
where one is active is how \`xlings use\` switches back without a reinstall;
reporting it would train users to delete the thing that makes that work:
$OUT"
fi
log "  ✓ a package with an active version is not reported"

log "PASS: subos single live version per package"
