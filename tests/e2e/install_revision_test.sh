#!/usr/bin/env bash
# E2E: a recipe's packaging revision, and the install_targets record.
#
# WHAT THIS DEFENDS (openxlings/xlings#620, interface protocol 1.1)
#
# 1. `revision` on a version entry counts changes to what a recipe installs
#    under an unchanged upstream version. A payload is current iff the revision
#    its stamp records equals the recipe's. Without that, `install` of a version
#    already on disk returned on the strength of the version database and the
#    `installed` probe, and a fixed recipe never reached a machine that had
#    installed the broken one.
# 2. A stamp written before the field existed reads as revision 0: a recipe
#    that states none reinstalls nothing, one that states 1 reaches it.
# 3. The reinstall never leaves the version without a payload: a failing
#    reinstall puts the previous payload back.
# 4. The top-level install reports what each request resolved to
#    (`install_targets`) on every path, including "already installed" and
#    failures, exactly once per invocation; a dry run does not report.
# 5. The reinstall's rollback is a C++ destructor and never runs when the
#    process is killed instead of unwound: a marker written before the old
#    payload is parked lets the NEXT install finish the job -- put the
#    parked payload back, or discard it -- instead of leaking it under
#    <data>/stale/ forever.
#
# Offline: the fixture is hook-only (no download).
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/install_revision"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
RECIPE="$LOCAL_INDEX_DIR/pkgs/r/revpkg.lua"

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

