#!/usr/bin/env bash
# The fonts Luban OS ships, as the layout baselines were recorded with them
# (tests/luban-os/baseline/fonts.txt): fetched, checked against their
# digest, unpacked, and named to later steps as LUBAN_OS_FONTS.
#
#   tools/ci/luban_fonts.sh [dir]     (default $RUNNER_TEMP/luban-fonts)
#
# Prints the font directory; under GitHub Actions also writes it to
# $GITHUB_ENV.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
dir="${1:-${RUNNER_TEMP:-/tmp}/luban-fonts}"
read -r version url sha < <(grep -v '^#' "$root/tests/luban-os/baseline/fonts.txt" | head -1)
fonts="$dir/luban-fonts-$version/share/fonts/luban"
if [[ ! -f "$fonts/JetBrainsMono-Regular.ttf" ]]; then
  mkdir -p "$dir"
  curl -fsSL --retry 3 -o "$dir/fonts.tar.gz" "$url"
  [[ "$(sha256sum "$dir/fonts.tar.gz" | cut -d' ' -f1)" == "$sha" ]] || { echo "luban-fonts $version: digest mismatch" >&2; exit 1; }
  tar -xzf "$dir/fonts.tar.gz" -C "$dir"
  rm -f "$dir/fonts.tar.gz"
fi
[[ -f "$fonts/JetBrainsMono-Regular.ttf" && -f "$fonts/Inter-Regular.ttf" ]] || { echo "luban-fonts $version: no fonts in $fonts" >&2; exit 1; }
[[ -n "${GITHUB_ENV:-}" ]] && echo "LUBAN_OS_FONTS=$fonts" >> "$GITHUB_ENV"
echo "$fonts"
