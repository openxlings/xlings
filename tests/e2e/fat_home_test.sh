#!/usr/bin/env bash
# A home on a filesystem without symbolic links (FAT): refused when it is
# made, naming the filesystem and what to do -- not a home that half-works
# (shims and SubOS links are symbolic links).
#
#   fat_home_test.sh <release tarball> <a vfat mount point>
#
# xtest: covers=ATOMIC-PUBLICATION requires=linux,sudo
set -euo pipefail
tarball="${1:?usage: fat_home_test.sh <release tarball> <vfat mount>}"
mount="${2:?usage: fat_home_test.sh <release tarball> <vfat mount>}"
fail() { echo "FAIL: $*"; exit 1; }

grep -qE " $mount vfat " /proc/mounts || fail "$mount is not a vfat mount"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tar -xzf "$tarball" -C "$work"
pkg="$(find "$work" -maxdepth 1 -type d -name 'xlings-*-linux-*' | head -1)"
[[ -x "$pkg/bin/xlings" ]] || fail "the release has no bin/xlings"

home="$mount/home"
rc=0
out="$(env -u XLINGS_ACTIVE_SUBOS -u XLINGS_SUBOS_MODE -u XLINGS_BIN XLINGS_HOME="$home" HOME="$work" \
       "$pkg/bin/xlings" self init 2>&1)" || rc=$?
echo "$out"
[[ $rc -ne 0 ]] || fail "a home was made on a filesystem without symbolic links"
grep -q "without symbolic links (vfat)" <<<"$out" || fail "the refusal does not name the filesystem"
grep -q "XLINGS_HOME" <<<"$out" || fail "the refusal does not say what to do"
echo "PASS: a home on FAT is refused at its making, naming the filesystem and what to do"
