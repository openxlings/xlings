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
# "host vs ours" branch — adding one would give the question two answerers
# again and make us depend on the host having an `ldd` at all.
#
# ASSERTS PROPERTIES, NOT SPELLINGS. Library names, paths and versions differ
# per distro; what must hold is "agrees with the host's own answer" and "no
# library is reported missing that the host can find". Pinning `libc.so.6 =>
# /lib/x86_64-linux-gnu/libc.so.6` would be a test of this machine.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

case "$(uname -s)" in
  Linux) ;;
  *) log "SKIP: ldd/PT_INTERP delegation is Linux-only (this is $(uname -s))"; exit 0 ;;
esac
[ -x /usr/bin/ldd ] || { log "SKIP: no host /usr/bin/ldd to compare against"; exit 0; }

HOME_DIR="$(runtime_home_dir ldd_files_world)"
assert_home_is_isolated "$HOME_DIR"
BIN="$(find_xlings_bin)"
cleanup() { rm -rf "$HOME_DIR"; }
trap cleanup EXIT

# A glibc payload to register `ldd` from. Any installed one will do — the rule
# under test is that the packaged script is NOT what answers for a host binary,
# so which glibc it is cannot matter. That independence is the point.
GLIBC_DIR=""
for cand in "${HOME}/.xlings/data/xpkgs/xim-x-glibc"/*; do
  [ -x "$cand/bin/ldd" ] || continue
  GLIBC_DIR="$cand"
done
if [ -z "$GLIBC_DIR" ]; then
  log "SKIP: no xim-x-glibc payload available to register ldd from"
  exit 0
fi
log "using glibc payload: $GLIBC_DIR"

mkdir -p "$HOME_DIR/bin" "$HOME_DIR/subos/default/bin"
cp "$BIN" "$HOME_DIR/bin/xlings"
ln -sf ../../../bin/xlings "$HOME_DIR/subos/default/bin/ldd"
cat > "$HOME_DIR/.xlings.json" <<EOF
{"activeSubos":"default","mirror":"${XLINGS_TEST_MIRROR:-GLOBAL}",
 "versions":{"ldd":{"filename":"ldd","type":"program",
   "versions":{"pkg":{"path":"$GLIBC_DIR/bin"}}}}}
EOF
printf '{"workspace":{"ldd":"pkg"}}\n' > "$HOME_DIR/subos/default/.xlings.json"

SHIM="$HOME_DIR/subos/default/bin/ldd"
run_shim() { env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" "$SHIM" "$@"; }

# Drop the load addresses; they differ per run (ASLR).
norm() { sed 's/ (0x[0-9a-f]*)//' | sort; }

checked=0
for target in /usr/bin/env /usr/bin/ls /bin/sh; do
  [ -f "$target" ] || continue
  # Only dynamic executables are in scope; a static one has no PT_INTERP and
  # is handled by the packaged script, which is covered separately below.
  /usr/bin/ldd "$target" >/dev/null 2>&1 || continue
  /usr/bin/ldd "$target" 2>&1 | grep -q "not a dynamic executable" && continue

  host_out="$(/usr/bin/ldd "$target" 2>&1 | norm)"
  shim_out="$(run_shim "$target" 2>&1 | norm)" || fail "ldd $target exited non-zero"

  if echo "$shim_out" | grep -q "not found"; then
    fail "ldd $target reported a host library as 'not found' — the subos loader answered for a host file:
$shim_out"
  fi
  if [ "$host_out" != "$shim_out" ]; then
    fail "ldd $target disagrees with the host's own ldd:
--- host ---
$host_out
--- shim ---
$shim_out"
  fi
  checked=$((checked + 1))
  log "  ok: $target agrees with /usr/bin/ldd"
done
# Not-measured is never agreement.
[ "$checked" -gt 0 ] || fail "no dynamic host executable was checked; the test proved nothing"

# A store binary must still resolve inside the store — the rule is "the file's
# own loader", and for our own payloads that IS the subos glibc. Without this
# row, "delegate everything to the host" would pass the rows above.
STORE_BIN=""
for cand in "${HOME}/.xlings/data/xpkgs"/*/*/bin/*; do
  [ -f "$cand" ] && [ -x "$cand" ] || continue
  head -c 4 "$cand" 2>/dev/null | grep -q ELF || continue
  if /usr/bin/ldd "$cand" 2>/dev/null | grep -q "$HOME/.xlings/data/xpkgs"; then
    STORE_BIN="$cand"; break
  fi
done
if [ -n "$STORE_BIN" ]; then
  out="$(run_shim "$STORE_BIN" 2>&1 || true)"
  echo "$out" | grep -q "data/xpkgs" \
    || fail "ldd on a store binary stopped resolving inside the store:
$out"
  log "  ok: store binary still resolves inside the store"
else
  log "  note: no store binary with in-store deps found; that row is skipped"
fi

# The inputs where the FILE has no answer must reach the packaged script
# unchanged. These are the cases it already handles correctly.
out="$(run_shim --version 2>&1 || true)"
assert_contains "$out" "ldd" "ldd --version must still be answered by the packaged ldd"
log "  ok: --version unchanged"

out="$(run_shim 2>&1 || true)"
assert_contains "$out" "missing file arguments" "bare ldd must still print its own usage error"
log "  ok: no-argument case unchanged"

out="$(run_shim /nonexistent/file 2>&1 || true)"
assert_contains "$out" "No such file" "a missing file must still be reported by the packaged ldd"
log "  ok: missing-file case unchanged"

log "PASS: ldd answers in the file's world"
