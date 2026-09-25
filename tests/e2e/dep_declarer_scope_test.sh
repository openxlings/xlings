#!/usr/bin/env bash
# E2E: a recipe's bare dependency means the package in ITS OWN index; an index
# entry that configures a declared sub-index is that sub-index, not a peer.
#
# WHAT THIS DEFENDS (2026.9.20.1, reproduced before the fix)
#
#   $ xlings install xmake
#   [error] package 'ncurses' is ambiguous, candidates:      <- printed twice
#           1. scode:ncurses@6.4 from global repo 'scode'
#           2. xim:ncurses@6.5 from global repo 'xim'
#
# xmake.lua (in `xim`) declared `deps = { "ncurses", ... }`. The bare name was
# resolved against every index the USER had configured, and this user listed
# `scode` in index_repos -- the only way to pin it -- which also promoted it to
# a peer of `xim`. So an official recipe's meaning depended on a user's config,
# and the "use one of: xlings install ..." advice ran and fixed nothing.
#
# Fixture: four local indexes, planned with `plan_install` (dry run, no
# downloads, no network).
#   xim    app -> libdep (bare), app9 -> libdep@9, appmiss -> onlypeers,
#          libdep@1.0.0, shared@1.0.0
#   peer1  libdep@2.0.0, onlypeers@1.0.0, zlibx@1.0.0       (independent)
#   peer2  onlypeers@1.0.0, zlibx@1.0.0                      (independent)
#   decl   shared@3.0.0                 (declared by xim's xim-indexrepos.lua,
#                                        AND listed in index_repos, same url)
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

HOME_DIR="$(runtime_home_dir dep_declarer_scope_home)"
FIX="$(runtime_home_dir dep_declarer_scope_repos)"
assert_home_is_isolated "$HOME_DIR"
cleanup() { rm -rf "$HOME_DIR" "$FIX"; }
trap cleanup EXIT
cleanup
mkdir -p "$HOME_DIR" "$FIX"

recipe() {  # recipe <repo-dir> <name> <version> [deps-lua]
  local dir="$1/pkgs/${2:0:1}" deps="${4:-}"
  mkdir -p "$dir"
  local depline=""
  [[ -n "$deps" ]] && depline="deps = { $deps },"
  local plat
  local body=""
  for plat in linux macosx windows; do
    body+="        $plat = { $depline [\"latest\"] = { ref = \"$3\" }, [\"$3\"] = { url = \"https://example.invalid/$2-$3.tar.gz\" } },
"
  done
  cat > "$dir/$2.lua" <<LUA
package = {
    spec = "1",
    name = "$2",
    description = "fixture",
    licenses = {"MIT"},
    type = "package",
    xpm = {
$body    },
}
LUA
}

recipe "$FIX/xim"   app     1.0.0 '"libdep"'
recipe "$FIX/xim"   app9    1.0.0 '"libdep@9"'
recipe "$FIX/xim"   appmiss 1.0.0 '"onlypeers"'
recipe "$FIX/xim"   libdep  1.0.0
recipe "$FIX/xim"   shared  1.0.0
recipe "$FIX/peer1" libdep  2.0.0
recipe "$FIX/peer1" onlypeers 1.0.0
recipe "$FIX/peer1" zlibx   1.0.0
recipe "$FIX/peer2" onlypeers 1.0.0
recipe "$FIX/peer2" zlibx   1.0.0
recipe "$FIX/decl"  shared  3.0.0
(cd "$FIX/decl" && git init -q && git add -A \
   && git -c user.email=t@t -c user.name=t commit -q -m init)

# One key per line: the declaration parser is line-based, like the real file.
cat > "$FIX/xim/xim-indexrepos.lua" <<LUA
xim_indexrepos = {
    ["decl"] = {
        ["GLOBAL"] = "file://$FIX/decl",
        ["CN"] = "file://$FIX/decl",
    },
}
LUA

cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim",   "url": "$FIX/xim" },
    { "name": "peer1", "url": "$FIX/peer1" },
    { "name": "peer2", "url": "$FIX/peer2" }
  ]
}
EOF

X() { run_xlings "$HOME_DIR" "$ROOT_DIR" "$@"; }
plan() {  # plan <json-args> -> prints NDJSON; never fails the script
  X interface plan_install --args "$1" 2>/dev/null || true
}

X update >/dev/null 2>&1 || true
[[ -d "$HOME_DIR/data/xim-index-repos" ]] || fail "setup: sub-index tree was never synced"

# ── AC1: the declaring index answers a bare dependency ─────────────────
# xim and peer1 both offer `libdep` at the same rank; app is xim's.
OUT="$(plan '{"targets":["app"]}')"
case "$OUT" in *'"xim:libdep@1.0.0"'*) ;; *) fail "AC1: app did not get xim:libdep:
$OUT" ;; esac
case "$OUT" in *peer1:libdep*) fail "AC1: app's bare libdep went to peer1:
$OUT" ;; esac
log "  ok AC1: xim:app's bare 'libdep' is xim:libdep, though peer1 has one too"

