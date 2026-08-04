#!/usr/bin/env bash
# E2E: the `subos.env` capability probe, run against a REAL old xlings.
#
# `xim.libxpkg.subos` is a NEW MODULE, and that changes the probe rule the V2
# spec gives for a new *function* on an existing module.
#
#   if xvm.files then ... end            -- correct: `xvm` exists on old
#                                           clients, so the field really is nil
#   if subos.env then ... end            -- WRONG for a new module
#   if type(subos.env) == "function"     -- correct for a new module
#
# import() answers an unknown module with a permissive proxy stub: every key
# read off it returns a truthy, callable table. So on a client that predates
# the module, `if subos.env then` is TRUE, the recipe takes the new branch,
# calls it, and the call evaporates. The install succeeds, nothing is
# configured, and nothing says so.
#
# Reading the prelude says all this. That is not enough — the same reasoning
# said `xvm.files` was safe, and it was only safe by accident of `xvm` already
# existing. So this runs one recipe through a real released binary and the
# current build and asserts what each observed:
#
#   old client → truthiness TRUE, type() FALSE   → legacy branch
#   new client → truthiness TRUE, type() TRUE    → subos.env branch
#
# The first line is the finding. If it ever reads FALSE the rule can be
# relaxed; until then it must stay, and this is what proves it.
#
# XLINGS_OLD_BIN can point at an already-downloaded old binary; otherwise the
# test fetches one, and skips (exit 0) if there is no network.

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

OLD_VERSION="${XLINGS_OLD_VERSION:-2026.8.4.2}"
RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_env_probe"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

NEW_BIN="$(find_xlings_bin)"

OLD_BIN="${XLINGS_OLD_BIN:-}"
if [[ -z "$OLD_BIN" ]]; then
  TARBALL="$RUNTIME_DIR/old.tar.gz"
  URL="https://github.com/openxlings/xlings/releases/download/v${OLD_VERSION}/xlings-${OLD_VERSION}-linux-x86_64.tar.gz"
  if ! curl -fsSL --max-time 120 -o "$TARBALL" "$URL"; then
    log "SKIP: cannot fetch xlings $OLD_VERSION (no network?)"
    exit 0
  fi
  tar xzf "$TARBALL" -C "$RUNTIME_DIR"
  OLD_BIN="$(find "$RUNTIME_DIR" -type f -name xlings -perm -u+x | head -1)"
fi
[[ -x "$OLD_BIN" ]] || { log "SKIP: no usable old binary"; exit 0; }
log "old client: $("$OLD_BIN" --version 2>&1 | head -1)"
log "new client: $("$NEW_BIN" --version 2>&1 | head -1)"

# ── one recipe, recording what each client observed ─────────────────────
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/s"

cat > "$LOCAL_INDEX_DIR/pkgs/s/subosprobe.lua" <<'LUA'
package = {
    spec = "1",
    name = "subosprobe",
    description = "Local fixture for tests/e2e/subos_env_probe_compat_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
import("xim.libxpkg.subos")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    os.mkdir(path.join(dir, "drivers"))
    io.writefile(path.join(dir, "bin", "subosprobe"), "#!/bin/sh\necho probe\n")
    os.exec("chmod +x " .. path.join(dir, "bin", "subosprobe"))
    return true
end

function config()
    xvm.add(package.name, { bindir = path.join(pkginfo.install_dir(), "bin") })

    local dir = pkginfo.install_dir()
    -- Both readings, written down. The gap between them IS the finding.
    io.writefile(path.join(dir, "TRUTHY"),
                 tostring(subos.env ~= nil))
    io.writefile(path.join(dir, "TYPED"),
                 tostring(type(subos.env) == "function"))

    if type(subos.env) == "function" then
        io.writefile(path.join(dir, "BRANCH"), "subos.env")
        subos.env{ var = "E2E_PROBE_PATH", op = "set",
                   value = "${pkgdir}/drivers",
                   binding = package.name .. "@" .. pkginfo.version() }
    else
        io.writefile(path.join(dir, "BRANCH"), "legacy")
    end
    return true
end

function uninstall()
    return true
end
LUA

run_with() {   # <binary> <home> <args...>
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$2" "$1" "${@:3}" )
}

prepare_home() {  # <home>
  mkdir -p "$1/subos/default/bin" "$1/data/xim-index-repos"
  cat > "$1/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
  printf '{}\n' > "$1/data/xim-index-repos/xim-indexrepos.json"
}

read_marker() {  # <home> <name>
  local f
  f="$(find "$1" -name "$2" -type f 2>/dev/null | head -1)"
  [[ -n "$f" ]] && cat "$f" || echo "<none>"
}

# old: truthy true, typed false, legacy branch
# new: truthy true, typed true,  subos.env branch
for pair in "old:$OLD_BIN:true:false:legacy" "new:$NEW_BIN:true:true:subos.env"; do
  IFS=: read -r label bin wantTruthy wantTyped wantBranch <<<"$pair"
  HOME_DIR="$RUNTIME_DIR/home-$label"
  mkdir -p "$HOME_DIR"
  cp "$bin" "$HOME_DIR/xlings"
  prepare_home "$HOME_DIR"
  run_with "$bin" "$HOME_DIR" self init >/dev/null 2>&1 || true

  OUT="$(run_with "$bin" "$HOME_DIR" install subosprobe@1.0.0 -y 2>&1)"
  if grep -qiE "unsupported registration node kind|config hook failed" <<<"$OUT"; then
    echo "$OUT" >&2
    fail "$label client failed to install — the probe does not gate cleanly"
  fi

  gotTruthy="$(read_marker "$HOME_DIR" TRUTHY)"
  gotTyped="$(read_marker "$HOME_DIR" TYPED)"
  gotBranch="$(read_marker "$HOME_DIR" BRANCH)"

  [[ "$gotTruthy" == "$wantTruthy" ]] || {
    echo "$OUT" >&2
    fail "$label: 'subos.env ~= nil' was $gotTruthy, expected $wantTruthy"
  }
  [[ "$gotTyped" == "$wantTyped" ]] || {
    echo "$OUT" >&2
    fail "$label: 'type(subos.env)==function' was $gotTyped, expected $wantTyped"
  }
  [[ "$gotBranch" == "$wantBranch" ]] || {
    echo "$OUT" >&2
    fail "$label client took the '$gotBranch' branch, expected '$wantBranch'"
  }
  log "  ✓ $label → truthy=$gotTruthy typed=$gotTyped branch=$gotBranch"
done

# The old client must not have silently produced a manifest section, and the
# new one must have.
OLD_MANIFEST="$RUNTIME_DIR/home-old/subos/default/.xlings.json"
NEW_MANIFEST="$RUNTIME_DIR/home-new/subos/default/.xlings.json"

if [[ -f "$OLD_MANIFEST" ]] && grep -q "E2E_PROBE_PATH" "$OLD_MANIFEST"; then
  fail "the old client recorded an env declaration it cannot apply"
fi
grep -q "E2E_PROBE_PATH" "$NEW_MANIFEST" \
  || fail "the new client did not record the declaration"

log "  ✓ only the client that can apply the declaration recorded one"
log "E2E subos.env probe compatibility: PASS"
