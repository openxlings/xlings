#!/usr/bin/env bash
# The Luban pointer theme's release payload (luban:cursors; design 2026-10-20
# WuXian §8.1): build/luban-cursors-<version>.tar.gz and its .sha256, made by
# cursor-make from luban/os/window/cursors/luban.path -- the text the Luban OS
# window carries, so the pointer is the same inside a system and out.
#
#   luban-cursors-<version>/share/icons/Luban/cursors   Xcursor files at 24, 30, 36, 48, 60
#                                                       and 72 px; the X11 names as links
#   luban-cursors-<version>/share/icons/default         inherits Luban: what a client
#                                                       naming no theme gets
#
# Data: the same files for every system and machine.
#
#   luban/tools/cursors_release.sh [cursor-make]   # default: the newest dist build
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project="$(cd "$here/../.." && pwd)"
fail() { echo "[cursors] $*" >&2; exit 1; }

version="$(sed -n 's/.*VERSION = "\([^"]*\)".*/\1/p' "$project/src/core/config.cppm" | head -1)"
[[ -n "$version" ]] || fail "no version in src/core/config.cppm"
tool="${1:-}"
if [[ -z "$tool" ]]; then
  tool="$(find "$project/target" -path '*/bin/cursor-make/cursor-make' -type f -perm -111 -exec ls -t {} + 2>/dev/null | head -1)"
fi
[[ -x "$tool" ]] || fail "no cursor-make build (mcpp build -p cursor-make --profile dist)"

name="luban-cursors-$version"
out="$project/build/$name"
rm -rf "$out"
"$tool" "$project/luban/os/window/cursors/luban.path" "$out" --name Luban --default
# Every shape opensal names but none, and the default theme.
for shape in default text pointer grab grabbing move ew-resize ns-resize nwse-resize nesw-resize crosshair wait progress not-allowed help; do
  [[ -f "$out/share/icons/Luban/cursors/$shape" ]] || fail "no $shape in the theme"
done
[[ -L "$out/share/icons/Luban/cursors/left_ptr" ]] || fail "no X11 names (links) in the theme"
grep -q '^Inherits=Luban$' "$out/share/icons/default/index.theme" || fail "the default theme does not inherit Luban"
mkdir -p "$out/share/licenses/luban-cursors"
cp "$project/LICENSE" "$out/share/licenses/luban-cursors/LICENSE"
printf 'luban-cursors %s (openxlings/xlings): the Luban pointer theme, drawn by cursorkit (luban/kits/cursor) from luban/os/window/cursors/luban.path; Apache-2.0\n' \
  "$version" > "$out/README.provenance"

file="$project/build/$name.tar.gz"
tar --owner=0 --group=0 --numeric-owner -czf "$file" -C "$project/build" "$name"
(cd "$project/build" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "[cursors] $(cat "$file.sha256")"
