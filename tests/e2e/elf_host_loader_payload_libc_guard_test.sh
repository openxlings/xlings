#!/usr/bin/env bash
# E2E: an ELF using the host loader may not search a payload core runtime.
# Ordinary payload leaf libraries remain legal; the reverse guard is about
# ld.so/libc, not about every library distributed through xlings.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

RUNTIME_DIR="$(runtime_home_dir elf_host_loader_payload_libc_guard)"
HOME_DIR="$RUNTIME_DIR/home"
INDEX_DIR="$RUNTIME_DIR/empty-index"
TOOLS_DIR="$RUNTIME_DIR/tools"
PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-hostcore/1.0.0"
LIBDIR="$PAYLOAD/lib64"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$HOME_DIR/subos/default/bin" "$INDEX_DIR/pkgs" \
         "$TOOLS_DIR" "$PAYLOAD/bin" "$LIBDIR"

XLINGS_BIN="$(find_xlings_bin)"
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"

printf 'xim_indexrepos = {}\n' > "$INDEX_DIR/xim-indexrepos.lua"
cat > "$HOME_DIR/.xlings.json" <<JSON
{
  "activeSubos": "default",
  "mirror": "GLOBAL",
  "index_repos": [{"name": "xim", "url": "$INDEX_DIR"}],
  "versions": {
    "hostcore": {
      "type": "group",
      "versions": {
        "1.0.0": {"kind": "group", "path": "$PAYLOAD"}
      }
    }
  }
}
JSON

cat > "$HOME_DIR/subos/default/.xlings.json" <<JSON
{
  "subos_info": {
    "schema_version": 1,
    "runtime": "hostcore@1.0.0",
    "envs": {},
    "created_at": "2026-08-09T00:00:00Z",
    "created_by": "elf-host-loader-e2e"
  },
  "workspace": {
    "hostcore": {"active": "1.0.0", "installed": ["1.0.0"]}
  }
}
JSON

# scan_payload only needs ELF magic before asking patchelf for the two fields.
# The recorder makes the fixture independent of the host compiler and libc.
printf '\177ELFfixture\n' > "$PAYLOAD/bin/host-linked-app"
printf 'fixture libc\n' > "$LIBDIR/libc.so.6"

cat > "$TOOLS_DIR/patchelf" <<'SH'
#!/bin/sh
case "$1" in
  --print-interpreter) printf '/lib64/ld-linux-x86-64.so.2\n' ;;
  --print-rpath)       printf '%s\n' "$ELF_GUARD_LIBDIR" ;;
  *)                   exit 2 ;;
esac
SH
chmod +x "$TOOLS_DIR/patchelf"

run_doctor() {
  ( cd /tmp && env -i HOME="$HOME_DIR" PATH="$TOOLS_DIR:/usr/bin:/bin" \
      XLINGS_HOME="$HOME_DIR" ELF_GUARD_LIBDIR="$LIBDIR" \
      "$XLINGS_BIN" self doctor --deep )
}

rc=0
OUT="$(run_doctor 2>&1)" || rc=$?
[[ "$rc" -ne 0 ]] || fail "host loader + payload libc passed the deep audit"

CLEAN="$(printf '%s\n' "$OUT" | strip_ansi)"
grep -Fq '/lib64/ld-linux-x86-64.so.2' <<<"$CLEAN" \
  || fail "finding did not name the host interpreter:\n$CLEAN"
grep -Fq "$LIBDIR" <<<"$CLEAN" \
  || fail "finding did not name the payload core-runtime directory:\n$CLEAN"
grep -Eqi 'host loader|host interpreter' <<<"$CLEAN" \
  || fail "finding did not explain the reverse host-loader hazard:\n$CLEAN"

log "PASS: host loader + payload core runtime is rejected and named"
