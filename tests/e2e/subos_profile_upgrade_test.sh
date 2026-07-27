#!/usr/bin/env bash
#
# E2E for `xlings self init` profile auto-upgrade based on the
# `# xlings-profile-version: <N>` marker.
#
#   * No marker (legacy v1) → overwrite with current version
#   * Same marker          → leave alone (respect user edits)
#   * Older marker         → overwrite (covered transitively by case 1)

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_profile_upgrade"
HOME_DIR="$RUNTIME_DIR/home"
XLINGS_BIN_PATH="$(find_xlings_bin)"

# Read the expected marker from the source of truth instead of pinning it.
# It was pinned at 9, the profiles moved to 10, and this test was failing
# unnoticed because it is not in run_all.sh. The property under test is
# "the emitted profile carries the marker the source declares" -- what the
# number happens to be today is not part of it.
PROFILE_VERSION="$(grep -oP 'xlings-profile-version: \K[0-9]+' \
    "$ROOT_DIR/src/core/xself/profile_resources.cppm" | head -1)"
[[ -n "$PROFILE_VERSION" ]] \
    || fail "could not read the profile version from profile_resources.cppm"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$HOME_DIR"

run_xlings() {
    env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
        "$XLINGS_BIN_PATH" "$@"
}

# ── 1. Fresh init produces a v2 profile ───────────────────────────────
log "Scenario 1: fresh init writes v2 profile"
run_xlings self init >/dev/null
grep -q "^# xlings-profile-version: $PROFILE_VERSION" "$HOME_DIR/config/shell/xlings-profile.sh" \
    || fail "S1: bash profile missing version marker"
grep -q "^# xlings-profile-version: $PROFILE_VERSION" "$HOME_DIR/config/shell/xlings-profile.fish" \
    || fail "S1: fish profile missing version marker"
grep -q "^# xlings-profile-version: $PROFILE_VERSION" "$HOME_DIR/config/shell/xlings-profile.ps1" \
    || fail "S1: pwsh profile missing version marker"

# ── 2. Legacy profile (no marker) gets upgraded ───────────────────────
log "Scenario 2: legacy v1 profile (no marker) is upgraded to v2"
cat > "$HOME_DIR/config/shell/xlings-profile.sh" <<'EOF'
# Xlings Shell Profile (bash/zsh) -- LEGACY V1 (no marker)
export XLINGS_BIN="$XLINGS_HOME/subos/current/bin"
case ":$PATH:" in
    *":$XLINGS_BIN:"*) ;;
    *) export PATH="$XLINGS_BIN:$XLINGS_HOME/bin:$PATH" ;;
esac
EOF
run_xlings self init >/dev/null
grep -q "^# xlings-profile-version: $PROFILE_VERSION" "$HOME_DIR/config/shell/xlings-profile.sh" \
    || fail "S2: legacy profile was not upgraded"
grep -q 'XLINGS_ACTIVE_SUBOS' "$HOME_DIR/config/shell/xlings-profile.sh" \
    || fail "S2: upgraded profile missing env-fallback logic"

# ── 3. Same-version user edits are preserved ──────────────────────────
log "Scenario 3: same-version profile + user comment is preserved"
echo '# my custom user comment' >> "$HOME_DIR/config/shell/xlings-profile.sh"
run_xlings self init >/dev/null
grep -q '# my custom user comment' "$HOME_DIR/config/shell/xlings-profile.sh" \
    || fail "S3: user comment was clobbered on a same-version re-init"

# ── 4. Older marker (v1 written explicitly) gets upgraded ─────────────
log "Scenario 4: explicit older version marker triggers upgrade"
cat > "$HOME_DIR/config/shell/xlings-profile.sh" <<'EOF'
# xlings-profile-version: 1
# Some older content
export FOO=bar
EOF
run_xlings self init >/dev/null
grep -q "^# xlings-profile-version: $PROFILE_VERSION" "$HOME_DIR/config/shell/xlings-profile.sh" \
    || fail "S4: v1-marked profile was not upgraded"

log "PASS: subos profile upgrade (1-4)"
