#!/usr/bin/env bash
# Back-compat shim — xlings-specific entry point for the generic mirror_res.sh.
# Kept because release.yml (`mirror-binaries` job) and docs call this by name.
#
# Usage: tools/mirror_xlings_res.sh <version>          # e.g. 0.4.63
# See tools/mirror_res.sh for the actual logic and auth/env contract.
set -euo pipefail
exec "$(dirname "$0")/mirror_res.sh" xlings "$@"
