#!/usr/bin/env bash
# declared_file_assets_removal_test.sh — E2E-94
#
# openxlings/xlings#423: declared assets (`xvm.files`) were never reclaimed by
# ANY removal path, from the day they shipped in 2026.7.27.0 until 2026.8.26.1.
#
# The bug had two halves and the second is the one nothing ever tested:
#
#   * FULL UNINSTALL (the payload goes) left the links behind DANGLING. Ugly,
#     accumulating, and at least visible once you look with the right test.
#   * DETACH (another subos still uses the payload, so `remove` only opts THIS
#     subos out) left the links behind WORKING. A compiler kept finding headers
#     from a package the user had removed, and no scan for broken links could
#     ever see it.
#
# THE JUDGEMENT USED HERE, and why the obvious one is not enough:
#
#   `[ -e link ]` follows the link. With the payload deleted, a link that is
#   STILL THERE reads as "does not exist" -- which is how the original check
#   passed while the leak was on disk. `-xtype l` fixes that half.
#
#   `-xtype l` alone still cannot see the detach half, because those links
#   resolve perfectly. So the assertion below is a RECONCILIATION: every link
#   in the subos that points into the payload store must be declared by
#   something this subos has active. Difference non-empty = fail. That one
#   judgement covers both halves and does not care which mechanism leaked.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/declared_file_assets_removal"
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
      "$XLINGS_BIN" "$@" )
}
RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
      "$XLINGS_BIN" "$@" )
}

mkdir -p "$HOME_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"

