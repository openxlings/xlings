#!/usr/bin/env bash
# E2E test for `xlings self uninstall`. Builds an isolated XLINGS_HOME
# from the release tarball, exercises:
#   - --dry-run leaves home untouched, exit 0
#   - empty stdin (no confirmation) → exit 1, home untouched
#   - bogus self action → exit 2 (unknown-action regression guard)
#   - bogus uninstall flag → exit 2
#   - safety refusal: XLINGS_HOME=/  / XLINGS_HOME=$HOME → exit 1
#   - --keep-data -y keeps data/ and every subos (their homes are user data), exit 0
#   - full -y uninstall removes XLINGS_HOME entirely, exit 0
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/release_test_lib.sh"

ARCHIVE_PATH="${1:-$ROOT_DIR/build/release.tar.gz}"
require_release_archive "$ARCHIVE_PATH"

PKG_DIR="$(extract_release_archive "$ARCHIVE_PATH" self_uninstall)"
INSTALL_USER_DIR="$RUNTIME_ROOT/self_uninstall_user"

bootstrap_home() {
    rm -rf "$INSTALL_USER_DIR"
    mkdir -p "$INSTALL_USER_DIR"
    HOME="$INSTALL_USER_DIR" \
    PATH="$(minimal_system_path)" \
    env -u XLINGS_HOME "$PKG_DIR/bin/xlings" self install >/dev/null
    INSTALLED_HOME="$INSTALL_USER_DIR/.xlings"
    [[ -d "$INSTALLED_HOME" ]] || fail "bootstrap home missing: $INSTALLED_HOME"
}

run_uninstall() {
    # $1 ... = extra args; uses isolated HOME + XLINGS_HOME, project-context
    # cleared, runs from a neutral cwd. Returns the actual exit code.
    local rc=0
    ( cd /tmp && \
      HOME="$INSTALL_USER_DIR" \
      XLINGS_HOME="$INSTALLED_HOME" \
      PATH="$(minimal_system_path)" \
      env -u XLINGS_PROJECT_DIR "$INSTALLED_HOME/bin/xlings" self uninstall "$@" \
      </dev/null >/dev/null 2>&1 ) || rc=$?
    echo "$rc"
}

# T1 — --dry-run is a no-op
bootstrap_home
files_before="$(find "$INSTALLED_HOME" -type f | wc -l)"
rc="$(run_uninstall --dry-run -y)"
[[ "$rc" == "0" ]] || fail "T1: --dry-run -y exit code $rc != 0"
files_after="$(find "$INSTALLED_HOME" -type f | wc -l)"
[[ "$files_before" == "$files_after" ]] || \
    fail "T1: --dry-run modified files ($files_before → $files_after)"
log "PASS T1: --dry-run -y leaves home untouched, exit 0"

# T2 — empty stdin → cancelled, exit 1
rc="$(run_uninstall)"
[[ "$rc" == "1" ]] || fail "T2: empty stdin exit code $rc != 1"
files_after="$(find "$INSTALLED_HOME" -type f | wc -l)"
[[ "$files_before" == "$files_after" ]] || \
    fail "T2: cancel modified files"
log "PASS T2: empty stdin → exit 1, home untouched"

# T3 — bogus self action → exit 2 (regression: previously silent exit 0)
rc=0
HOME="$INSTALL_USER_DIR" \
XLINGS_HOME="$INSTALLED_HOME" \
PATH="$(minimal_system_path)" \
env -u XLINGS_PROJECT_DIR "$INSTALLED_HOME/bin/xlings" self bogus-action \
    >/dev/null 2>&1 || rc=$?
[[ "$rc" == "2" ]] || fail "T3: bogus self action exit $rc != 2"
log "PASS T3: bogus self action → exit 2"

# T4 — bogus uninstall flag → exit 2
rc="$(run_uninstall --bogus-flag -y)"
[[ "$rc" == "2" ]] || fail "T4: bogus flag exit $rc != 2"
log "PASS T4: bogus uninstall flag → exit 2"

# T5 — safety refusal when XLINGS_HOME=/
rc=0
HOME="$INSTALL_USER_DIR" \
XLINGS_HOME=/ \
PATH="$(minimal_system_path)" \
env -u XLINGS_PROJECT_DIR "$INSTALLED_HOME/bin/xlings" self uninstall -y \
    </dev/null >/dev/null 2>&1 || rc=$?
[[ "$rc" == "1" ]] || fail "T5: safety reject XLINGS_HOME=/ exit $rc != 1"
log "PASS T5: XLINGS_HOME=/ refused (exit 1)"

# T6 — safety refusal when XLINGS_HOME=$HOME
rc=0
HOME="$INSTALL_USER_DIR" \
XLINGS_HOME="$INSTALL_USER_DIR" \
PATH="$(minimal_system_path)" \
env -u XLINGS_PROJECT_DIR "$INSTALLED_HOME/bin/xlings" self uninstall -y \
    </dev/null >/dev/null 2>&1 || rc=$?
[[ "$rc" == "1" ]] || fail "T6: safety reject XLINGS_HOME=\$HOME exit $rc != 1"
log "PASS T6: XLINGS_HOME=\$HOME refused (exit 1)"

# T7 — --keep-data -y keeps the user's data: data/ AND every subos with its
# home. It used to print "data: KEEP" and delete every subos home regardless.
mkdir -p "$INSTALLED_HOME/subos/default/home/u" && echo work > "$INSTALLED_HOME/subos/default/home/u/notes.txt"
rc="$(run_uninstall --keep-data -y)"
[[ "$rc" == "0" ]] || fail "T7: --keep-data -y exit $rc != 0"
[[ -d "$INSTALLED_HOME/data" ]] || fail "T7: data/ should survive --keep-data"
[[ ! -d "$INSTALLED_HOME/bin" ]] || fail "T7: bin/ should be removed"
[[ -f "$INSTALLED_HOME/subos/default/home/u/notes.txt" ]] || fail "T7: a subos home should survive --keep-data"
[[ ! -d "$INSTALLED_HOME/config" ]] || fail "T7: config/ should be removed"
[[ ! -f "$INSTALLED_HOME/.xlings.json" ]] || fail "T7: .xlings.json should be removed"
log "PASS T7: --keep-data -y removes bin/config + state, keeps data/ and subos homes"

# T8 — full -y uninstall removes XLINGS_HOME entirely
bootstrap_home
rc="$(run_uninstall -y)"
[[ "$rc" == "0" ]] || fail "T8: full -y exit $rc != 0"
[[ ! -d "$INSTALLED_HOME" ]] || \
    fail "T8: XLINGS_HOME should be gone, but $INSTALLED_HOME still exists"
log "PASS T8: full -y uninstall removes XLINGS_HOME entirely"

# Cleanup
rm -rf "$INSTALL_USER_DIR"

log "PASS: self uninstall scenario"
