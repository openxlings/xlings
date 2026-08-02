#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT_DIR/tools/other/quick_install.sh"

bash -n "$SCRIPT"
grep -F 'linux-x86_64|linux-aarch64|macosx-arm64' "$SCRIPT" >/dev/null
grep -F '"${url}.sha256"' "$SCRIPT" >/dev/null
grep -F 'verify_sidecar' "$SCRIPT" >/dev/null

latest=$(printf '%s\n' 2026.8.3.9 2026.8.3.10 \
    | sort -t. -k1,1n -k2,2n -k3,3n -k4,4n | tail -1)
[[ "$latest" == "2026.8.3.10" ]]

echo "quick_install.sh contracts: ok"