# ── Two fixture packages.
#
# `leafpkg` declares NESTED assets, two levels below `usr/include`. That depth
# is the whole point: `declare_headers_tree` (29 recipes in the real index)
# produces exactly this shape, and every check that came before #423 looked
# only at the immediate children of `usr/include`.
#
# `sharedpkg` declares one destination `leafpkg` also declares. Two packages
# claiming one path is not hypothetical -- `usr/include/scsi` is claimed by
# both glibc and linux-headers on a real installation -- and it is the case
# where "delete what the removed release declared" and "leave anything another
# release declares" are BOTH wrong.
mk_pkg() {
  local name="$1" dstdir="$2" contested="$3"
  local file="$LOCAL_INDEX_DIR/pkgs/${name:0:1}/${name}.lua"
  mkdir -p "$(dirname "$file")"
  cat > "$file" <<LUA
package = {
    spec = "1", name = "$name",
    description = "fixture for tests/e2e/declared_file_assets_removal_test.sh",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "arm64"}, status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local d = pkginfo.install_dir()
    os.tryrm(d); os.mkdir(d)
    os.mkdir(path.join(d, "include", "$dstdir", "deep"))
    io.writefile(path.join(d, "include", "$dstdir", "top.h"), "$name")
    io.writefile(path.join(d, "include", "$dstdir", "deep", "nested.h"), "$name")
    if "$contested" == "yes" then
        os.mkdir(path.join(d, "include", "shared"))
        io.writefile(path.join(d, "include", "shared", "contested.h"), "$name")
    end
    return true
end

function config()
    local binding = package.name .. "@" .. pkginfo.version()
    -- No legacy fallback on purpose: this fixture only ever runs against a
    -- client being tested for asset reclamation, so a client without
    -- \`xvm.files\` should fail loudly rather than silently test nothing.
    xvm.add("$name", { bindir = pkginfo.install_dir() })
    xvm.files{ src = path.join("include", "$dstdir", "top.h"),
               dst = path.join("usr", "include", "$dstdir", "top.h"),
               binding = binding }
    xvm.files{ src = path.join("include", "$dstdir", "deep", "nested.h"),
               dst = path.join("usr", "include", "$dstdir", "deep", "nested.h"),
               binding = binding }
    if "$contested" == "yes" then
        xvm.files{ src = path.join("include", "shared", "contested.h"),
                   dst = path.join("usr", "include", "shared", "contested.h"),
                   binding = binding }
    end
    return true
end

function uninstall()
    -- Deliberately NOT mirroring the declarations by hand. The claim under
    -- test is that the client reclaims what the recipe declared; a recipe
    -- that also removes them by hand cannot tell us whether it does.
    xvm.remove("$name")
    return true
end
LUA
}

mk_pkg leafpkg leafpkg yes
mk_pkg sharedpkg sharedpkg yes

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON

log "init sandbox"
RUN self init >/dev/null 2>&1
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ── The judgement, spelled once ────────────────────────────────────────
#
# Difference between what is on disk and what this subos declares. Both sets
# are derived, neither is hand-written: a hand-written expectation is a second
# opinion that can be wrong in the same direction as the code.
reconcile() {
  python3 - "$HOME_DIR" "$1" <<'PY'
import json, os, sys
home, subos = sys.argv[1], sys.argv[2]
store = os.path.join(home, "data", "xpkgs")
root  = os.path.join(home, "subos", subos)

db = json.loads(open(os.path.join(home, ".xlings.json")).read()).get("versions", {})
ws = json.loads(open(os.path.join(root, ".xlings.json")).read()).get("workspace", {})

declared = set()
for target, entry in ws.items():
    active = entry.get("active") if isinstance(entry, dict) else entry
    if not active:
        continue
    data = (db.get(target, {}).get("versions", {}) or {}).get(active)
    if data and data.get("fileDst"):
        declared.add(data["fileDst"])

on_disk = set()
for top in ("usr", "etc", "share"):
    base = os.path.join(root, top)
    for dirpath, dirnames, filenames in os.walk(base):
        for name in list(dirnames) + filenames:
            p = os.path.join(dirpath, name)
            if os.path.islink(p) and os.readlink(p).startswith(store):
                on_disk.add(os.path.relpath(p, root))
        dirnames[:] = [d for d in dirnames
                       if not os.path.islink(os.path.join(dirpath, d))]

for leaked in sorted(on_disk - declared):
    print(leaked)
PY
}

assert_reconciled() {
  local subos="$1" what="$2"
  local leaked; leaked="$(reconcile "$subos")"
  if [[ -n "$leaked" ]]; then
    echo "  leaked in [$subos]:" >&2
    printf '    %s\n' $leaked >&2
    fail "$what"
  fi
}

# Reconciliation alone is not enough, and finding that out was worth the run.
#
# It compares disk against the workspace, so it is only as good as the
# workspace. Measured against the PRE-FIX binary: the detach left the links AND
# left `leafpkg.files.1..3` recorded as active, so "on disk but not declared"
# was empty and the leak read as clean. A judgement that the bug can satisfy is
# not a judgement.
#
# This one cannot be satisfied by bad metadata: after giving a release up, no
# link in this subos may point into that release's payload, whatever any record
# says about it.
assert_no_links_into() {
  local subos="$1" payload="$2" what="$3"
  local hits
  hits="$(find "$HOME_DIR/subos/$subos" -type l 2>/dev/null \
            -exec sh -c 'readlink "$1" | grep -q "$2" && echo "$1"' _ {} "$payload" \; || true)"
  if [[ -n "$hits" ]]; then
    echo "  still linked into $payload from [$subos]:" >&2
    printf '    %s\n' $hits >&2
    fail "$what"
  fi
}

count_links() { find "$HOME_DIR/subos/$1/usr" -type l 2>/dev/null | wc -l | tr -d ' '; }

# ══════════════════════════════════════════════════════════════════════
# S1: full uninstall in a single subos
# ══════════════════════════════════════════════════════════════════════
log "S1: install + remove in one subos"
RUN_IN default install leafpkg@1.0.0 -y >"$RUNTIME_DIR/s1-install.log" 2>&1 || fail "S1: install failed"

[[ -L "$HOME_DIR/subos/default/usr/include/leafpkg/deep/nested.h" ]] \
  || fail "S1: the nested asset was never placed — the fixture proves nothing"
S1_BEFORE="$(count_links default)"
[[ "$S1_BEFORE" -ge 3 ]] || fail "S1: expected at least 3 links, got $S1_BEFORE"
log "  installed: $S1_BEFORE link(s)"

RUN_IN default remove leafpkg -y >/dev/null 2>&1 || fail "S1: remove failed"

assert_no_links_into default "xim-x-leafpkg" \
  "S1: uninstall left links into the deleted payload (#423)"
assert_reconciled default "S1: uninstall left declared assets on disk (#423)"

# Both halves of the classic mistake, stated explicitly so a future reader
# cannot re-introduce either.
if [[ -e "$HOME_DIR/subos/default/usr/include/leafpkg/deep/nested.h" \
   || -L "$HOME_DIR/subos/default/usr/include/leafpkg/deep/nested.h" ]]; then
  fail "S1: the nested asset survived removal"
fi
[[ -d "$HOME_DIR/subos/default/usr/include/leafpkg" ]] \
  && fail "S1: the directory that only held this release's links was left behind"
[[ -d "$HOME_DIR/subos/default/usr/include" ]] \
  || fail "S1: usr/include is the sysroot's own shape and must survive"
log "  ✓ reclaimed, empty dirs swept, usr/include kept"

# ══════════════════════════════════════════════════════════════════════
# S2: DETACH — the half no test has ever covered
# ══════════════════════════════════════════════════════════════════════
#
# Two subos hold the SAME version, so removing it from one cannot delete the
# payload; `remove` takes the detach path instead. Before the fix that path
# asked `file_placement(db, "<pkg>", v)` -- a question whose answer is empty
# for every release that has ever declared an asset, because assets register
# on `<pkg>.files.<n>`. So it reclaimed nothing, and the links it left were
# not dangling: they pointed at a live payload.
log "S2: two subos share a version; remove from one (detach path)"
RUN subos new other >/dev/null 2>&1 || fail "S2: subos new failed"
RUN_IN default install leafpkg@2.0.0 -y >/dev/null 2>&1 || fail "S2: default install failed"
RUN_IN other   install leafpkg@2.0.0 -y >/dev/null 2>&1 || fail "S2: other install failed"

S2_OTHER_BEFORE="$(count_links other)"
[[ "$S2_OTHER_BEFORE" -ge 3 ]] || fail "S2: 'other' did not materialize the assets"

REMOVE_LOG="$RUNTIME_DIR/s2-remove.log"
RUN_IN default remove leafpkg -y >"$REMOVE_LOG" 2>&1 || fail "S2: remove failed"

# The payload must survive: this is what makes it a detach rather than an
# uninstall, and if it did not survive the scenario tested nothing.
[[ -d "$HOME_DIR/data/xpkgs/xim-x-leafpkg/2.0.0" ]] \
  || fail "S2: the payload was deleted — this was not the detach path"

assert_no_links_into default "xim-x-leafpkg" \
  "S2: detach left links pointing at a payload this subos gave up (#423)"
assert_reconciled default "S2: detach left declared assets pointing at a live payload (#423)"

S2_OTHER_AFTER="$(count_links other)"
[[ "$S2_OTHER_AFTER" == "$S2_OTHER_BEFORE" ]] \
  || fail "S2: detaching in 'default' changed 'other' ($S2_OTHER_BEFORE -> $S2_OTHER_AFTER)"
[[ -L "$HOME_DIR/subos/other/usr/include/leafpkg/deep/nested.h" ]] \
  || fail "S2: the other subos lost an asset it still declares"
log "  ✓ default reclaimed, other untouched ($S2_OTHER_AFTER links), payload kept"

# The detach must also drop the release's MEMBERS from this subos, not just
# the name the user typed. Leaving them is what made `self doctor --fix` the
# thing that eventually finished the job.
python3 - "$HOME_DIR/subos/default/.xlings.json" <<'PY' || fail "S2: member records survived the detach"
import json, sys
ws = json.loads(open(sys.argv[1]).read()).get("workspace", {})
stale = [k for k in ws if k.startswith("leafpkg")]
if stale:
    print("stale workspace entries:", stale)
    sys.exit(1)
PY
log "  ✓ member records dropped with the release"

# ══════════════════════════════════════════════════════════════════════
# S3: two packages declare one destination
# ══════════════════════════════════════════════════════════════════════
log "S3: contested destination, one of the two removed"
RUN_IN other remove leafpkg -y >/dev/null 2>&1 || fail "S3: cleanup remove failed"
RUN_IN default install leafpkg@1.0.0 -y   >/dev/null 2>&1 || fail "S3: install leafpkg failed"
RUN_IN default install sharedpkg@1.0.0 -y >/dev/null 2>&1 || fail "S3: install sharedpkg failed"

CONTESTED="$HOME_DIR/subos/default/usr/include/shared/contested.h"
[[ -L "$CONTESTED" ]] || fail "S3: the contested destination was never placed"

RUN_IN default remove leafpkg -y >/dev/null 2>&1 || fail "S3: remove failed"

# Not deleted: `sharedpkg` still declares it and is still active. Deleting it
# would take a header away from a package that is installed and asking for it.
[[ -L "$CONTESTED" ]] \
  || fail "S3: removing one claimant deleted a path the other still declares"
# And not left pointing into the deleted payload either, which is what "leave
# it alone if anything else declares it" would have done.
readlink "$CONTESTED" | grep -q "sharedpkg" \
  || fail "S3: the contested link still points at the removed package's payload"
[[ -e "$CONTESTED" ]] \
  || fail "S3: the contested link is dangling"
assert_reconciled default "S3: reclaiming a contested destination leaked"
log "  ✓ contested path re-pointed at the surviving claimant"

# ══════════════════════════════════════════════════════════════════════
# S4: doctor reports what --fix repairs, at any depth
# ══════════════════════════════════════════════════════════════════════
#
# Reporter and repairer must key on the same thing. This pair has been wrong
# before in both directions, and the specific failure here was that the
# reporter only ever looked one level below `usr/include`, so a leak two
# levels down was invisible to the report AND to `--fix`.
log "S4: doctor sees a nested dangling link and --fix removes it"
DEEP_DIR="$HOME_DIR/subos/default/usr/include/handmade/deeper"
mkdir -p "$DEEP_DIR"
ln -s "$HOME_DIR/data/xpkgs/xim-x-gone/9.9.9/include/gone.h" "$DEEP_DIR/gone.h"

DOCTOR_LOG="$RUNTIME_DIR/s4-doctor.log"
RUN_IN default self doctor >"$DOCTOR_LOG" 2>&1 || true
grep -q "dangling sysroot link" "$DOCTOR_LOG" \
  || { sed -n '1,60p' "$DOCTOR_LOG" >&2; fail "S4: doctor did not report a nested dangling link"; }
grep -q "handmade/deeper/gone.h" "$DOCTOR_LOG" \
  || { sed -n '1,60p' "$DOCTOR_LOG" >&2; fail "S4: the report did not name the link"; }

RUN_IN default self doctor --fix >/dev/null 2>&1 || true
[[ -L "$DEEP_DIR/gone.h" ]] && fail "S4: --fix did not remove what doctor reported"
[[ -d "$HOME_DIR/subos/default/usr/include/handmade" ]] \
  && fail "S4: --fix left the directories that only held the link"
[[ -d "$HOME_DIR/subos/default/usr/include" ]] \
  || fail "S4: --fix removed the sysroot's own shape"

RUN_IN default self doctor >"$DOCTOR_LOG.2" 2>&1 || true
grep -q "dangling sysroot link" "$DOCTOR_LOG.2" \
  && fail "S4: doctor still reports after --fix — the two disagree"
log "  ✓ reported, repaired, converged"


# ══════════════════════════════════════════════════════════════════════
# S5: one package claims a DIRECTORY another package wants to fill
# ══════════════════════════════════════════════════════════════════════
#
# The real shape this guards: `usr/include/scsi` is a whole-directory asset of
# one package while another ships different files for the same directory.
# `create_directories` treats a symlink-to-directory as "already there", so
# placing a leaf under it writes INTO THE FIRST PACKAGE'S PAYLOAD -- a store
# every subos on the machine reads and no uninstall cleans.
#
# Verified against the pre-fix binary with a five-line C++ program before this
# guard was written: the link really does land in the payload.
log "S5: a leaf placed under another package's directory asset"

DIRPKG="$LOCAL_INDEX_DIR/pkgs/d/dirpkg.lua"
mkdir -p "$(dirname "$DIRPKG")"
cat > "$DIRPKG" <<'LUA'
package = {
    spec = "1", name = "dirpkg",
    description = "fixture: claims a whole directory as one asset",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "arm64"}, status = "stable",
    categories = {"test-fixture"},
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} },
            windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local d = pkginfo.install_dir()
    os.tryrm(d); os.mkdir(d)
    os.mkdir(path.join(d, "include", "merged"))
    io.writefile(path.join(d, "include", "merged", "from-dirpkg.h"), "dirpkg")
    return true
end
function config()
    local binding = package.name .. "@" .. pkginfo.version()
    xvm.add("dirpkg", { bindir = pkginfo.install_dir() })
    -- Directory granularity: ONE node for the whole directory.
    xvm.files{ src = path.join("include", "merged"),
               dst = path.join("usr", "include", "merged"),
               binding = binding }
    return true
end
function uninstall() xvm.remove("dirpkg"); return true end
LUA

LEAFINTO="$LOCAL_INDEX_DIR/pkgs/l/leafinto.lua"
cat > "$LEAFINTO" <<'LUA'
package = {
    spec = "1", name = "leafinto",
    description = "fixture: fills a directory another package claims",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "arm64"}, status = "stable",
    categories = {"test-fixture"},
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} },
            windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local d = pkginfo.install_dir()
    os.tryrm(d); os.mkdir(d)
    os.mkdir(path.join(d, "include", "merged"))
    io.writefile(path.join(d, "include", "merged", "from-leafinto.h"), "leafinto")
    return true
end
function config()
    local binding = package.name .. "@" .. pkginfo.version()
    xvm.add("leafinto", { bindir = pkginfo.install_dir() })
    -- Leaf granularity: one node per FILE, inside the same directory.
    xvm.files{ src = path.join("include", "merged", "from-leafinto.h"),
               dst = path.join("usr", "include", "merged", "from-leafinto.h"),
               binding = binding }
    return true
end
function uninstall() xvm.remove("leafinto"); return true end
LUA

RUN subos new merge >/dev/null 2>&1 || fail "S5: subos new failed"
RUN_IN merge install dirpkg@1.0.0 -y   >/dev/null 2>&1 || fail "S5: dirpkg install failed"
[[ -L "$HOME_DIR/subos/merge/usr/include/merged" ]] \
  || fail "S5: dirpkg did not claim the directory as one link — fixture is wrong"

RUN_IN merge install leafinto@1.0.0 -y >/dev/null 2>&1 || fail "S5: leafinto install failed"

DIRPAYLOAD="$HOME_DIR/data/xpkgs/xim-x-dirpkg/1.0.0/include/merged"
[[ -e "$DIRPAYLOAD/from-leafinto.h" ]] \
  && fail "S5: the placement wrote into dirpkg's PAYLOAD — every subos on this machine reads that"

[[ -L "$HOME_DIR/subos/merge/usr/include/merged" ]] \
  && fail "S5: the directory link was not unwrapped"
[[ -d "$HOME_DIR/subos/merge/usr/include/merged" ]] \
  || fail "S5: usr/include/merged is not a real directory"
[[ -e "$HOME_DIR/subos/merge/usr/include/merged/from-leafinto.h" ]] \
  || fail "S5: the arriving header was not placed"
[[ -e "$HOME_DIR/subos/merge/usr/include/merged/from-dirpkg.h" ]] \
  || fail "S5: unwrapping lost the other package's header — it must survive as its own link"
log "  ✓ unwrapped losslessly; both packages' headers present, payload untouched"

# Reverse order: the leaf package first, then the directory claimant. The
# directory asset now lands on a real directory that already has contents, so
# it replaces it -- and that is the case the INDEX has to avoid by declaring
# both sides per-file. Recorded here rather than asserted as good: what this
# run proves is only that nothing writes into a payload.
RUN subos new merge2 >/dev/null 2>&1 || fail "S5: subos new merge2 failed"
RUN_IN merge2 install leafinto@1.0.0 -y >/dev/null 2>&1 || fail "S5: reverse leafinto failed"
RUN_IN merge2 install dirpkg@1.0.0 -y   >/dev/null 2>&1 || fail "S5: reverse dirpkg failed"
LEAFPAYLOAD="$HOME_DIR/data/xpkgs/xim-x-leafinto/1.0.0/include/merged"
[[ -e "$LEAFPAYLOAD/from-dirpkg.h" ]] \
  && fail "S5: reverse order wrote into leafinto's PAYLOAD"
log "  ✓ reverse order also leaves both payloads untouched"

# ══════════════════════════════════════════════════════════════════════
# S6: `use` to a release with a smaller asset set
# ══════════════════════════════════════════════════════════════════════
#
# The outgoing release's extra assets used to be listed as stranded members
# and left on disk -- a sysroot serving two releases of one package at once.
#
# This scenario exists because the first attempt at fixing it was a NO-OP that
# a plan-level unit test happily passed. Reclaiming asks "does anything still
# active declare this destination", and the stranded member was still active,
# so it answered yes about itself and its link was re-pointed straight back.
# Only running the command and looking at the sysroot catches that.
log "S6: use to a release that declares fewer assets"

USEPKG="$LOCAL_INDEX_DIR/pkgs/u/usepkg.lua"
mkdir -p "$(dirname "$USEPKG")"
cat > "$USEPKG" <<'LUA'
package = {
    spec = "1", name = "usepkg",
    description = "fixture: 2.0.0 declares one asset more than 1.0.0",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "arm64"}, status = "stable",
    categories = {"test-fixture"},
    xpm = { linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
            macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
            windows = { ["1.0.0"] = {}, ["2.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local d = pkginfo.install_dir(); os.tryrm(d); os.mkdir(d)
    os.mkdir(path.join(d, "include", "up"))
    io.writefile(path.join(d, "include", "up", "a.h"), pkginfo.version())
    if pkginfo.version() == "2.0.0" then
        io.writefile(path.join(d, "include", "up", "b.h"), "only in 2.0.0")
    end
    return true
end
function config()
    local binding = package.name .. "@" .. pkginfo.version()
    xvm.add("usepkg", { bindir = pkginfo.install_dir() })
    xvm.files{ src = path.join("include", "up", "a.h"),
               dst = path.join("usr", "include", "up", "a.h"),
               binding = binding }
    if pkginfo.version() == "2.0.0" then
        xvm.files{ src = path.join("include", "up", "b.h"),
                   dst = path.join("usr", "include", "up", "b.h"),
                   binding = binding }
    end
    return true
end
function uninstall() xvm.remove("usepkg"); return true end
LUA

RUN subos new switching >/dev/null 2>&1 || fail "S6: subos new failed"
RUN_IN switching install usepkg@2.0.0 -y >/dev/null 2>&1 || fail "S6: install 2.0.0 failed"
RUN_IN switching install usepkg@1.0.0 -y >/dev/null 2>&1 || fail "S6: install 1.0.0 failed"

UPDIR="$HOME_DIR/subos/switching/usr/include/up"
[[ -e "$UPDIR/b.h" ]] || fail "S6: 2.0.0's extra asset was never placed"

RUN_IN switching use usepkg 1.0.0 >/dev/null 2>&1 || fail "S6: use failed"

[[ -e "$UPDIR/a.h" ]] || fail "S6: the asset both releases declare was removed"
readlink "$UPDIR/a.h" | grep -q "1.0.0" \
  || fail "S6: the shared asset still points at the release we switched away from"
if [[ -e "$UPDIR/b.h" || -L "$UPDIR/b.h" ]]; then
  fail "S6: the asset 1.0.0 does not declare survived the switch -- the sysroot now holds two releases"
fi
assert_reconciled switching "S6: switching releases leaked an asset"
log "  ✓ shared asset moved, the outgoing-only asset reclaimed"

log "PASS: declared_file_assets_removal (S1-S6)"
