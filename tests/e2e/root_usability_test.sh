#!/usr/bin/env bash
# E2E: xlings usability under the Linux root user.
#
# Verifies the root / sudo execution-identity layer end to end on the
# real release binary:
#   Scenario A (pure root): `self install` succeeds WITHOUT ever shelling
#     out to sudo (P0-1) and leaves a consistent root-owned home. A
#     sabotaging `sudo` shim on PATH fails the test if anything invokes it.
#   Scenario B (simulated sudo): install artifacts are chowned back to the
#     invoking user (P0-2), so a later non-sudo run isn't locked out.
#
# Design: .agents/docs/2026-06-21-root-privilege-identity-design.md
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/release_test_lib.sh"

ARCHIVE_PATH="${1:-$ROOT_DIR/build/release.tar.gz}"
require_release_archive "$ARCHIVE_PATH"

# Meaningful only as root. Off-root (local dev) skip cleanly so the suite
# still passes for non-root contributors.
if [[ "$(id -u)" -ne 0 ]]; then
  log "SKIP: root usability test requires EUID 0 (current uid=$(id -u))"
  exit 0
fi

owner_uid() { stat -c '%u' "$1"; }

# ── Scenario A: pure root, sudo must NOT be invoked (P0-1) ───────────
log "Scenario A: pure-root self install without any sudo dependency"

PKG_DIR_A="$(extract_release_archive "$ARCHIVE_PATH" root_pure)"
A_HOME="$RUNTIME_ROOT/root_pure_home"
rm -rf "$A_HOME"; mkdir -p "$A_HOME"

# A sabotaging `sudo` that fails loudly: if xlings shells out to sudo while
# already root, this trips the test (the historical hardcoded "sudo mount"
# etc. would have). Minimal root containers often lack sudo entirely.
SHIMDIR="$RUNTIME_ROOT/root_nosudo_shim"
rm -rf "$SHIMDIR"; mkdir -p "$SHIMDIR"
cat > "$SHIMDIR/sudo" <<'EOF'
#!/bin/sh
echo "[root-e2e] FAIL: xlings invoked 'sudo' while already root" >&2
exit 97
EOF
chmod +x "$SHIMDIR/sudo"

env -u XLINGS_HOME -u SUDO_UID -u SUDO_GID -u SUDO_USER \
  HOME="$A_HOME" PATH="$SHIMDIR:$(minimal_system_path)" \
  XLINGS_NON_INTERACTIVE=1 \
  "$PKG_DIR_A/bin/xlings" self install

A_INSTALLED="$A_HOME/.xlings"
[[ -x "$A_INSTALLED/bin/xlings" ]] || fail "pure-root install missing bin/xlings"
[[ "$(owner_uid "$A_INSTALLED/bin/xlings")" == "0" ]] || \
  fail "pure-root install should be root-owned (uid 0)"

# The installed binary runs as root with sudo still sabotaged.
env -u XLINGS_HOME HOME="$A_HOME" \
  PATH="$A_INSTALLED/subos/current/bin:$A_INSTALLED/bin:$SHIMDIR:$(minimal_system_path)" \
  "$A_INSTALLED/bin/xlings" -h >/dev/null || fail "pure-root 'xlings -h' failed"
env -u XLINGS_HOME HOME="$A_HOME" \
  PATH="$A_INSTALLED/subos/current/bin:$A_INSTALLED/bin:$SHIMDIR:$(minimal_system_path)" \
  "$A_INSTALLED/bin/xlings" --verbose config >/dev/null 2>&1 || \
  fail "pure-root 'xlings config' failed"
log "PASS Scenario A (pure root, no sudo)"

# ── Scenario B: simulated sudo chowns install back to invoker (P0-2) ─
log "Scenario B: simulated sudo chowns ~/.xlings back to the invoking user"

PKG_DIR_B="$(extract_release_archive "$ARCHIVE_PATH" root_sudo)"
B_HOME="$RUNTIME_ROOT/root_sudo_home"
rm -rf "$B_HOME"; mkdir -p "$B_HOME"

# Numeric ids of the "real" user behind the sudo. lchown takes a numeric
# uid/gid, so the user need not exist in the container's passwd db.
INVOKER_UID=12345
INVOKER_GID=12345

# Reproduce exactly what `sudo` leaves in the environment (root EUID +
# SUDO_*), then assert xlings hands ownership back to the invoker.
env -u XLINGS_HOME \
  HOME="$B_HOME" PATH="$(minimal_system_path)" \
  SUDO_UID="$INVOKER_UID" SUDO_GID="$INVOKER_GID" SUDO_USER="tester" \
  XLINGS_NON_INTERACTIVE=1 \
  "$PKG_DIR_B/bin/xlings" self install

B_INSTALLED="$B_HOME/.xlings"
[[ -x "$B_INSTALLED/bin/xlings" ]] || fail "sudo-sim install missing bin/xlings"

got_uid="$(owner_uid "$B_INSTALLED/bin/xlings")"
[[ "$got_uid" == "$INVOKER_UID" ]] || \
  fail "sudo-sim install not chowned back (bin/xlings uid=$got_uid, want $INVOKER_UID)"

# A nested file (deeper than the top level) must be chowned too — proves
# the recursive walk, not just a top-level chown.
nested="$(find "$B_INSTALLED" -type f | head -1)"
[[ -n "$nested" ]] || fail "no files found under installed home"
[[ "$(owner_uid "$nested")" == "$INVOKER_UID" ]] || \
  fail "nested file not chowned to invoker: $nested"
log "PASS Scenario B (simulated sudo chown-back)"

log "PASS: root usability scenarios"
