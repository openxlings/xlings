#!/usr/bin/env bash
# The Luban OS app's release payload (Luban OS architecture design §14.7):
# build/luban-os-<version>-linux-<arch>.tar.gz and its .sha256 -- or, on a
# Mac, build/luban-os-<version>-macosx-arm64.tar.gz (part 4 §8.1): there the
# app links only the system's frameworks and libSystem, so the payload is the
# binary as built.
#
# The app opens a window, so it is not the static release: it links glibc and
# X11 (through SDL). The payload is an ordinary glibc program -- its loader the
# system's path, no RPATH -- and the index package (xim:luban-os) depends on
# the index's glibc and X11 libraries, which an install patches it onto, as it
# does every glibc payload. Its desktop entry and icon are in share/.
#
#   tools/luban_os_release.sh [binary]   # default: the newest dist build
#
# Needs patchelf (xlings install patchelf; CI: tools/ci/xlings_tools.sh patchelf).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project="$(cd "$here/.." && pwd)"
fail() { echo "[luban-os] $*" >&2; exit 1; }

version="$(sed -n 's/.*VERSION = "\([^"]*\)".*/\1/p' "$project/src/core/config.cppm" | head -1)"
[[ -n "$version" ]] || fail "no version in src/core/config.cppm"

if [[ "$(uname -s)" == Darwin ]]; then
  binary="${1:-}"
  if [[ -z "$binary" ]]; then
    binary="$(find "$project/target" -path '*/bin/luban-os/luban-os' -type f -perm -111 -exec ls -t {} + 2>/dev/null | head -1)"
  fi
  [[ -x "$binary" ]] || fail "no luban-os build (mcpp build -p luban-os --profile dist)"
  name="luban-os-$version-macosx-arm64"
  out="$project/build/$name"
  rm -rf "$out"
  mkdir -p "$out/bin" "$out/share/icons/hicolor/scalable/apps"
  cp "$binary" "$out/bin/luban-os"
  chmod 755 "$out/bin/luban-os"
  strip -x "$out/bin/luban-os" 2>/dev/null || true
  # Only the system's: libSystem, libobjc, the frameworks -- nothing a Mac
  # does not have, nothing from the build machine.
  while read -r lib; do
    case "$lib" in
      /usr/lib/libSystem*|/usr/lib/libobjc*|/usr/lib/libc++*|/usr/lib/libiconv*|/usr/lib/libz*|/System/Library/Frameworks/*) ;;
      *) fail "luban-os links $lib, which a Mac does not have" ;;
    esac
  done < <(otool -L "$out/bin/luban-os" | tail -n +2 | awk '{print $1}')
  cp "$project/apps/luban-os/share/luban-os.svg" "$out/share/icons/hicolor/scalable/apps/"
  printf 'luban-os %s (openxlings/xlings, apps/luban-os): the Luban OS window for macOS; the system frameworks only\n' \
    "$version" > "$out/README.provenance"
  file="$project/build/$name.tar.gz"
  tar -czf "$file" -C "$project/build" "$name"
  (cd "$project/build" && shasum -a 256 "$name.tar.gz" > "$name.tar.gz.sha256")
  echo "[luban-os] $(cat "$file.sha256")"
  exit 0
fi
arch="$(uname -m)"; [[ "$arch" == arm64 ]] && arch=aarch64
binary="${1:-}"
if [[ -z "$binary" ]]; then
  binary="$(find "$project/target" -path '*-linux-gnu/*/bin/luban-os/luban-os' -type f -perm -111 -exec ls -t {} + 2>/dev/null | head -1)"
fi
[[ -x "$binary" ]] || fail "no luban-os build (mcpp build -p luban-os --profile dist)"
command -v patchelf >/dev/null || fail "patchelf is needed: xlings install patchelf"

name="luban-os-$version-linux-$arch"
out="$project/build/$name"
rm -rf "$out"
mkdir -p "$out/bin" "$out/share/applications" "$out/share/icons/hicolor/scalable/apps"
cp "$binary" "$out/bin/luban-os"
chmod 755 "$out/bin/luban-os"
case "$arch" in
  x86_64)  loader=/lib64/ld-linux-x86-64.so.2 ;;
  aarch64) loader=/lib/ld-linux-aarch64.so.1 ;;
  *) fail "no glibc loader path for $arch" ;;
esac
patchelf --set-interpreter "$loader" --remove-rpath "$out/bin/luban-os"
strip "$out/bin/luban-os" 2>/dev/null || true

# What it may need: glibc's, libgcc's and X11's -- the index packages its
# recipe depends on. Anything else is a dependency nobody declared.
allowed="libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 ld-linux-x86-64.so.2 ld-linux-aarch64.so.1 libgcc_s.so.1
libX11.so.6 libxcb.so.1 libXcursor.so.1 libXext.so.6 libXfixes.so.3 libXi.so.6 libXrandr.so.2 libXrender.so.1"
for lib in $(readelf -d "$out/bin/luban-os" | awk '/NEEDED/ { gsub(/[\[\]]/, "", $NF); print $NF }'); do
  grep -qw -- "$lib" <<<"$allowed" || fail "luban-os needs $lib, which its recipe does not declare"
done
readelf -l "$out/bin/luban-os" | grep -q "interpreter: $loader" || fail "the loader is not $loader"
! readelf -d "$out/bin/luban-os" | grep -qE 'RPATH|RUNPATH' || fail "an RPATH is left (the build's own paths)"

cp "$project/apps/luban-os/share/luban-os.desktop" "$out/share/applications/"
cp "$project/apps/luban-os/share/luban-os.svg" "$out/share/icons/hicolor/scalable/apps/"
printf 'luban-os %s (openxlings/xlings, apps/luban-os): the Luban OS window; glibc and X11 linked, loader %s, no RPATH -- an xlings install patches it onto the index'"'"'s glibc\n' \
  "$version" "$loader" > "$out/README.provenance"

file="$project/build/$name.tar.gz"
tar --owner=0 --group=0 --numeric-owner -czf "$file" -C "$project/build" "$name"
(cd "$project/build" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "[luban-os] $(cat "$file.sha256")"
