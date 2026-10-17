#!/usr/bin/env bash
# E2E: a payload's currency beside its integrity, and a metadata transaction
# that is not refused by another package's dangling record.
#
# WHAT THIS DEFENDS (mcpp design 2026-10-17, X1/X2/X4; protocol 1.11)
#
# 1. `payload_status` reports `complete` as integrity only, with
#    `installed_revision`, `floor`, `usable` and `current` beside it. 1.10
#    folded a stale revision into `complete`, so a client could not tell an
#    old payload from a broken one, and mcpp replaced llvm, gcc and glibc on a
#    user's next build for revisions that changed nothing they used.
# 2. `floor` on the version entry decides `usable`: a payload recorded below
#    it is not usable; absent, every recorded revision is.
# 3. Two packages register one target (gcc and llvm both register `c++`).
#    With one package's registration gone while the target stays active at it
#    -- the user's home: `c++` active at gcc's 16.1.0, no registration -- the
#    OTHER package's replacement used to fail on every run with "sysroot
#    update refused before metadata changed: ...: installed asset registration
#    missing". It succeeds now, deactivates the dangling target and reports it
#    as `scope_repaired`.
#
# Differential: C2 and S2 fail on 2026.10.16.2 (complete=false for an old
# revision; the replacement refused).
#
# Offline: every fixture is hook-only.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/payload_currency_scope_repair"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() {
  chmod -R u+w "$RUNTIME_DIR" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

RUN() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
      XLINGS_HOME="$HOME_DIR" XLINGS_ACTIVE_SUBOS=default \
      XLINGS_NON_INTERACTIVE=1 \
      "$XLINGS_BIN" "$@" )
}

# write_owner <name> <version> <entry-fields>
#   A package that registers its own name and the shared target `sharedtool`,
#   bound to its release -- the shape of gcc and llvm registering `c++`.
write_owner() {
  local name="$1" version="$2" fields="$3"
  local recipe="$LOCAL_INDEX_DIR/pkgs/${name:0:1}/$name.lua"
  mkdir -p "$(dirname "$recipe")"
  cat > "$recipe" <<LUA
package = {
    spec = "1", name = "$name",
    description = "scope repair fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "aarch64"}, status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["$version"] = { $fields } },
        macosx  = { ["$version"] = { $fields } },
        windows = { ["$version"] = { $fields } },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "$name"), "$name-$version\n")
    io.writefile(path.join(bindir, "$name-tool"), "$name-tool-$version\n")
    return true
end
function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    local binding = "$name@" .. pkginfo.version()
    xvm.add("$name", { bindir = bindir })
    xvm.add("sharedtool", { bindir = bindir, alias = "$name-tool", binding = binding })
    return true
end
function uninstall()
    xvm.remove("sharedtool")
    xvm.remove("$name")
    return true
end
LUA
  rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
}

# The one `<kind>` data payload of an NDJSON transcript, as JSON.
PY_DATA='
import json, sys
kind = sys.argv[1]
hits = []
for line in sys.stdin:
    line = line.strip()
    if line.startswith("{"):
        try: e = json.loads(line)
        except ValueError: continue
        if e.get("kind") == "data" and e.get("dataKind") == kind: hits.append(e["payload"])
if len(hits) != 1:
    sys.exit("expected one %s event, got %d" % (kind, len(hits)))
print(json.dumps(hits[0]))
'
data_of() { python3 -c "$PY_DATA" "$2" <<<"$1"; }
# expect_fields <json-object> key=value ...  (value `null` matches JSON null)
PY_EXPECT='
import json, sys
row = json.loads(sys.argv[1])
bad = []
for pair in sys.argv[2:]:
    key, want = pair.split("=", 1)
    got = row.get(key, "<absent>")
    shown = "null" if got is None else (str(got).lower() if isinstance(got, bool) else str(got))
    if shown != want:
        bad.append("%s: want %s, got %r" % (key, want, got))
if bad:
    sys.exit("; ".join(bad) + "\n  row: " + json.dumps(row))
