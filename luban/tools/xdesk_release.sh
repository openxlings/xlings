#!/usr/bin/env bash
# xdesk's release payload (Luban OS desktop part 2 §2.1, D7):
# build/xdesk-<version>-linux-<arch>.tar.gz and its .sha256.
#
# xdesk is a glibc program (wlroots and its C libraries are built by mcpp,
# some of them as shared objects that keep one copy per process). The payload
# carries those privately, in libexec/lib -- not lib/, which a root's
# projection would put on every program's path beside the root's own Mesa
# libraries -- and is otherwise an ordinary glibc program: its loader the
# system's path, no RPATH. The index package (xim:xdesk) depends on the
# index's glibc and gcc-runtime; an install patches it onto them and adds
# libexec/lib to its RPATH. The sessions it is (classic, canvas) are in
# share/wayland-sessions.
#
#   luban/tools/xdesk_release.sh [binary]   # default: the newest dist build
#
# Needs patchelf (xlings install patchelf; CI: tools/ci/xlings_tools.sh patchelf).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project="$(cd "$here/../.." && pwd)"
fail() { echo "[xdesk] $*" >&2; exit 1; }

version="$(sed -n 's/.*VERSION = "\([^"]*\)".*/\1/p' "$project/src/core/config.cppm" | head -1)"
[[ -n "$version" ]] || fail "no version in src/core/config.cppm"
arch="$(uname -m)"; [[ "$arch" == arm64 ]] && arch=aarch64
binary="${1:-}"
if [[ -z "$binary" ]]; then
  binary="$(find "$project/target" -path '*-linux-gnu/*/bin/xdesk-app/xdesk' -type f -perm -111 -exec ls -t {} + 2>/dev/null | head -1)"
fi
[[ -x "$binary" ]] || fail "no xdesk build (mcpp build -p xdesk-app --profile dist)"
command -v patchelf >/dev/null || fail "patchelf is needed: xlings install patchelf"
built="$(dirname "$binary")/.."

name="xdesk-$version-linux-$arch"
out="$project/build/$name"
rm -rf "$out"
mkdir -p "$out/bin" "$out/libexec/lib" "$out/share/wayland-sessions" "$out/share/xdesk" "$out/share/applications"
cp "$binary" "$out/bin/xdesk"
chmod 755 "$out/bin/xdesk"
case "$arch" in
  x86_64)  loader=/lib64/ld-linux-x86-64.so.2 ;;
  aarch64) loader=/lib/ld-linux-aarch64.so.1 ;;
  *) fail "no glibc loader path for $arch" ;;
esac
patchelf --set-interpreter "$loader" --remove-rpath "$out/bin/xdesk"
strip "$out/bin/xdesk" 2>/dev/null || true

# The libraries mcpp built as shared objects, by their sonames.
private="libwayland-server.so.0 libwayland-client.so.0 libdrm.so.2 libffi.so.8"
for lib in $private; do
  src="$(find "$built" -maxdepth 1 -name "$lib" | head -1)"
  [[ -n "$src" ]] || fail "no $lib beside the build"
  cp -L "$src" "$out/libexec/lib/$lib"
  chmod 644 "$out/libexec/lib/$lib"
  patchelf --remove-rpath "$out/libexec/lib/$lib"
  strip --strip-unneeded "$out/libexec/lib/$lib" 2>/dev/null || true
done

# What it may need: glibc's, libgcc's and libstdc++'s (the index's glibc and
# gcc-runtime) and the libraries it carries. Anything else is a dependency
# nobody declared.
allowed="libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 ld-linux-x86-64.so.2 ld-linux-aarch64.so.1
libgcc_s.so.1 libstdc++.so.6 $private"
for f in "$out/bin/xdesk" "$out"/libexec/lib/*; do
  for lib in $(readelf -d "$f" | awk '/NEEDED/ { gsub(/[\[\]]/, "", $NF); print $NF }'); do
    grep -qw -- "$lib" <<<"$allowed" || fail "$(basename "$f") needs $lib, which its recipe does not declare"
  done
  ! readelf -d "$f" | grep -qE 'RPATH|RUNPATH' || fail "an RPATH is left in $(basename "$f") (the build's own paths)"
done
readelf -l "$out/bin/xdesk" | grep -q "interpreter: $loader" || fail "the loader is not $loader"

cp "$project"/luban/xdesk/app/share/wayland-sessions/*.desktop "$out/share/wayland-sessions/"
cp "$project"/luban/xdesk/app/share/applications/*.desktop "$out/share/applications/"
# The profiles' defaults (xdesk.config reads them from $XDG_DATA_DIRS/xdesk).
cp "$project"/luban/xdesk/app/share/xdesk/*.ini "$out/share/xdesk/"

# Fonts of its own, so it has text in any root (a root's share/fonts is the
# root's): DejaVu Sans and WenQuanYi Zen Hei, from the published labwc-desktop
# tree, its digest checked.
fonts_from="https://github.com/xlings-res/labwc-desktop/releases/download/0.7.2-r1/labwc-desktop-0.7.2-r1-linux-x86_64.tar.gz"
fonts_sha=2e20a3cf3b79e0440a1a67ef4252b1aa32104aa49e322f921482eceed21a5bcc
cache="$project/build/.xdesk-fonts.tar.gz"
if ! echo "$fonts_sha  $cache" | sha256sum -c --status 2>/dev/null; then
  curl -fsSL --retry 3 --max-time 600 -o "$cache" "$fonts_from" || fail "cannot fetch the fonts ($fonts_from)"
  echo "$fonts_sha  $cache" | sha256sum -c --status || fail "the fonts' tree is not the one published (sha256)"
fi
mkdir -p "$out/share/xdesk/fonts"
tar -xzf "$cache" -C "$out/share/xdesk/fonts" --strip-components=4 \
  labwc-desktop-0.7.2-r1-linux-x86_64/share/fonts/dejavu/DejaVuSans.ttf \
  labwc-desktop-0.7.2-r1-linux-x86_64/share/fonts/wqy-zenhei/wqy-zenhei.ttc || fail "no fonts in the tree"
printf 'xdesk %s (openxlings/xlings, luban/xdesk): the Luban OS desktop compositor and shell; glibc, loader %s, no RPATH; %s in libexec/lib -- an xlings install patches it onto the index'"'"'s glibc and gcc-runtime\nfonts (share/xdesk/fonts): DejaVu Sans (Bitstream Vera / DejaVu licence) and WenQuanYi Zen Hei (GPL-2.0 with the font exception), from xlings-res/labwc-desktop 0.7.2-r1 (sha256 %s)\n' \
  "$version" "$loader" "$private" "$fonts_sha" > "$out/README.provenance"

file="$project/build/$name.tar.gz"
tar --owner=0 --group=0 --numeric-owner -czf "$file" -C "$project/build" "$name"
(cd "$project/build" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "[xdesk] $(cat "$file.sha256")"
