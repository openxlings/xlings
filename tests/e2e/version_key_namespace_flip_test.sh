#!/usr/bin/env bash
# E2E: the spelling of a version key must not follow the ORDER of index_repos,
# and a home whose keys already did must stay usable.
#
# Reproduced on the released 2026.8.30.2 against a real home
# (.agents/docs/2026-09-02-version-key-namespace-flip-plan.md): the default
# index's entry had been moved out of `index_repos[0]` by a documented command,
# `version_namespace_()` read position, and from then on the same package keyed
# its versions two ways on one machine. Five faces, one cause:
#
#   remove   refused ("exact removal version is not registered", version='xim:0.18.0'
#            while the store held '0.18.0')
#   remove   skipped the multi-subos detach path and tried to delete a payload
#            three subos were using
#   use      landed on an owner-less duplicate record and dropped the binding group
#   install  refused with `xvm-group-conflict` and blamed the recipe
#   doctor   could not name any of it
#
# Scenarios (each one a differential that fails on 2026.8.30.2):
#   S1  keys are bare under [xim]
#   S2  keys are STILL bare under [other, xim]                       (D1)
#   S3  remove of a version another subos uses detaches, whatever the order (D2)
#   S4  a reinstall of a release keyed before the flip does not conflict (D3/D5)
#   S5  twins: `use` lands on the owned record; doctor reports, --fix merges,
#       every subos is rewritten, and the repair converges              (D6)
#   S6  keys are bare under [xim, other] too -- three orders, one answer

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/version_key_namespace_flip"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
OTHER_INDEX_DIR="$RUNTIME_DIR/other-pkgindex"

# E2E_KEEP=1 leaves the runtime home behind for inspection.
cleanup() { [[ -n "${E2E_KEEP:-}" ]] || rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"
errors=()

# A setup failure must not hide the differentials collected before it: on the
# broken binary S2 already went wrong by the time S5 cannot be set up.
fail() {
  [[ ${#errors[@]} -gt 0 ]] && printf '%s\n' "${errors[@]}" >&2
  echo "[project-e2e] FAIL: $*" >&2; exit 1
}

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

# The system python, not whatever shim is first on PATH: on a developer
# machine `python3` can be an xlings shim that refuses outside its subos.
py() { env PATH=/usr/bin:/bin python3 "$@"; }

# The keys a target is stored under, space-separated and sorted.
db_keys() {
  py - "$HOME_DIR" "$1" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1], ".xlings.json").read_text())
print(" ".join(sorted(data.get("versions", {}).get(sys.argv[2], {}).get("versions", {}))))
PY
}

ws_active()    { py - "$HOME_DIR" "$1" "$2" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1], "subos", sys.argv[2], ".xlings.json")
print(json.loads(p.read_text()).get("workspace", {}).get(sys.argv[3], {}).get("active", ""))
PY
}
ws_installed() { py - "$HOME_DIR" "$1" "$2" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1], "subos", sys.argv[2], ".xlings.json")
print(" ".join(sorted(json.loads(p.read_text()).get("workspace", {}).get(sys.argv[3], {}).get("installed", []))))
PY
}

# Rewrite index_repos in place, preserving everything else in the file.
set_index_order() {
  py - "$HOME_DIR" "$LOCAL_INDEX_DIR" "$OTHER_INDEX_DIR" "$@" <<'PY'
import json, pathlib, sys
home, xim, other, *order = sys.argv[1:]
p = pathlib.Path(home, ".xlings.json")
data = json.loads(p.read_text())
urls = {"xim": xim, "other": other}
data["index_repos"] = [{"name": n, "url": urls[n]} for n in order]
p.write_text(json.dumps(data, indent=2))
PY
}

mkdir -p "$HOME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/f"
# A second, empty index. Its only job is to be able to sit in front of `xim`.
mkdir -p "$OTHER_INDEX_DIR/pkgs"

