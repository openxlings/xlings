#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT/tools/other/quick_install.ps1"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

require_pattern() {
    local pattern="$1"
    local message="$2"
    grep -Eq "$pattern" "$SCRIPT" || fail "$message"
}

reject_pattern() {
    local pattern="$1"
    local message="$2"
    if grep -Eq "$pattern" "$SCRIPT"; then
        fail "$message"
    fi
}

require_pattern 'RESOURCE_REPO = "xlings-res/xlings"' 'Windows quick installer must probe xlings-res release resources'
require_pattern 'GITHUB_API = "https://api\.github\.com"' 'Windows quick installer must support GitHub release metadata'
require_pattern 'GITCODE_API = "https://api\.gitcode\.com/api/v5"' 'Windows quick installer must support GitCode xlings-res release metadata'
require_pattern 'GITEE_API = "https://gitee\.com/api/v5"' 'Windows quick installer must support Gitee xlings-res release metadata when available'
require_pattern 'Resolve-ReleaseCandidates' 'Windows quick installer must resolve release candidates lazily'
require_pattern 'Get-ReleaseAsset' 'Windows quick installer must select assets from release metadata'
reject_pattern '^[[:space:]]*exit[[:space:]]+[0-9]+' 'Windows quick installer must not call exit from irm|iex execution path'

echo "PASS: Windows quick install resource probing checks"