# ── AC2: what the USER types is not widened ────────────────────────────
OUT="$(plan '{"targets":["zlibx"]}')"
case "$OUT" in *"is ambiguous"*) ;; *) fail "AC2: a bare top-level name two peers offer stopped being ambiguous:
$OUT" ;; esac
log "  ok AC2: a bare name the user types, offered by two peers, is still ambiguous"

# ── AC2b: a version the declaring index lacks is an error, not a switch ─
OUT="$(plan '{"targets":["app9"]}')"
case "$OUT" in *"the index that declares it"*"matching '9'"*) ;; *) fail "AC2b: expected an error naming the declaring index:
$OUT" ;; esac
case "$OUT" in *'"peer1:libdep'*) fail "AC2b: switched to peer1 for a version xim lacks:
$OUT" ;; esac
log "  ok AC2b: libdep@9 from xim fails naming xim, never taking peer1's libdep"

# ── AC4: a dependency failure is reported once, with its chain ─────────
OUT="$(plan '{"targets":["appmiss"]}')"
COUNT="$(printf '%s\n' "$OUT" | grep -c "dependency 'onlypeers' is ambiguous" || true)"
[[ "$COUNT" == "1" ]] || fail "AC4: expected the dependency failure once, got $COUNT:
$OUT"
case "$OUT" in *"xim:appmiss@1.0.0 -> onlypeers"*) ;; *) fail "AC4: no dependency chain in:
$OUT" ;; esac
case "$OUT" in *"xlings install"*) fail "AC4: dependency ambiguity still advises an install that cannot help:
$OUT" ;; esac
case "$OUT" in *"--rm-index-repo peer2"*) ;; *) fail "AC4: the remedy for a user-added repo is missing:
$OUT" ;; esac
log "  ok AC4: one report, with 'xim:appmiss@1.0.0 -> onlypeers', no install advice"

# ── AC5: the printed remedy, executed, gets the user where they wanted ─
X config --rm-index-repo peer2 >/dev/null 2>&1 || fail "AC5: config --rm-index-repo peer2 failed"
OUT="$(plan '{"targets":["appmiss"]}')"
case "$OUT" in *'"peer1:onlypeers@1.0.0"'*) ;; *) fail "AC5: after the printed remedy appmiss still does not plan:
$OUT" ;; esac
log "  ok AC5: running the printed --rm-index-repo remedy makes appmiss plan"

# ── AC1b / AC6: an entry configuring a declared sub-index is that sub-index ─
OUT="$(X config --index-repo "decl:file://$FIX/decl" 2>&1 || true)"
case "$OUT" in *"stays a sub-index"*) ;; *) fail "AC6: adding the declared url did not say it stays a sub-index:
$OUT" ;; esac
set +e
OUT="$(X config --index-repo "decl:file://$FIX/peer1" 2>&1)"; RC=$?
set -e
[[ $RC -ne 0 ]] || fail "AC6: a second url under a declared sub-index's name was accepted"
case "$OUT" in *"cannot share one namespace"*) ;; *) fail "AC6: collision refusal did not say why:
$OUT" ;; esac
X update >/dev/null 2>&1 || true
[[ ! -e "$HOME_DIR/data/decl" ]] || fail "AC1b: the declared sub-index was synced a second time into data/decl"
OUT="$(plan '{"targets":["shared"]}')"
case "$OUT" in *'"xim:shared@1.0.0"'*) ;; *) fail "AC1b: listing decl promoted it to a peer of xim:
$OUT" ;; esac
log "  ok AC1b/AC6: decl stays a sub-index (one copy, xim wins 'shared'); a second url is refused"

# doctor: a second copy an older client left behind is reclaimable.
mkdir -p "$HOME_DIR/data/decl/pkgs" && cp -r "$FIX/decl/pkgs/." "$HOME_DIR/data/decl/pkgs/"
OUT="$(X self doctor 2>&1 || true)"
case "$OUT" in *"second copy of index 'decl'"*) ;; *) fail "AC6: doctor did not report the duplicate index copy:
$OUT" ;; esac
X self doctor --fix >/dev/null 2>&1 || true
[[ ! -e "$HOME_DIR/data/decl" ]] || fail "AC6: doctor --fix left the duplicate index copy"
log "  ok AC6: doctor reports data/decl as a second copy, --fix deletes it"

# ── AC7: noDeps is refused out loud ────────────────────────────────────
set +e
OUT="$(X interface plan_install --args '{"targets":["app"],"noDeps":true}' 2>/dev/null)"; RC=$?
set -e
[[ $RC -eq 2 ]] || fail "AC7: noDeps:true exit $RC, want 2:
$OUT"
case "$OUT" in *"noDeps is not supported"*) ;; *) fail "AC7: noDeps refusal did not say so:
$OUT" ;; esac
log "  ok AC7: noDeps:true is refused (exit 2) instead of silently ignored"

log "PASS: dependency names resolve in their declaring index; one identity per index"
