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
require_pattern 'Measure-ReleaseLatency' 'Windows quick installer must probe release source latency'
require_pattern 'Sort-ReleaseCandidates' 'Windows quick installer must prefer lower-latency release candidates'
require_pattern 'Get-ReleaseAsset' 'Windows quick installer must select assets from release metadata'
require_pattern 'Convert-GitCodeAssetUrl' 'Windows quick installer must normalize GitCode release asset URLs'
require_pattern 'api\.gitcode\.com.*/releases/download' 'Windows quick installer must guard GitCode API-hosted asset URLs'
require_pattern 'https://gitcode\.com/' 'Windows quick installer must download GitCode release assets from gitcode.com'
reject_pattern 'GITEE|gitee\.com|Gitee' 'Windows quick installer should only use GitHub and GitCode sources'
reject_pattern 'GitHub xlings-res/xlings' 'Windows quick installer should not add a second GitHub resource source'
reject_pattern 'return \[string\]\$asset\.browser_download_url' 'Windows quick installer must not return raw GitCode asset URLs without normalization'
reject_pattern '^[[:space:]]*exit[[:space:]]+[0-9]+' 'Windows quick installer must not call exit from irm|iex execution path'

echo "PASS: Windows quick install resource probing checks"
