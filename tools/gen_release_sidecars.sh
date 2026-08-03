#!/usr/bin/env bash
# Generate the .sha256 sidecars published alongside the release archives.
#
#   gen_release_sidecars.sh <artifacts-dir> <output-dir>
#
# A sidecar records BOTH a digest and a filename, and `sha256sum -c` -- what a
# user or a mirror actually runs -- resolves that filename relative to the
# checking directory. Generating with `sha256sum artifacts/<job>/<file>` bakes
# in a path that exists only on the build runner, so every published sidecar
# fails verification everywhere else while still carrying the right digest.
# Hence the `cd`: the recorded name is the asset's own.
#
# This lives in a script rather than inline in release.yml so a test can run
# it, which is the only way to tell a sidecar that verifies from one that
# merely looks like it should.
set -euo pipefail

artifacts=${1:?artifacts directory required}
outdir=${2:?output directory required}

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    else
        shasum -a 256 "$@"
    fi
}

mkdir -p "$outdir"
count=0
while IFS= read -r file; do
    dir=$(dirname "$file")
    name=$(basename "$file")
    (cd "$dir" && sha256 "$name") > "$outdir/$name.sha256"
    count=$((count + 1))
done < <(find "$artifacts" -type f \( -name '*.tar.gz' -o -name '*.zip' \) | sort)

if [[ $count -eq 0 ]]; then
    echo "no release archives found under $artifacts" >&2
    exit 1
fi

echo "generated $count sidecar(s) in $outdir"
