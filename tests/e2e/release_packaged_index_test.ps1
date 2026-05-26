#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\release_test_lib.ps1"

$ARCHIVE_PATH = if ($args.Count -ge 1) { $args[0] } else { Join-Path $ROOT_DIR 'build\release.zip' }
$ARCHIVE_PATH = Require-ReleaseArchive $ARCHIVE_PATH

$PKG_DIR = Expand-ReleaseArchive $ARCHIVE_PATH 'release_packaged_index'
$INDEX_DIR = Join-Path $PKG_DIR 'data\xim-pkgindex'

if (-not (Test-Path "$INDEX_DIR\pkgs")) { Fail "release package missing bundled xim-pkgindex/pkgs" }
if (-not (Test-Path "$INDEX_DIR\pkgs\p\patchelf.lua")) { Fail "bundled xim-pkgindex missing patchelf package" }
if (-not (Test-Path "$INDEX_DIR\.git")) { Fail "bundled xim-pkgindex should remain a git repo for xlings update" }

$mirror = if ($env:XLINGS_RELEASE_MIRROR) { $env:XLINGS_RELEASE_MIRROR } else { $null }
if (-not $mirror) {
    $config = Get-Content (Join-Path $PKG_DIR '.xlings.json') -Raw | ConvertFrom-Json
    $mirror = if ($config.mirror) { $config.mirror } else { 'GLOBAL' }
}

$expectedOrigin = 'https://gitee.com/sunrisepeak/xim-pkgindex.git'
if ($mirror -eq 'GLOBAL') {
    $expectedOrigin = 'https://github.com/openxlings/xim-pkgindex.git'
}
$actualOrigin = (& git -C $INDEX_DIR remote get-url origin).Trim()
if ($actualOrigin -ne $expectedOrigin) {
    Fail "bundled xim-pkgindex origin mismatch: expected $expectedOrigin, got $actualOrigin"
}

Log "PASS: release package includes usable xim-pkgindex snapshot"
