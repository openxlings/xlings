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
# Y-asset: the bundled index is artifact-managed (no .git); refreshed via
# `xlings update` from xlings-res/xim-index, not git pull.
if (Test-Path "$INDEX_DIR\.git") { Fail "bundled xim-pkgindex should be artifact-managed (no .git)" }
if (-not (Test-Path "$INDEX_DIR\.xlings-index-version")) { Fail "bundled xim-pkgindex missing .xlings-index-version marker" }

Log "PASS: release package includes usable xim-pkgindex snapshot"