# write_recipe <revision|-> <ok|fail>
#   `-` writes an entry that states no revision at all.
#   `fail` makes the install hook write part of a payload and then fail.
write_recipe() {
  local rev="$1" mode="$2" entry label
  if [[ "$rev" == "-" ]]; then entry="{}"; label="r0"
  else entry="{ revision = $rev }"; label="r$rev"; fi
  mkdir -p "$(dirname "$RECIPE")"
  cat > "$RECIPE" <<LUA
package = {
    spec = "1", name = "revpkg",
    description = "packaging revision fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64", "aarch64"}, status = "stable",
    categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0"] = $entry },
        macosx  = { ["1.0"] = $entry },
        windows = { ["1.0"] = $entry },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.mkdir(bindir)
    if "$mode" == "fail" then
        io.writefile(path.join(bindir, "half-written"), "partial\n")
        return false
    end
    io.writefile(path.join(bindir, "revpkg"), "revpkg-1.0-$label\n")
    return true
end
function config()
    xvm.add("revpkg", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end
function uninstall() xvm.remove("revpkg") return true end
LUA
  rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
}

# The single install_targets payload of one NDJSON transcript, as JSON.
# Fails unless there is exactly one: nested installs must not add their own.
PY_TARGETS='
import json, sys
events = []
for line in sys.stdin:
    line = line.strip()
    if line.startswith("{"):
        try: events.append(json.loads(line))
        except ValueError: pass
hits = [e for e in events
        if e.get("kind") == "data" and e.get("dataKind") == "install_targets"]
want = int(sys.argv[1])
if len(hits) != want:
    sys.exit("expected %d install_targets event(s), got %d" % (want, len(hits)))
if hits:
    print(json.dumps(hits[0]["payload"]))
'
# expect_target <payload-json> <index> key=value ...
PY_EXPECT='
import json, sys
payload = json.loads(sys.argv[1])
target = payload["targets"][int(sys.argv[2])]
bad = []
for pair in sys.argv[3:]:
    key, want = pair.split("=", 1)
    got = target.get(key)
    if str(got) != want:
        bad.append("%s: want %r, got %r" % (key, want, got))
if bad:
    sys.exit("; ".join(bad) + "\n  entry: " + json.dumps(target))
'
targets_of() { python3 -c "$PY_TARGETS" "${2:-1}" <<<"$1"; }
expect_target() {
  local payload="$1" index="$2"; shift 2
  python3 -c "$PY_EXPECT" "$payload" "$index" "$@" \
    || fail "install_targets entry $index does not match"
}
has_kind() { grep -q "\"dataKind\":\"$2\"" <<<"$1"; }

mkdir -p "$HOME_DIR/subos/default/bin"
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
write_recipe - ok

cp "$XLINGS_BIN" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<JSON
{ "mirror": "GLOBAL",
  "index_repos": [{ "name": "xim", "url": "$LOCAL_INDEX_DIR" }] }
JSON
RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-revpkg/1.0"
STAMP="$PAYLOAD/.xpkg-install.json"
ARGS='{"targets":["revpkg@1.0"],"yes":true}'

# ── R1: fresh install ────────────────────────────────────────────────
log "R1: a fresh install reports installed, with the payload directory"
OUT="$(RUN interface install_packages --args "$ARGS" 2>/dev/null)" \
  || fail "R1: install failed:
$OUT"
T="$(targets_of "$OUT")" || fail "R1: $OUT"
expect_target "$T" 0 request=revpkg@1.0 namespace=xim name=revpkg \
  version=1.0 revision=0 status=installed "payload_dir=$PAYLOAD"
[[ "$(cat "$PAYLOAD/bin/revpkg")" == "revpkg-1.0-r0" ]] \
  || fail "R1: the payload is not on disk"
grep -q '"revision": 0' "$STAMP" \
  || fail "R1: the stamp does not record the revision: $(cat "$STAMP")"
python3 -c '
import json, sys
for line in sys.stdin:
    e = json.loads(line) if line.startswith("{") else {}
    if e.get("dataKind") == "install_plan":
        entry = e["payload"]["packages"][0]
        if len(entry) != 3 or entry[2] != 0:
            sys.exit("install_plan entry carries no revision: %r" % entry)
        sys.exit(0)
sys.exit("no install_plan event")
' <<<"$OUT" || fail "R1: install_plan"
log "  ok"

# ── R2: already installed ────────────────────────────────────────────
log "R2: everything already installed still reports, as already_present"
OUT="$(RUN interface install_packages --args "$ARGS" 2>/dev/null)" \
  || fail "R2: install failed:
$OUT"
T="$(targets_of "$OUT")" || fail "R2: $OUT"
expect_target "$T" 0 request=revpkg@1.0 version=1.0 revision=0 \
  status=already_present "payload_dir=$PAYLOAD"
if has_kind "$OUT" install_plan; then fail "R2: a plan for nothing to do"; fi
log "  ok"

# ── R3: a stamp from before revisions existed ────────────────────────
log "R3: a stamp without the field is revision 0 -- nothing is reinstalled"
python3 - "$STAMP" <<'PY'
import json, sys
path = sys.argv[1]
stamp = json.load(open(path))
stamp.pop("revision", None)
stamp["xlings_version"] = "2026.9.26.3"
open(path, "w").write(json.dumps(stamp, indent=2) + "\n")
PY
if grep -q '"revision"' "$STAMP"; then fail "R3: fixture still carries the field"; fi
printf 'sentinel\n' > "$PAYLOAD/keep-me"
OUT="$(RUN interface install_packages --args "$ARGS" 2>/dev/null)" \
  || fail "R3: install failed"
T="$(targets_of "$OUT")" || fail "R3: $OUT"
expect_target "$T" 0 status=already_present revision=0
[[ -f "$PAYLOAD/keep-me" ]] || fail "R3: a revision-less recipe reinstalled"
log "  ok"

# ── R4: the recipe states revision 1 ─────────────────────────────────
log "R4: revision 1 reinstalls the version in place, and says why"
write_recipe 1 ok
printf 'ok\n' > "$PAYLOAD/.mcpp_ok"
OUT="$(RUN install revpkg@1.0 -y 2>&1 | strip_ansi)" \
  || fail "R4: the reinstall failed:
$OUT"
echo "$OUT" | sed 's/^/    /'
grep -qF "reinstalling revpkg@1.0: recipe revision 1, installed revision 0" \
  <<<"$OUT" || fail "R4: the output does not say why it reinstalled"
[[ "$(cat "$PAYLOAD/bin/revpkg")" == "revpkg-1.0-r1" ]] \
  || fail "R4: the payload was not replaced -- the fast path answered"
grep -q '"revision": 1' "$STAMP" || fail "R4: stamp: $(cat "$STAMP")"
[[ ! -e "$PAYLOAD/keep-me" ]] || fail "R4: the old tree was kept"
[[ ! -e "$PAYLOAD/.mcpp_ok" ]] \
  || fail "R4: a marker about the old payload survived into the new one"
[[ ! -e "$HOME_DIR/data/stale" ]] \
  || fail "R4: the parked payload was not removed: $(ls -R "$HOME_DIR/data/stale")"
log "  ok"

log "R5: the reinstalled payload is current"
OUT="$(RUN interface install_packages --args "$ARGS" 2>/dev/null)" \
  || fail "R5: install failed"
T="$(targets_of "$OUT")" || fail "R5: $OUT"
expect_target "$T" 0 status=already_present revision=1 "payload_dir=$PAYLOAD"
log "  ok"

# ── R6: a reinstall that fails keeps the previous payload ────────────
log "R6: a failing reinstall puts the previous payload back"
write_recipe 2 fail
set +e
OUT="$(RUN interface install_packages --args "$ARGS" 2>/dev/null)"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || fail "R6: a failed reinstall exited 0"
T="$(targets_of "$OUT")" || fail "R6: $OUT"
expect_target "$T" 0 request=revpkg@1.0 version=1.0 revision=2 \
  status=failed payload_dir=
[[ "$(cat "$PAYLOAD/bin/revpkg" 2>/dev/null)" == "revpkg-1.0-r1" ]] \
  || fail "R6: the previous payload is gone after a failed reinstall"
[[ ! -e "$PAYLOAD/bin/half-written" ]] || fail "R6: the partial payload stayed"
grep -q '"revision": 1' "$STAMP" || fail "R6: stamp: $(cat "$STAMP")"
if grep -q '"incomplete"' "$STAMP"; then
  fail "R6: the restored payload is marked incomplete"
fi
[[ ! -e "$HOME_DIR/data/stale" ]] || fail "R6: a parked payload was left behind"

set +e
OUT="$(RUN install revpkg@1.0 -y 2>&1 | strip_ansi)"
set -e
grep -qF "reinstalling revpkg@1.0: recipe revision 2, installed revision 1" \
  <<<"$OUT" || fail "R6: no reason printed:
$OUT"
grep -qF "kept the previous payload" <<<"$OUT" \
  || fail "R6: the restore is not reported:
$OUT"
[[ "$(cat "$PAYLOAD/bin/revpkg")" == "revpkg-1.0-r1" ]] \
  || fail "R6: the CLI run lost the previous payload"
log "  ok"

# ── R7: a request that resolves to nothing ───────────────────────────
log "R7: an unresolvable request is reported as failed"
set +e
OUT="$(RUN interface install_packages \
        --args '{"targets":["xim:revpkg-nosuch"],"yes":true}' 2>/dev/null)"
set -e
T="$(targets_of "$OUT")" || fail "R7: $OUT"
expect_target "$T" 0 request=xim:revpkg-nosuch namespace= name= version= \
  status=failed payload_dir=
log "  ok"

# ── R8: a dry run does not report ────────────────────────────────────
log "R8: plan_install emits no install_targets"
write_recipe 1 ok
OUT="$(RUN interface plan_install --args '{"targets":["revpkg@1.0"]}' 2>/dev/null)" \
  || fail "R8: plan_install failed"
targets_of "$OUT" 0 >/dev/null || fail "R8: a dry run reported install_targets"
log "  ok"

# ── R9: a killed reinstall's parked payload is recovered ─────────────
#
# set_aside_payload's rollback (PayloadReplacement) is a C++ destructor: it
# never runs when the process is killed instead of unwound, so a crash right
# after the rename into <data>/stale/ used to leak that directory forever.
# This fabricates exactly that shape -- a parked payload, a marker naming a
# dead pid, and the original path missing (the crash landed before
# set_aside_payload even recreated the empty placeholder) -- and asserts
# that the NEXT install notices and finishes the job.
log "R9: a killed reinstall's parked payload is recovered on the next install"
[[ "$(cat "$PAYLOAD/bin/revpkg")" == "revpkg-1.0-r1" ]] \
  || fail "R9: precondition: the payload from R6 is not what this case expects"

STALE_ROOT="$HOME_DIR/data/stale"
PARKED="$STALE_ROOT/xim-x-revpkg-1.0.stale-999999"
rm -rf "$STALE_ROOT"
mkdir -p "$PARKED/bin"
cp "$PAYLOAD/bin/revpkg" "$PARKED/bin/revpkg"
cp "$STAMP" "$PARKED/.xpkg-install.json"
# Written by ONLY this fabricated parked copy, never by the recipe's own
# install() -- proves a later assertion sees the RESTORED payload, not one a
# fresh install hook happened to reproduce identically.
printf 'parked\n' > "$PARKED/bin/PARKED_MARKER"

# A pid guaranteed not to be running: spawn a no-op subshell and reap it
# before writing the marker, rather than guessing a number.
( : ) & DEADPID=$!
wait "$DEADPID" 2>/dev/null || true

cat > "${PARKED}.origin" <<JSON
{
  "payload_dir": "$PAYLOAD",
  "pid": $DEADPID
}
JSON

rm -rf "$PAYLOAD"   # the crash landed before the placeholder was recreated

OUT="$(RUN install revpkg@1.0 -y 2>&1 | strip_ansi)" \
  || fail "R9: the install failed:
$OUT"
echo "$OUT" | sed 's/^/    /'

grep -qF "recovered a parked payload" <<<"$OUT" \
  || fail "R9: the recovery is not logged:
$OUT"
[[ -f "$PAYLOAD/bin/PARKED_MARKER" ]] \
  || fail "R9: the payload is not the recovered one -- a fresh install ran instead"
[[ "$(cat "$PAYLOAD/bin/revpkg")" == "revpkg-1.0-r1" ]] \
  || fail "R9: the recovered payload does not hold the parked content"
[[ ! -e "$STALE_ROOT" ]] \
  || fail "R9: stale/ was not cleared: $(ls -R "$STALE_ROOT")"
log "  ok"

log "PASS: packaging revision and install_targets"