'
expect_fields() { local row="$1"; shift; python3 -c "$PY_EXPECT" "$row" "$@" || fail "fields do not match"; }
status_of() {
  local out
  out="$(RUN interface payload_status --args "{\"targets\":[\"$1\"]}" 2>/dev/null)" \
    || fail "payload_status $1 failed:
$out"
  data_of "$out" payload_status | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)["targets"][0]))'
}

mkdir -p "$HOME_DIR/subos/default/bin"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
write_owner ownera 1.0 ''
write_owner ownerb 2.0 ''
cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON
RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

VERSION_FEATURES="$(RUN interface --version)"
grep -q '"payload_currency"' <<<"$VERSION_FEATURES" \
  || fail "the handshake does not announce payload_currency: $VERSION_FEATURES"

# ── C1: a current payload ────────────────────────────────────────────
log "C1: a fresh payload is complete, current and usable"
RUN install ownerb@2.0 -y >/dev/null 2>&1 || fail "C1: install ownerb failed"
ROW="$(status_of ownerb@2.0)"
expect_fields "$ROW" complete=true current=true usable=true revision=0 \
  installed_revision=0 floor=0 origin=local
log "  ok"

# ── C2: the recipe moves to revision 1, no floor ─────────────────────
log "C2: an old revision is complete and usable, not current"
write_owner ownerb 2.0 'revision = 1'
ROW="$(status_of ownerb@2.0)"
expect_fields "$ROW" complete=true current=false usable=true revision=1 \
  installed_revision=0 floor=0
log "  ok"

# ── C3: the recipe declares floor 1 ──────────────────────────────────
log "C3: below the floor it is complete and not usable"
write_owner ownerb 2.0 'revision = 1, floor = 1'
ROW="$(status_of ownerb@2.0)"
expect_fields "$ROW" complete=true current=false usable=false revision=1 \
  installed_revision=0 floor=1
log "  ok"

# ── C4: an absent payload ────────────────────────────────────────────
log "C4: an uninstalled version is absent and nothing else"
ROW="$(status_of ownera@1.0)"
expect_fields "$ROW" complete=false current=false usable=false origin=absent \
  installed_revision=null
log "  ok"

# ── S1: the user's home shape ────────────────────────────────────────
log "S1: sharedtool active at ownera's 1.0, whose registration is gone"
write_owner ownerb 2.0 ''
RUN install ownera@1.0 -y >/dev/null 2>&1 || fail "S1: install ownera failed"
RUN use sharedtool 1.0 >/dev/null 2>&1 || fail "S1: use sharedtool 1.0 failed"
python3 - "$HOME_DIR" <<'PY'
import json, os, sys
home = sys.argv[1]
def drop(path, key):
    if not os.path.exists(path): return False
    doc = json.load(open(path))
    versions = doc.get(key, doc).get("versions") if key else doc.get("versions")
    if not versions or "sharedtool" not in versions: return False
    versions["sharedtool"]["versions"].pop("1.0", None)
    open(path, "w").write(json.dumps(doc, indent=2) + "\n")
    return True
changed = drop(os.path.join(home, ".xlings.json"), None)
vdb = os.path.join(home, "data", "versions.json")
if os.path.exists(vdb):
    doc = json.load(open(vdb))
    if "sharedtool" in doc.get("versions", {}):
        doc["versions"]["sharedtool"]["versions"].pop("1.0", None)
        open(vdb, "w").write(json.dumps(doc, indent=2) + "\n")
        changed = True
ws = json.load(open(os.path.join(home, "subos", "default", ".xlings.json")))["workspace"]
active = ws.get("sharedtool")
active = active.get("active") if isinstance(active, dict) else active
if not changed or active != "1.0":
    sys.exit("fixture: changed=%s active=%r" % (changed, active))
PY
DOCTOR="$(RUN self doctor 2>&1 | strip_ansi || true)"
grep -q "sharedtool" <<<"$DOCTOR" || fail "S1: doctor does not name the dangling target:
$DOCTOR"
if grep -q "xlings install sharedtool" <<<"$DOCTOR"; then
  fail "S1: doctor sends the user to install a target as if it were a package:
