#!/usr/bin/env bash
# install_silent_failure_test.sh — regression test for the bug where
# `xlings install` (and `interface install_packages`) returned exitCode
# 0 even when individual packages failed to install.
#
# The bug-trigger scenario was: gitcode's file-cdn returns 200 OK + a
# 9-byte body "Not Found" for missing release artifacts. xlings treated
# the 9-byte file as a successful download, libarchive failed at
# extract, the failure was logged + accumulated into `failedCount`, but
# then `cmd_install` discarded `failedCount` and returned 0. Programmatic
# consumers (mcpp via `interface install_packages`) trusted the exitCode
# and reported false success.
#
# This e2e exercises the same return-code propagation path through a
# simpler trigger: an install hook that deliberately returns false.
# That accumulates failedCount via InstallPhase::Failed exactly the
# same way as the gitcode 9-byte → libarchive failure does. We test:
#
#   S1: CLI `xlings install <broken-pkg> -y` must exit non-zero
#   S2: `xlings interface install_packages` must report exitCode != 0
#       in its NDJSON {"exitCode":N,"kind":"result"} envelope — that's
#       the exact failure mode mcpp tripped over
#   S3: install_summary event must report failed=1 (sanity check on
#       the existing instrumentation we're now propagating to exitCode)
#   S5: a refusal after the install hook succeeded (here: an env
#       declaration for a package it does not own, refused while the
#       release is configured) reaches the interface WITH its reason --
#       the message used to be empty (#652)
#   S4: `self doctor --fix -y` against the same always-fails payload must
#       exit non-zero and must NOT stamp `verifiedBy` — an IncompletePayload
#       finding that survives the repair ladder is an outstanding failure,
#       not a silently-accepted one (2026.9.12 Item B)
#
# The downloader size-check (P1 fix) cannot be exercised here without
# spinning up an HTTPS server with a trusted cert (tinyhttps rejects
# HTTP and verifies certs). It is covered by the unit test
# SubosCmdInstall.LooksLikeArchiveFilename in tests/unit/test_main.cpp
# and by direct code review of the size-check at downloader.cppm.
#
# Refs: .agents/docs/2026-05-22-cmd-install-silent-failure-analysis.md

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/install_silent_failure"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

mkdir -p "$HOME_DIR/subos/default/bin" \
         "$LOCAL_INDEX_DIR/pkgs/b"

