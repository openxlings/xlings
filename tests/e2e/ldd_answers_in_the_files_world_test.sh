#!/usr/bin/env bash
# E2E: `ldd` answers in its ARGUMENT's world, not in the subos's.
#
# WHAT THIS DEFENDS (openxlings/xlings#608, #522)
#
# `ldd` is the one registered program whose answer belongs to the file it is
# given rather than to the subos it was dispatched from. Every ldd — ours and
# the host's — picks a loader from its own RTLDLIST and runs the target through
# it. For a file built against a different glibc that is the wrong loader, and
# the result is not an error, it is a WRONG ANSWER:
#
#   * on a host whose binaries use DT_RELR, the subos loader rejects all of
#     them with "DT_RELR without GLIBC_ABI_DT_RELR dependency" (#608);
#   * on a host whose binaries do not, it reports every host library as
#     `not found` AND EXITS 0 — so `set -e` and `pipefail` both miss it, and a
#     script that bundles ldd's output ships an empty bundle.
#
# The fix deletes the substitution: PT_INTERP, frozen into the file at build
# time, is the single answerer for "which loader". There is deliberately no
# "host vs ours" branch.
#
# HERMETIC ON PURPOSE. An earlier cut of this test registered `ldd` from a real
# `xim-x-glibc` payload and skipped when none was installed — which is every CI
# runner, so it would have been green having run nothing. Here the packaged
# `ldd` is a stand-in that announces itself, which makes the question sharper
# than a real one could: for a file that carries its own loader the marker must
# be ABSENT (the packaged script did not answer), and for the inputs where the
# file has no answer it must be PRESENT (the packaged script did answer).
#
# ASSERTS PROPERTIES, NOT SPELLINGS. Library names and paths differ per distro;
# what must hold is "agrees with the host's own answer" and "nothing the host
# can find is reported missing".
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

case "$(uname -s)" in
  Linux) ;;
  *) log "SKIP: ELF/PT_INTERP delegation is Linux-only (this is $(uname -s))"; exit 0 ;;
esac
[ -x /usr/bin/ldd ] || { log "SKIP: no host /usr/bin/ldd to compare against"; exit 0; }

HOME_DIR="$(runtime_home_dir ldd_files_world)"
assert_home_is_isolated "$HOME_DIR"
BIN="$(find_xlings_bin)"
cleanup() { rm -rf "$HOME_DIR"; }
trap cleanup EXIT
cleanup

MARKER="PACKAGED-LDD-ANSWERED"
PKG_DIR="$HOME_DIR/data/xpkgs/xim-x-fakeglibc/1.0/bin"
mkdir -p "$HOME_DIR/bin" "$HOME_DIR/subos/default/bin" "$PKG_DIR"
cp "$BIN" "$HOME_DIR/bin/xlings"

# The stand-in for glibc's `bin/ldd`: same shape (an RTLDLIST it trusts), and
# it says so whenever it runs.
cat > "$PKG_DIR/ldd" <<EOS
#!/bin/bash
echo "$MARKER"
case "\$1" in
  --version) echo "ldd (fake) 1.0"; exit 0 ;;
  "")        echo "ldd: missing file arguments"; exit 1 ;;
esac
if [ ! -e "\$1" ]; then echo "ldd: \$1: No such file or directory"; exit 1; fi
echo "	not a dynamic executable"
exit 0
EOS
chmod +x "$PKG_DIR/ldd"
ln -sf ../../../bin/xlings "$HOME_DIR/subos/default/bin/ldd"
cat > "$HOME_DIR/.xlings.json" <<EOF
{"activeSubos":"default","mirror":"${XLINGS_TEST_MIRROR:-GLOBAL}",
 "versions":{"ldd":{"filename":"ldd","type":"program",
   "versions":{"1.0":{"path":"$PKG_DIR"}}}}}
EOF
printf '{"workspace":{"ldd":"1.0"}}\n' > "$HOME_DIR/subos/default/.xlings.json"

SHIM="$HOME_DIR/subos/default/bin/ldd"
run_shim() { env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" "$SHIM" "$@"; }
norm() { sed 's/ (0x[0-9a-f]*)//' | sort; }   # load addresses differ per run

# ── 1. a file that carries its own loader is answered by that loader ──
checked=0
for target in /usr/bin/env /usr/bin/ls /bin/sh; do
  [ -f "$target" ] || continue
  host_out="$(/usr/bin/ldd "$target" 2>&1)" || continue
  case "$host_out" in *"not a dynamic executable"*) continue ;; esac

  shim_out="$(run_shim "$target" 2>&1)" || fail "ldd $target exited non-zero"

  case "$shim_out" in
    *"$MARKER"*) fail "the packaged ldd answered for $target — the subos's loader
