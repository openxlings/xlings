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
# A REAL ELF, built here rather than compiled.
#
# This fixture used to be `\177ELF` followed by the word "fixture", plus a stub
# `patchelf` on PATH that answered both queries -- which worked because the
# audit shelled out to whatever `patchelf` it found. It no longer does: the two
# fields are read in-process from the program header table (xlings.core.elfread),
# so a file that is not really an ELF now has nothing to read and the finding
# correctly disappears.
#
# That is the same property this change was FOR. A reader that could be
# satisfied by a script on PATH was a reader whose answer depended on the
# machine, and a missing patchelf made a clean scan and an unperformed scan
# print the same thing.
#
# So the fixture becomes real: ELF64, PT_INTERP naming the HOST loader,
# DT_RUNPATH naming the PAYLOAD's core-runtime directory. Written byte by byte
# rather than compiled, which keeps the original property the stub was for --
# independence from the host compiler and libc.
python3 - "$PAYLOAD/bin/host-linked-app" "$LIBDIR" <<'PY'
import struct, sys

out_path, runpath = sys.argv[1], sys.argv[2]
interp = b"/lib64/ld-linux-x86-64.so.2\0"
LOAD_VADDR, PHENTSIZE, PHNUM = 0x400000, 56, 3
body_start = 64 + PHENTSIZE * PHNUM

body = bytearray()
interp_off = len(body); body += interp

strtab_off = len(body)
body += b"\0"                      # .dynstr index 0 is the empty string
runpath_idx = len(body) - strtab_off
body += runpath.encode() + b"\0"

while len(body) % 8: body += b"\0"
dyn_off = len(body)
for tag, val in ((5, LOAD_VADDR + body_start + strtab_off),  # DT_STRTAB (vaddr)
                 (29, runpath_idx),                          # DT_RUNPATH
                 (0, 0)):                                    # DT_NULL
    body += struct.pack("<QQ", tag, val)
dyn_size = len(body) - dyn_off
total = body_start + len(body)

eh = bytearray(b"\x7fELF\x02\x01\x01\x00" + b"\0" * 8)
eh += struct.pack("<HHIQQQIHHHHHH",
                  2, 0x3e, 1, LOAD_VADDR, 64, 0, 0, 64, PHENTSIZE, PHNUM, 64, 0, 0)

def phdr(t, off, vaddr, size):
    return struct.pack("<IIQQQQQQ", t, 4, off, vaddr, vaddr, size, size, 0x1000)

ph = (phdr(1, 0, LOAD_VADDR, total)                                        # PT_LOAD
      + phdr(3, body_start + interp_off,
             LOAD_VADDR + body_start + interp_off, len(interp))            # PT_INTERP
      + phdr(2, body_start + dyn_off,
             LOAD_VADDR + body_start + dyn_off, dyn_size))                 # PT_DYNAMIC

with open(out_path, "wb") as f:
    f.write(bytes(eh) + ph + bytes(body))
PY
printf 'fixture libc\n' > "$LIBDIR/libc.so.6"

run_doctor() {
  ( cd /tmp && env -i HOME="$HOME_DIR" PATH="$TOOLS_DIR:/usr/bin:/bin" \
      XLINGS_HOME="$HOME_DIR" \
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
