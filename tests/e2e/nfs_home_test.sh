#!/usr/bin/env bash
# A home on NFSv3 (mcpp-community/mcpp#809): a filesystem without
# renameat2(RENAME_NOREPLACE), where every publication into a SubOS used to
# fail with "cannot publish ...: Invalid argument". The packages that failed
# there -- linux-headers (usr/include/asm), glibc (lib/Scrt1.o) -- install,
# their entries are in place, and a removal takes them away again.
#
#   nfs_home_test.sh <release tarball> <an NFSv3 mount point>
#
# xtest: covers=ATOMIC-PUBLICATION requires=linux,network,sudo
set -euo pipefail
tarball="${1:?usage: nfs_home_test.sh <release tarball> <NFSv3 mount>}"
mount="${2:?usage: nfs_home_test.sh <release tarball> <NFSv3 mount>}"
fail() { echo "FAIL: $*"; exit 1; }
log() { echo "[nfs-home] $*"; }

grep -qE " $mount nfs .*vers=3" /proc/mounts || fail "$mount is not an NFSv3 mount: $(grep " $mount " /proc/mounts || true)"
work="$(mktemp -d -p "$mount" nfs-home.XXXX)"
trap 'rm -rf "$work"' EXIT

# The lane proves what it claims: on this filesystem the rename flag is refused.
python3 - "$work" <<'PY' || fail "the rename flag works here: this lane would not exercise the fallback"
import ctypes, errno, os, sys
libc = ctypes.CDLL(None, use_errno=True)
d = sys.argv[1]
open(os.path.join(d, "a"), "w").close()
rc = libc.syscall(316, -100, os.path.join(d, "a").encode(), -100, os.path.join(d, "b").encode(), 1)
sys.exit(0 if rc != 0 and ctypes.get_errno() == errno.EINVAL else 1)
PY
log "renameat2(RENAME_NOREPLACE) answers EINVAL here"

tar -xzf "$tarball" -C "$work"
pkg="$(find "$work" -maxdepth 1 -type d -name 'xlings-*-linux-*' | head -1)"
[[ -x "$pkg/bin/xlings" ]] || fail "the release has no bin/xlings"
run() { env -u XLINGS_ACTIVE_SUBOS -u XLINGS_SUBOS_MODE -u XLINGS_BIN XLINGS_HOME="$pkg" HOME="$work" "$pkg/bin/xlings" "$@"; }

run self init >/dev/null
run update >/dev/null 2>&1 || true
out="$(run install -y xim:linux-headers xim:glibc 2>&1)" || { echo "$out"; fail "install on NFS"; }
grep -q "cannot publish" <<<"$out" && { echo "$out"; fail "a publication was refused"; }
sysroot="$pkg/subos/default"
[[ -e "$sysroot/usr/include/asm" ]] || fail "usr/include/asm is not in place"
[[ -e "$sysroot/lib/Scrt1.o" ]] || fail "lib/Scrt1.o is not in place"
find "$sysroot" -name '.xlings-materialize-*' | grep -q . && fail "a staging entry was left: $(find "$sysroot" -name '.xlings-materialize-*')"
log "linux-headers and glibc installed; their entries are in place"

run remove -y xim:linux-headers >/dev/null 2>&1 || fail "remove on NFS"
[[ ! -e "$sysroot/usr/include/asm" ]] || fail "usr/include/asm outlived its package"
echo "PASS: a home on NFSv3 installs and removes what places files into its SubOS"
