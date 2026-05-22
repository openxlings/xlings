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

function install()
    -- Deliberately fail to simulate the gitcode 9-byte stub →
    -- libarchive extraction failure path that motivated the fix.
    return false
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
  ( cd /tmp && env -i HOME="$HOME" USER="$USER" SHELL="${SHELL:-/bin/sh}" \
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

echo "$out_cli" | tail -6 | sed 's/^/    /'

[[ "$rc_cli" -ne 0 ]] \
  || fail "S1: CLI exitCode was $rc_cli, expected non-zero
$out_cli"
log "  ✓ CLI exitCode = $rc_cli (non-zero, P0 fix verified)"

# ── S2: interface install_packages must propagate non-zero exitCode ──
log "S2: xlings interface install_packages → exitCode != 0 in NDJSON envelope"

set +e
out_iface="$(RUN interface install_packages \
              --args '{"targets":["brokenpkg"],"yes":true}' 2>/dev/null)"
rc_iface=$?
set -e

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
# running the install hook), but it must be empty / lack the install
# stamp because the hook returned false. Either is a valid "failed"
# fingerprint; failed=1 in install_summary above is the canonical
# proof that the failure was registered.
VERDIR="$HOME_DIR/data/xpkgs/xim-x-brokenpkg/1.0.0"
if [[ -d "$VERDIR" ]]; then
  if [[ -n "$(ls -A "$VERDIR" 2>/dev/null)" ]]; then
    fail "S3: brokenpkg verdir has content — install hook should not have populated it
$(ls -la "$VERDIR")"
  fi
  log "  ✓ brokenpkg verdir is empty (hook returned false before populating it)"
else
  log "  ✓ brokenpkg verdir absent"
fi

log "PASS: cmd_install exit-code propagation regression covered"