$DOCTOR"
fi
log "  ok"

# ── S2: another package's replacement succeeds and reports the repair ─
log "S2: ownerb's replacement succeeds and reports sharedtool deactivated"
write_owner ownerb 2.0 'revision = 1'
OUT="$(RUN interface install_packages --args '{"targets":["ownerb@2.0"],"yes":true}' 2>/dev/null)" \
  || fail "S2: the replacement failed:
$OUT"
if grep -q "installed asset registration missing" <<<"$OUT"; then
  fail "S2: refused by the dangling record:
$OUT"
fi
REPAIRS="$(data_of "$OUT" scope_repaired)" || fail "S2: no scope_repaired event:
$OUT"
python3 -c '
import json, sys
repairs = json.loads(sys.argv[1])["repairs"]
want = {"target": "sharedtool", "version": "1.0", "action": "deactivated"}
sys.exit(0 if want in repairs else "scope_repaired: %r" % repairs)
' "$REPAIRS" || fail "S2: the repair is not reported as expected"
ROW="$(status_of ownerb@2.0)"
expect_fields "$ROW" complete=true current=true usable=true installed_revision=1
TARGETS="$(data_of "$OUT" install_targets)"
python3 -c '
import json, sys
row = json.loads(sys.argv[1])["targets"][0]
for k, v in {"status": "installed", "complete": True, "current": True, "usable": True, "installed_revision": 1}.items():
    if row.get(k) != v: sys.exit("install_targets %s: %r" % (k, row))
' "$TARGETS" || fail "S2: install_targets"
log "  ok"

# ── S3: nothing dangles any more ─────────────────────────────────────
log "S3: a second install reports no repair"
OUT="$(RUN interface install_packages --args '{"targets":["ownerb@2.0"],"yes":true}' 2>/dev/null)" \
  || fail "S3: install failed"
if grep -q '"dataKind":"scope_repaired"' <<<"$OUT"; then
  fail "S3: a repair reported on a coherent scope:
$OUT"
fi
STATE="$(RUN interface system_status --args '{}' 2>/dev/null)" || fail "S3: system_status failed"
FINDINGS="$(data_of "$STATE" binding_state)" || fail "S3: no binding_state:
$STATE"
# The scope no longer references the missing version. ownera's own release
# still names it -- the database half of the damage, which `self doctor --fix`
# repairs -- and the summary says so.
python3 -c '
import json, sys
findings = json.loads(sys.argv[1])["findings"]
refs = [x for x in findings if x["target"] == "sharedtool"
        and x["code"] in ("xvm-active-version-missing", "xvm-installed-version-missing")]
if refs: sys.exit("still referenced: %r" % refs)
if not any(x["code"] == "xvm-legacy-edge-dangling" for x in findings):
    sys.exit("the release damage is not summarized: %r" % findings)
' "$FINDINGS" || fail "S3: binding_state"
log "  ok"

# ── S4: the owner's own reinstall restores the record ───────────────
# gcc's reinstall registers `c++@16.1.0` again: the scope ends where it began,
# so nothing is reported as repaired.
log "S4: a reinstall of the record's owner reports no repair"
python3 - "$HOME_DIR" <<'PY' || fail "S4: fixture"
import json, os, sys
wsf = os.path.join(sys.argv[1], "subos", "default", ".xlings.json")
doc = json.load(open(wsf)); ws = doc["workspace"]
entry = ws.get("sharedtool")
if isinstance(entry, dict): entry["active"] = "1.0"
else: ws["sharedtool"] = "1.0"
json.dump(doc, open(wsf, "w"), indent=2)
PY
write_owner ownera 1.0 'revision = 1'
OUT="$(RUN interface install_packages --args '{"targets":["ownera@1.0"],"yes":true}' 2>/dev/null)" \
  || fail "S4: the owner's reinstall failed:
$OUT"
if grep -q '"dataKind":"scope_repaired"' <<<"$OUT"; then
  fail "S4: a record the reinstall restored is reported as repaired:
$OUT"
fi
log "  ok"

log "PASS: payload currency and scope repair"