answered for a file that names its own:
$shim_out" ;;
  esac
  case "$shim_out" in
    *"not found"*) fail "ldd $target reported a host library as 'not found':
$shim_out" ;;
  esac
  [ "$(printf '%s\n' "$shim_out" | norm)" = "$(printf '%s\n' "$host_out" | norm)" ] \
    || fail "ldd $target disagrees with the host's own ldd:
--- host ---
$host_out
--- shim ---
$shim_out"
  checked=$((checked + 1))
  log "  ok: $target answered by its own PT_INTERP, agrees with /usr/bin/ldd"
done
# Not-measured is never agreement.
[ "$checked" -gt 0 ] || fail "no dynamic host executable was checked; the test proved nothing"

# ── 1b. ldd's own flags are loader environment, and must still work ──
#
# A first cut passed the whole command line to the loader, so `ldd -r f` became
# `ld.so -r f` and the loader read `-r` as the program to trace:
# "-r: cannot open shared object file". Same tool, two answers depending on a
# flag — the exact shape this change exists to remove, reintroduced by its own
# fix. The flags are translated to the environment variables glibc's own ldd
# sets, and only the file reaches the loader.
for flag in -d -r -u -v; do
  host_out="$(/usr/bin/ldd $flag /usr/bin/env 2>&1 | norm)"
  shim_out="$(run_shim $flag /usr/bin/env 2>&1 | norm)"
  [ "$shim_out" = "$host_out" ] || fail "ldd $flag disagrees with the host's own ldd:
--- host ---
$host_out
--- shim ---
$shim_out"
  log "  ok: ldd $flag agrees with /usr/bin/ldd"
done

# An option we do NOT understand must reach the packaged ldd, which owns its
# own usage error. Guessing at it would be the worse failure.
out="$(run_shim --bogus-option /usr/bin/env 2>&1 || true)"
case "$out" in
  *"$MARKER"*) log "  ok: an unknown option falls through to the packaged ldd" ;;
  *) fail "an unknown option did not reach the packaged ldd: $out" ;;
esac

# ── 2. where the file has NO answer, the packaged ldd still gets it ───
# Without these rows, "delegate everything, always" would pass row 1 — and
# would break `--version`, the usage error and the static case.
assert_marker() {
  local why="$1"; shift
  local out; out="$(run_shim "$@" 2>&1 || true)"
  case "$out" in
    *"$MARKER"*) log "  ok: $why still reaches the packaged ldd" ;;
    *) fail "$why did not reach the packaged ldd: $out" ;;
  esac
}
assert_marker "--version"            --version
assert_marker "no arguments"
assert_marker "a missing file"       /nonexistent/file
assert_marker "a non-ELF file"       "$HOME_DIR/.xlings.json"

STATIC_PROBE="$HOME_DIR/static-probe"
printf '#!/bin/sh\ntrue\n' > "$STATIC_PROBE"; chmod +x "$STATIC_PROBE"
assert_marker "a script (no PT_INTERP)" "$STATIC_PROBE"

# ── 3. an xlings-owned loader is honoured too, when one is installed ──
# The rule is "the file's own loader", not "always the host's". Only a real
# payload can demonstrate the difference, so this row is conditional — the
# rows above already fail if delegation went to the host unconditionally,
# because the host's ldd would still not print the marker.
GLIBC_DIR=""
for cand in "${HOME}/.xlings/data/xpkgs/xim-x-glibc"/*; do
  [ -d "$cand/lib64" ] && GLIBC_DIR="$cand"
done
STORE_BIN=""
if [ -n "$GLIBC_DIR" ]; then
  for cand in "${HOME}/.xlings/data/xpkgs"/*/*/bin/*; do
    [ -f "$cand" ] && [ -x "$cand" ] || continue
    head -c 4 "$cand" 2>/dev/null | grep -q ELF || continue
    if /usr/bin/ldd "$cand" 2>/dev/null | grep -q "/.xlings/data/xpkgs"; then
      STORE_BIN="$cand"; break
    fi
  done
fi
if [ -n "$STORE_BIN" ]; then
  out="$(run_shim "$STORE_BIN" 2>&1 || true)"
  case "$out" in *"$MARKER"*) fail "the packaged ldd answered for a store binary: $out" ;; esac
  echo "$out" | grep -q "data/xpkgs" \
    || fail "a store binary stopped resolving inside the store: $out"
  log "  ok: a store binary resolves through ITS loader, inside the store"
else
  log "  note: no store binary with in-store deps on this machine; row skipped"
fi

log "PASS: ldd answers in the file's world"