# Fixture package whose install hook deliberately returns false. The
# package declares no URL (type=script, install hook is the install
# path), so no download happens — the failure comes purely from the
# hook returning false. This drives the executor to emit
# InstallPhase::Failed → cmd_install increments failedCount → with the
# fix in place, returns non-zero.
cat > "$LOCAL_INDEX_DIR/pkgs/b/brokenpkg.lua" <<'LUA'
package = {
    spec = "1",
    name = "brokenpkg",
    description = "Test fixture: install hook always fails (regression coverage for cmd_install exit code propagation)",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.log")

function install()
    io.write(string.rep("P", 17000))
    print("REPRO stdout")
    io.stderr:write("REPRO stderr\n")
    log.error("REPRO log.error")
    return false
end
LUA

# S5's fixture: the install succeeds, then configuring the release is
# refused -- it declares an environment variable for a package it does not
# own. No config hook, so the refusal comes from the registration step
# itself, the path that reported `[strayenv] failed: ` with no reason.
mkdir -p "$LOCAL_INDEX_DIR/pkgs/s"
cat > "$LOCAL_INDEX_DIR/pkgs/s/strayenv.lua" <<'LUA'
package = {
    spec = "1",
    name = "strayenv",
    description = "Test fixture: configuring the release is refused (#652)",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.subos")

function install()
    subos.env{ var = "STRAYENV_PROBE", op = "prepend", value = "${subosdir}/x", binding = "someoneelse@1.0.0" }
    return true
end
LUA

# Neutralise sub-index repos so the test stays offline
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"

# Home config — point the xim namespace at our local fixture
cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "version": "0.4.39",
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "subos": {"default": {"dir": ""}},
  "index_repos": [
    {"name": "xim", "url": "$LOCAL_INDEX_DIR"}
  ]
}
JSON

RUN() {
  ( cd /tmp && env -i HOME="$HOME_DIR" USER=xlings-test SHELL=/bin/sh \
      PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" "$XLINGS_BIN" "$@" )
}

RUN self init >/dev/null 2>&1 || fail "self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"

# ── S1: CLI `xlings install brokenpkg -y` must exit non-zero ─────────
log "S1: xlings install brokenpkg -y → expect non-zero exit"

set +e
out_cli="$(RUN install brokenpkg -y 2>&1)"
rc_cli=$?
set -e

echo "$out_cli" \
  | sed -E 's/P{80,}/[bounded hook transcript]/g' \
  | tail -8 \
  | sed 's/^/    /'

[[ "$rc_cli" -ne 0 ]] \
  || fail "S1: CLI exitCode was $rc_cli, expected non-zero
$out_cli"
for marker in \
  "install hook returned false" \
  "REPRO stdout" \
  "REPRO stderr" \
  "REPRO log.error" \
  "[libxpkg: hook output truncated]"; do
  [[ "$out_cli" == *"$marker"* ]] \
    || fail "S1: CLI failure omitted '$marker'
$out_cli"
done
log "  ✓ CLI exitCode = $rc_cli (non-zero, P0 fix verified)"

# ── S2: interface install_packages must propagate non-zero exitCode ──
log "S2: xlings interface install_packages → exitCode != 0 in NDJSON envelope"

set +e
out_iface="$(RUN interface install_packages \
              --args '{"targets":["brokenpkg"],"yes":true}' \
              2>"$RUNTIME_DIR/interface.stderr")"
rc_iface=$?
set -e

printf '%s\n' "$out_iface" > "$RUNTIME_DIR/interface.stdout"
python3 - "$RUNTIME_DIR/interface.stdout" <<'PY' \
  || fail "S2: interface stdout was not pure NDJSON or lacked the bounded hook transcript
$out_iface"
import json
import pathlib
import sys

events = []
for line_number, line in enumerate(pathlib.Path(sys.argv[1]).read_text().splitlines(), 1):
    if not line:
        continue
    try:
        events.append(json.loads(line))
    except json.JSONDecodeError as error:
        raise SystemExit(f"line {line_number} is not JSON: {error}: {line!r}")

required = (
    "install hook returned false",
    "REPRO stdout",
    "REPRO stderr",
    "REPRO log.error",
    "[libxpkg: hook output truncated]",
)
messages = [
    event.get("message", "")
    for event in events
    if event.get("code") == "E_INTERNAL"
]
if len(messages) != 1:
    raise SystemExit(f"expected exactly one E_INTERNAL message, got {len(messages)}")
if not all(marker in messages[0] for marker in required):
    raise SystemExit(f"E_INTERNAL message lacks hook diagnostics: {messages[0]!r}")
if len(messages[0].encode("utf-8")) > 16 * 1024 + 1024:
    raise SystemExit(f"E_INTERNAL hook transcript is not bounded: {len(messages[0].encode('utf-8'))} bytes")
PY

last_line="$(echo "$out_iface" | grep -E '"kind":"result"' | tail -1)"
log "  result envelope: $last_line"

[[ "$rc_iface" -ne 0 ]] \
  || fail "S2: shell exit was $rc_iface, expected non-zero
$out_iface"

# Parse exitCode from envelope without depending on python3 in the test
ec="$(printf '%s' "$last_line" | sed -nE 's/.*"exitCode":[[:space:]]*([-0-9]+).*/\1/p')"
[[ -n "$ec" && "$ec" != "0" ]] \
  || fail "S2: interface envelope exitCode was '$ec', expected non-zero — this is the original regression
$out_iface"
log "  ✓ interface envelope exitCode = $ec (non-zero — fixes mcpp false-positive)"

# ── S3: install_summary event must show failed=1 ─────────────────────
log "S3: install_summary event reports failed=1"
echo "$out_iface" | grep -qE '"failed":1' \
  || fail "S3: install_summary did not show failed=1
$out_iface"
log "  ✓ install_summary reports failed=1"

# Verdir may have been pre-created by installer.execute (see
# installer.cppm — it create_directories(ctx.install_dir) before
# running the install hook), and since 2026.8.11.1 a failed hook writes
# `.xpkg-install.json` with `"incomplete": true` INTO it.
#
# That marker is the point, not an exception to it. Emptiness was only ever
# a proxy for "this does not look installed", and it is a weak one: the next
# `xlings install` decided from the directory alone, so a hook that failed
# after unpacking anything left a payload that read as complete and was never
# retried (#541 ①). The marker is what makes the failure retryable.
#
# So the assertion is now the stronger one — no PAYLOAD, and the failure
# recorded — rather than the weaker "nothing at all".
VERDIR="$HOME_DIR/data/xpkgs/xim-x-brokenpkg/1.0.0"
STAMP=".xpkg-install.json"

# The marker is REQUIRED, not tolerated.
#
# Making it conditional ("if a stamp exists, it must say incomplete") would be
# the vacuous gate this repo has written before: if the marker silently stopped
# being written, the check would pass through the empty-directory branch and
# report success about the exact regression it exists to catch. The hook
# returned false, so the marker must be there.
[[ -f "$VERDIR/$STAMP" ]] || fail "S3: brokenpkg has no failure marker at $VERDIR/$STAMP —
a failed install that leaves no record of itself is one the next \`xlings install\`
cannot distinguish from a finished one, which is what made #541 ① permanent
$(ls -la "$VERDIR" 2>/dev/null || echo '(verdir absent)')"

grep -q '"incomplete"[[:space:]]*:[[:space:]]*true' "$VERDIR/$STAMP" \
  || fail "S3: brokenpkg carries a stamp that does not record the failure —
a stamp without \`incomplete\` reads as a SUCCESSFUL install
$(cat "$VERDIR/$STAMP")"

# Anything other than our own bookkeeping is payload, and payload here would
# mean the hook populated a directory it then failed out of.
leftover="$(ls -A "$VERDIR" 2>/dev/null | grep -v "^${STAMP}\$" || true)"
[[ -z "$leftover" ]] || fail "S3: brokenpkg verdir holds payload — install hook should not have populated it
$(ls -la "$VERDIR")"

log "  ✓ brokenpkg verdir has no payload and records the failed install"

# ── S4: an IncompletePayload finding that SURVIVES the repair ladder
#        blocks the verified stamp and the exit code (2026.9.12, Item B) ──
#
# brokenpkg's install() hook always returns false, so `self doctor --fix`'s
# own reinstall attempt (repair_incomplete_) fails exactly the same way the
# original install did — the marker is still on disk after `--fix` runs,
# re-detected as IncompletePayload on the very same re-scan. Before this,
# `outstanding` only looked at BrokenPayload/ForeignPayload findings, so a
# home whose ONLY remaining defect was this failed reinstall could still
# stamp `verifiedBy` and report success while returning 1 — a straight
# contradiction between the stamp and the exit code.
log "S4: self doctor --fix -y on a payload whose install() always fails"
set +e
out_doctor="$(RUN self doctor --fix -y 2>&1)"
rc_doctor=$?
set -e
echo "$out_doctor" | tail -20 | sed 's/^/    | /'

[[ "$rc_doctor" -ne 0 ]] \
  || fail "S4: self doctor --fix -y should exit non-zero while brokenpkg's reinstall keeps failing
$out_doctor"

[[ -f "$VERDIR/$STAMP" ]] \
  || fail "S4: the incomplete marker should still be there — its own reinstall keeps failing"
grep -q '"incomplete"[[:space:]]*:[[:space:]]*true' "$VERDIR/$STAMP" \
  || fail "S4: the marker must still say incomplete after the failed re-fix attempt"

s4_verified="$(python3 -c "
import json, pathlib
print(json.loads(pathlib.Path('$HOME_DIR/.xlings.json').read_text()).get('verifiedBy', ''))
")"
[[ -z "$s4_verified" ]] \
  || fail "S4: verifiedBy must not be written while an IncompletePayload finding survived the fix; got '$s4_verified'"
log "  ✓ doctor --fix exited $rc_doctor and did not stamp verifiedBy"

# ── S5: a refusal while configuring reaches the interface with its reason ──
log "S5: interface install_packages strayenv → the error names why (#652)"
set +e
out_s5="$(RUN interface install_packages --args '{"targets":["strayenv"],"yes":true}' 2>/dev/null)"
set -e
printf '%s\n' "$out_s5" > "$RUNTIME_DIR/s5.stdout"
python3 - "$RUNTIME_DIR/s5.stdout" <<'PY' || fail "S5: the failure's reason did not reach the interface
$out_s5"
import json, pathlib, sys
events = [json.loads(l) for l in pathlib.Path(sys.argv[1]).read_text().splitlines() if l.strip()]
texts = []
for e in events:
    if e.get("kind") == "error":
        texts.append(e.get("message", ""))
    if e.get("kind") == "data":
        texts.append(json.dumps(e.get("payload", "")))
joined = "\n".join(texts)
if "does not own" not in joined or "STRAYENV_PROBE" not in joined:
    print("no reason in:", joined[:2000]); sys.exit(1)
if "failed: \"" in joined or joined.rstrip().endswith("failed:"):
    print("an empty reason is still reported:", joined[:2000]); sys.exit(1)
PY
log "  ✓ the refusal's reason reaches the interface"

log "PASS: cmd_install exit-code propagation regression covered"