# ── Fixtures ─────────────────────────────────────────────────────────
#
# `flip-tool`: one program, `xvm.remove` without a version -- the claude shape.
# `flip-group`: a root plus one bound member -- the libxcb shape.

cat > "$LOCAL_INDEX_DIR/pkgs/f/flip-tool.lua" <<'LUA'
package = {
    spec = "1",
    name = "flip-tool",
    description = "Local fixture: one program, versionless uninstall",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    programs = {"flip-tool"},
    xpm = {
        linux   = { ["1.0.0"] = {}, ["1.1.0"] = {}, ["1.2.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["1.1.0"] = {}, ["1.2.0"] = {} },
        windows = { ["1.0.0"] = {}, ["1.1.0"] = {}, ["1.2.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "flip-tool"),
                 "#!/bin/sh\necho flip-tool@" .. pkginfo.version() .. "\n")
    if os.host() ~= "windows" then
        os.exec("chmod +x " .. path.join(bindir, "flip-tool"))
    end
    return true
end

function config()
    xvm.add("flip-tool", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("flip-tool")
    return true
end
LUA

cat > "$LOCAL_INDEX_DIR/pkgs/f/flip-group.lua" <<'LUA'
package = {
    spec = "1",
    name = "flip-group",
    description = "Local fixture: a root and one bound member",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},
    programs = {"flip-group", "flip-member"},
    xpm = {
        linux   = { ["2.0.0"] = {} },
        macosx  = { ["2.0.0"] = {} },
        windows = { ["2.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    for _, name in ipairs({"flip-group", "flip-member"}) do
        io.writefile(path.join(bindir, name), "#!/bin/sh\necho " .. name .. "\n")
        if os.host() ~= "windows" then
            os.exec("chmod +x " .. path.join(bindir, name))
        end
    end
    return true
end

function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    xvm.add("flip-group", { bindir = bindir })
    xvm.add("flip-member", { bindir = bindir, binding = "flip-group@" .. pkginfo.version() })
    return true
end

function uninstall()
    xvm.remove("flip-group")
    xvm.remove("flip-member")
    return true
end
LUA

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

# ── S1: the default index writes bare keys ───────────────────────────
log "S1: [xim] → bare key"
RUN install flip-tool@1.0.0 -y >/dev/null 2>&1 || fail "S1: install flip-tool@1.0.0 failed"
keys="$(db_keys flip-tool)"
[[ "$keys" == "1.0.0" ]] || errors+=("S1: expected key '1.0.0', got '$keys'")

# ── S2: the default index STILL writes bare keys when it is not first ──
#
# This is the trigger. On 2026.8.30.2 the same install wrote `xim:1.1.0`.
log "S2: [other, xim] → still a bare key"
set_index_order other xim
RUN install flip-tool@1.1.0 -y >/dev/null 2>&1 || fail "S2: install flip-tool@1.1.0 failed"
keys="$(db_keys flip-tool)"
[[ "$keys" == "1.0.0 1.1.0" ]] \
  || errors+=("S2: the key spelling followed the array order: got '$keys' (expected '1.0.0 1.1.0')")

# ── S3: a version another subos uses is detached, not refused ───────
#
# `other` is a second subos that references both versions. On 2026.8.30.2,
# with `xim` not first, `remove` asked the store for `xim:1.0.0`, found
# nothing, and refused -- never reaching the branch that would have said
# "payload kept, still used by other".
log "S3: remove a version another subos still uses → detached, payload kept"
mkdir -p "$HOME_DIR/subos/other/bin"
cp "$HOME_DIR/subos/default/.xlings.json" "$HOME_DIR/subos/other/.xlings.json"
RUN use flip-tool@1.0.0 >/dev/null 2>&1 || fail "S3 setup: use flip-tool@1.0.0 failed"
[[ "$(ws_active default flip-tool)" == "1.0.0" ]] || fail "S3 setup: default should be on 1.0.0"

out="$(RUN remove flip-tool@1.0.0 -y 2>&1)" && rc=0 || rc=$?
if [[ $rc -ne 0 ]]; then
  errors+=("S3: remove refused (rc=$rc): $(printf '%s' "$out" | tail -2 | tr '\n' ' ')")
else
  printf '%s' "$out" | grep -q "kept" \
    || errors+=("S3: remove did not report the payload as kept for the other subos; got: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')")
  [[ -d "$HOME_DIR/data/xpkgs/flip-tool/1.0.0" || -d "$HOME_DIR/data/xpkgs/xim-x-flip-tool/1.0.0" ]] \
    || errors+=("S3: the payload another subos uses was deleted")
  keys="$(db_keys flip-tool)"
  [[ "$keys" == "1.0.0 1.1.0" ]] \
    || errors+=("S3: the record another subos uses was dropped from the database: '$keys'")
  inst="$(ws_installed default flip-tool)"
  [[ "$inst" != *"1.0.0"* ]] \
    || errors+=("S3: default still lists 1.0.0 as installed after remove: '$inst'")
  inst="$(ws_installed other flip-tool)"
  [[ "$inst" == *"1.0.0"* ]] \
    || errors+=("S3: the other subos lost its reference to 1.0.0: '$inst'")
fi

# ── S4: a release keyed before the flip can be reinstalled after it ──
#
# Install under [xim], flip, stamp the payload incomplete (what a failed
# config hook leaves behind), reinstall. On 2026.8.30.2 the second batch
# spelled its root `flip-group@xim:2.0.0` against the persisted
# `flip-group@2.0.0`, and registration refused with `xvm-group-conflict` --
# "the recipe puts one release under two different roots". Ten packages on
# the measured home were stuck exactly there: not installable, not removable.
log "S4: reinstall across the flip → no group conflict, one spelling"
set_index_order xim other
RUN install flip-group@2.0.0 -y >/dev/null 2>&1 || fail "S4 setup: install flip-group failed"
keys="$(db_keys flip-group)"
[[ "$keys" == "2.0.0" ]] || fail "S4 setup: expected bare key for flip-group, got '$keys'"
set_index_order other xim
stamp="$(find "$HOME_DIR/data/xpkgs" -path '*flip-group/2.0.0/.xpkg-install.json' | head -1)"
[[ -n "$stamp" ]] || fail "S4 setup: no install stamp for flip-group@2.0.0"
py - "$stamp" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
data = json.loads(p.read_text())
data["incomplete"] = True
data["reason"] = "config hook failed"
p.write_text(json.dumps(data, indent=2))
PY
out="$(RUN install flip-group@2.0.0 -y 2>&1)" && rc=0 || rc=$?
if [[ $rc -ne 0 ]]; then
  errors+=("S4: reinstall refused (rc=$rc): $(printf '%s' "$out" | grep -i 'conflict\|error' | head -2 | tr '\n' ' ')")
fi
keys="$(db_keys flip-group)"
[[ "$keys" == "2.0.0" ]] || errors+=("S4: flip-group keyed twice after reinstall: '$keys'")
keys="$(db_keys flip-member)"
[[ "$keys" == "2.0.0" ]] || errors+=("S4: flip-member keyed twice after reinstall: '$keys'")

# ── S5: twins -- one version, two spellings, one payload ─────────────
#
# The shape the flip left on the measured home (240 pairs): an owner-less
# bare record from before providers were recorded, and the owned record a
# later install wrote beside it under the other spelling.
log "S5: twins → use lands on the owned record; doctor reports; --fix merges everywhere"
py - "$HOME_DIR" <<'PY'
import copy, json, pathlib, sys
home = pathlib.Path(sys.argv[1])
root = home / ".xlings.json"
data = json.loads(root.read_text())
versions = data["versions"]["flip-tool"]["versions"]
# Whichever spelling the binary under test wrote (the broken one wrote xim:).
key = next(k for k in ("1.1.0", "xim:1.1.0") if k in versions)
owned = versions.pop(key)
assert owned.get("bindingGroup"), "S5 setup: 1.1.0 should be owned"
members = owned.get("bindingMembers")
if isinstance(members, dict):
    owned["bindingMembers"] = {t: ("xim:1.1.0" if v == key else v) for t, v in members.items()}
owned["bindingGroup"]["rootVersion"] = "xim:1.1.0"
versions["xim:1.1.0"] = owned
versions["1.1.0"] = {"path": owned["path"]}          # the owner-less twin
root.write_text(json.dumps(data, indent=2))
for name in ("default", "other"):
    p = home / "subos" / name / ".xlings.json"
    ws = json.loads(p.read_text())
    entry = ws["workspace"]["flip-tool"]
    entry["installed"] = sorted({("xim:1.1.0" if v == "1.1.0" else v) for v in entry.get("installed", [])} | {"1.1.0"})
    entry["active"] = "1.1.0"                          # both subos on the loser
    p.write_text(json.dumps(ws, indent=2))
PY
keys="$(db_keys flip-tool)"
[[ "$keys" == *"1.1.0"* && "$keys" == *"xim:1.1.0"* ]] || fail "S5 setup: twins not seeded: '$keys'"

out="$(RUN use flip-tool@1.1.0 2>&1)" || errors+=("S5: use flip-tool@1.1.0 failed: $(printf '%s' "$out" | tail -2 | tr '\n' ' ')")
active="$(ws_active default flip-tool)"
[[ "$active" == "xim:1.1.0" ]] \
  || errors+=("S5: use landed on the owner-less twin: active='$active' (expected 'xim:1.1.0')")

out="$(RUN self doctor 2>&1)" && rc=0 || rc=$?
printf '%s' "$out" | grep -q "duplicate version key" \
  || errors+=("S5: doctor did not report the duplicate key (rc=$rc)")
[[ $rc -ne 0 ]] || errors+=("S5: doctor exited 0 with a duplicate key on the home")

RUN self doctor --fix >/dev/null 2>&1 || true
keys="$(db_keys flip-tool)"
[[ "$keys" == "1.0.0 xim:1.1.0" ]] \
  || errors+=("S5: --fix did not merge the twins: '$keys' (expected '1.0.0 xim:1.1.0')")
for name in default other; do
  active="$(ws_active "$name" flip-tool)"
  [[ "$active" == "xim:1.1.0" ]] \
    || errors+=("S5: subos '$name' still names the merged record: active='$active'")
  inst="$(ws_installed "$name" flip-tool)"
  [[ "$inst" != *" 1.1.0"* && "$inst" != "1.1.0"* ]] \
    || errors+=("S5: subos '$name' still lists the merged record as installed: '$inst'")
done
out="$(RUN self doctor 2>&1)" || true
printf '%s' "$out" | grep -q "duplicate version key" \
  && errors+=("S5: the duplicate key is still reported after --fix")
out="$(RUN self doctor --fix 2>&1)" || true
printf '%s' "$out" | grep -q "folded into" \
  && errors+=("S5: --fix does not converge; a second run merged again")

# ── S6: the third order ──────────────────────────────────────────────
log "S6: [xim, other] → bare key"
set_index_order xim other
RUN install flip-tool@1.2.0 -y >/dev/null 2>&1 || fail "S6: install flip-tool@1.2.0 failed"
keys="$(db_keys flip-tool)"
[[ "$keys" == "1.0.0 1.2.0 xim:1.1.0" ]] \
  || errors+=("S6: expected '1.0.0 1.2.0 xim:1.1.0', got '$keys'")

if [[ ${#errors[@]} -gt 0 ]]; then
  printf '%s\n' "${errors[@]}" >&2
  fail "${#errors[@]} defect(s)"
fi
log "PASS: version keys do not follow index_repos order, and a home whose keys did is usable"
