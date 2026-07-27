#!/usr/bin/env bash
# E2E-37: the `if xvm.files then` probe, run against a REAL old xlings.
#
# This is the single point the whole files-migration plan rests on. A recipe
# that wants to declare `type = "files"` cannot do it unconditionally -- an
# older client hard-fails on an unknown registration node kind and installs
# nothing. The plan is for recipes to probe instead:
#
#     if xvm.files then xvm.files{...} else <legacy> end
#
# The claim is that `xvm.files` is nil on every client that cannot handle the
# node kind, because it is a Lua function libxpkg 0.0.47 introduced and
# libxpkg is statically linked into xlings.
#
# Reading the code says so. That is not enough: `import()` returns a
# permissive proxy stub for unknown modules, and if `xvm` ever came from that
# path the probe would be truthy everywhere and silently useless. So this test
# downloads a real released 0.4.69 and runs the same recipe through both.
#
#   old client (0.4.69) → legacy branch taken, install succeeds
#   new client          → files branch taken, asset registered and switchable
#
# XLINGS_OLD_BIN can point at an already-downloaded old binary; otherwise the
# test fetches one, and skips (exit 0) if there is no network.

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

OLD_VERSION="${XLINGS_OLD_VERSION:-0.4.69}"
RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/xvm_files_probe"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

NEW_BIN="$(find_xlings_bin)"

# ── obtain an old client ────────────────────────────────────────────────
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

# ── one recipe, written the way the migration guide says to write it ────
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/p"

cat > "$LOCAL_INDEX_DIR/pkgs/p/probefixture.lua" <<'LUA'
package = {
    spec = "1",
    name = "probefixture",
    description = "Local fixture for tests/e2e/xvm_files_probe_compat_test.sh",
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
import("xim.libxpkg.system")
import("xim.libxpkg.xvm")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    os.mkdir(path.join(dir, "include"))
    io.writefile(path.join(dir, "bin", "probefixture"), "#!/bin/sh\necho probe\n")
    os.exec("chmod +x " .. path.join(dir, "bin", "probefixture"))
    io.writefile(path.join(dir, "include", "probe.h"), "#define PROBE 1\n")
    return true
end

function config()
    xvm.add(package.name, { bindir = path.join(pkginfo.install_dir(), "bin") })

    -- The contract under test. Probe the capability, never the version.
    if xvm.files then
        io.writefile(path.join(pkginfo.install_dir(), "BRANCH"), "files")
        xvm.files{
            src = "include/probe.h",
            dst = "usr/include/probe.h",
            binding = package.name .. "@" .. pkginfo.version(),
        }
    else
        io.writefile(path.join(pkginfo.install_dir(), "BRANCH"), "legacy")
        local sys_inc = path.join(system.subos_sysrootdir(), "usr/include")
        if not os.isdir(sys_inc) then os.mkdir(sys_inc) end
        os.cp(path.join(pkginfo.install_dir(), "include", "probe.h"), sys_inc)
    end
    return true
end

function uninstall()
    if not xvm.files then
        os.tryrm(path.join(system.subos_sysrootdir(), "usr/include", "probe.h"))
    end
    return true
end
LUA

# ── run the same recipe through both clients, in separate homes ─────────
run_with() {   # <binary> <home>
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$2" "$1" "${@:3}" )
}

prepare_home() {  # <home>
  mkdir -p "$1/subos/default/bin"
  cat > "$1/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
  mkdir -p "$1/data/xim-index-repos"
  printf '{}\n' > "$1/data/xim-index-repos/xim-indexrepos.json"
}

branch_taken() {  # <home>
  local f
  f="$(find "$1" -name BRANCH -type f 2>/dev/null | head -1)"
  [[ -n "$f" ]] && cat "$f" || echo "<none>"
}

for pair in "old:$OLD_BIN:legacy" "new:$NEW_BIN:files"; do
  IFS=: read -r label bin want <<<"$pair"
  HOME_DIR="$RUNTIME_DIR/home-$label"
  mkdir -p "$HOME_DIR"
  cp "$bin" "$HOME_DIR/xlings"
  prepare_home "$HOME_DIR"
  run_with "$bin" "$HOME_DIR" self init >/dev/null 2>&1 || true

  log "scenario $label: installing through $(basename "$(dirname "$bin")")"
  OUT="$(run_with "$bin" "$HOME_DIR" install probefixture@1.0.0 -y 2>&1)"

  # An old client hitting the files branch fails exactly like this.
  if grep -qi "unsupported registration node kind" <<<"$OUT"; then
    echo "$OUT" >&2
    fail "$label client hit the files branch — the probe does not gate"
  fi

  got="$(branch_taken "$HOME_DIR")"
  [[ "$got" == "$want" ]] \
    || { echo "$OUT" >&2; fail "$label client took the '$got' branch, expected '$want'"; }

  # Either way the header has to end up in the sysroot: the whole point of
  # the legacy branch is that the old behaviour is preserved exactly.
  [[ -e "$HOME_DIR/subos/default/usr/include/probe.h" ]] \
    || { echo "$OUT" >&2; fail "$label client did not place the header"; }

  log "  ✓ $label → '$got' branch, header present"
done

# ── the new client's placement must additionally be tracked ─────────────
NEW_HOME="$RUNTIME_DIR/home-new"
python3 - "$NEW_HOME/.xlings.json" <<'PY' || exit 1
import json, sys
state = json.load(open(sys.argv[1]))
versions = state.get("versions") or state.get("data") or {}
files = [t for t, info in versions.items()
         if any((v.get("kind") == "files")
                for v in info.get("versions", {}).values())]
if not files:
    print("no files-kind entry was registered", file=sys.stderr)
    raise SystemExit(1)
print("[project-e2e]   ✓ registered as a tracked asset:", ", ".join(files))
PY

log "E2E-37 xvm.files probe compatibility: PASS"
