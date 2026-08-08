#!/usr/bin/env bash
# E2E: does a subos.env declaration follow `xlings use <pkg> <ver>`?
#
# This closes the open question in
# .agents/docs/2026-08-08-declared-vs-effective-open-defects-design.md §6.
#
# `xvm.files` nodes were PROVEN to follow a same-namespace `use` switch
# (§2.6, with ca-certificates: the file changed and so did its sha). env
# declarations were left explicitly unverified, and the doc says why:
#
#   files 节点在 xvm DB 里,而 env 声明在 subos_info 里、按 binding 键值组织
#   —— 是两个不同的存储。在验证之前,不要假设它跟 files 一样是对的。
#
# Two stores, two mechanisms. One being right says nothing about the other.
# The earlier attempt to measure this was polluted by the `--add-xpkg` stub
# (D1), which is fixed, so it can now be measured cleanly.
#
# The invariant asserted: with two versions of one package installed, the
# variable injected into the subos is the ACTIVE version's. Not "some
# version's", and not both.

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_env_use_switch"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

BIN="$(find_xlings_bin)"
log "client: $("$BIN" --version 2>&1 | head -1)"

# ── one package, two versions, same variable ────────────────────────────
#
# `${pkgdir}` expands per version, so the two versions cannot produce the
# same value by accident -- the assertion below would be unfalsifiable if
# they could.
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/e"

cat > "$LOCAL_INDEX_DIR/pkgs/e/envswitch.lua" <<'LUA'
package = {
    spec = "1",
    name = "envswitch",
    description = "Local fixture for tests/e2e/subos_env_use_switch_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
import("xim.libxpkg.subos")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    os.mkdir(path.join(dir, "marker"))
    io.writefile(path.join(dir, "bin", "envswitch"),
                 "#!/bin/sh\necho envswitch " .. pkginfo.version() .. "\n")
    os.exec("chmod +x " .. path.join(dir, "bin", "envswitch"))
    return true
end

function config()
    xvm.add(package.name, {
        version = pkginfo.version(),
        bindir  = path.join(pkginfo.install_dir(), "bin"),
    })

    if type(subos.env) == "function" then
        subos.env{ var = "E2E_SWITCH_PATH", op = "set",
                   value = "${pkgdir}/marker",
                   binding = package.name .. "@" .. pkginfo.version() }
    end
    return true
end

function uninstall()
    return true
end
LUA

# ── an isolated home ────────────────────────────────────────────────────
mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/data/xim-index-repos"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
cp "$BIN" "$HOME_DIR/xlings"

x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$BIN" "$@" ) }

x self init >/dev/null 2>&1 || true

injected() {
  ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$HOME_DIR" \
    "$BIN" subos use default --cmd 'echo "P=${E2E_SWITCH_PATH:-<unset>}"' \
    2>/dev/null | sed -n 's/^P=//p' | head -1 )
}

# ── install both versions ───────────────────────────────────────────────
x install envswitch@1.0.0 -y >/dev/null 2>&1 \
  || fail "install envswitch@1.0.0 failed"
x install envswitch@2.0.0 -y >/dev/null 2>&1 \
  || fail "install envswitch@2.0.0 failed"
log "  ✓ both versions installed"

# Guard: if only one version ever registered, everything below passes
# vacuously. This is the shape that has made isolated-home tests lie before.
# The per-SUBOS manifest, not the home one. `subos_info` is subos-scoped
# state; reading $HOME_DIR/.xlings.json finds no `subos_info` key at all and
# reports "0 sections", which looks exactly like the declaration having been
# dropped.
MANIFEST="$HOME_DIR/subos/default/.xlings.json"
SECTIONS="$(python3 -c '
import json,sys
envs = json.load(open(sys.argv[1]))["subos_info"]["envs"]
print(len([k for k in envs if k.startswith("envswitch@")]))' "$MANIFEST" 2>/dev/null || echo 0)"
[ "$SECTIONS" = "2" ] \
  || fail "expected 2 envswitch env sections (one per version), got $SECTIONS"
log "  ✓ both versions declared their own env section"

# ── the active version's value is the one injected ──────────────────────
x use envswitch 1.0.0 >/dev/null 2>&1 || fail "use envswitch 1.0.0 failed"
P1="$(injected)"
case "$P1" in
  */1.0.0/marker) log "  ✓ after 'use 1.0.0' the injected value is 1.0.0's" ;;
  */2.0.0/marker) fail "after 'use 1.0.0' the env still points at 2.0.0: $P1" ;;
  *)              fail "after 'use 1.0.0' the env is unusable: $P1" ;;
esac

x use envswitch 2.0.0 >/dev/null 2>&1 || fail "use envswitch 2.0.0 failed"
P2="$(injected)"
case "$P2" in
  */2.0.0/marker) log "  ✓ after 'use 2.0.0' the injected value followed" ;;
  */1.0.0/marker) fail "the env declaration did NOT follow 'use': still $P2" ;;
  *)              fail "after 'use 2.0.0' the env is unusable: $P2" ;;
esac

[ "$P1" != "$P2" ] \
  || fail "both switches produced the same value ($P1) -- the test proves nothing"

log "E2E subos.env follows 'use': PASS"
