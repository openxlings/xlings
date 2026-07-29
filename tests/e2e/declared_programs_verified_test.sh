#!/usr/bin/env bash
# declared_programs_verified_test.sh — a package that promises programs and
# registers none of them must fail the install, not print a checkmark.
#
# Two real installs printed `✓ 1 package(s) installed` and left the user with
# nothing (#447):
#
#   llvm on Windows   — its recipe's directory listing came back empty, so no
#                       `clang` shim was ever registered. `xlings use llvm`
#                       then also reported success, and `clang --version` said
#                       "'clang' is not installed".
#   gcc on Windows    — the toolchain it delegates to aborted in its config
#                       hook, so nothing registered.
#
# In both cases every hook returned success. The install count counts recipes
# that did not raise, which is not the same as packages that work. Recipes
# state what they provide in `package.programs`, so the promise is checkable.
#
# Scenarios:
#   S1  declares programs, registers none  → install exits non-zero, and the
#       message names the missing program
#   S2  declares NO programs, registers none → install succeeds
#
# S2 is the falsifiability control and is the whole reason this test is worth
# having. Without it, S1 passing would be equally consistent with "the fixture
# fails to install for some unrelated reason" — the two fixtures are identical
# apart from the `programs` line, so S2 pins the failure to that field.

set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/declared_programs_verified"
HOME_DIR="$RUNTIME_DIR/home"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"

mkdir -p "$HOME_DIR/subos/default/bin" "$LOCAL_INDEX_DIR/pkgs/p"

# Fixture A — promises `ghostprog`, registers nothing. Both hooks return true,
# so nothing in the install pipeline has any other reason to complain.
cat > "$LOCAL_INDEX_DIR/pkgs/p/promiser.lua" <<'LUA'
package = {
    spec = "1",
    name = "promiser",
    description = "Test fixture: declares a program and registers none",
    type = "package",
    archs = {"x86_64", "aarch64"},
    status = "stable",
    programs = {"ghostprog"},
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

function install() return true end
function config()  return true end
LUA

# Fixture B — byte-identical except it promises nothing. Registering nothing is
# then correct, and the install must still succeed.
sed -e 's/name = "promiser"/name = "quietpkg"/' \
    -e '/programs = {"ghostprog"},/d' \
    -e 's/declares a program and registers none/declares no programs/' \
    "$LOCAL_INDEX_DIR/pkgs/p/promiser.lua" \
    > "$LOCAL_INDEX_DIR/pkgs/p/quietpkg.lua"

# Keep the test offline: no sub-index repos to fetch.
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"

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

# ── S1: promises a program, registers none → must fail ───────────────
log "S1: xlings install promiser -y → expect non-zero exit"

set +e
out_s1="$(RUN install promiser -y 2>&1 | tr -d "\0"; exit "${PIPESTATUS[0]}")"
rc_s1=$?
set -e

echo "$out_s1" | tail -6 | sed 's/^/    /'

[[ "$rc_s1" -ne 0 ]] \
  || fail "S1: exit code was $rc_s1, expected non-zero — an install that
registered none of its declared programs reported success
$out_s1"

# The message has to name the program, or a user cannot act on it.
echo "$out_s1" | grep -q "ghostprog" \
  || fail "S1: failure message does not name the missing program 'ghostprog'
$out_s1"
log "  ✓ exit=$rc_s1 and the message names ghostprog"

# ── S2: promises nothing, registers nothing → must succeed ───────────
log "S2: xlings install quietpkg -y → expect success (control)"

set +e
out_s2="$(RUN install quietpkg -y 2>&1 | tr -d "\0"; exit "${PIPESTATUS[0]}")"
rc_s2=$?
set -e

[[ "$rc_s2" -eq 0 ]] \
  || fail "S2: exit code was $rc_s2, expected 0 — a package that declares no
programs must not be caught by the declared-programs check. Without this
passing, S1 proves nothing about which field drove the failure.
$out_s2"
log "  ✓ exit=0 — the check keys off package.programs, not the fixture shape"

log "PASS: declared programs are verified after install"
