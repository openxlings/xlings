#Requires -Version 5.1
# E2E-07: Sub-Index Search + pkgindex-build
# Verifies that packages from sub-index repos (e.g., d2x:d2mcpp) are
# discoverable and installable after 'xlings update'.
# This validates the C++ os.files/os.isdir implementation in pkgindex-build.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\release_test_lib.ps1"

$ARCHIVE_PATH = if ($args.Count -ge 1) { $args[0] } else { Join-Path $ROOT_DIR 'build\release.zip' }
$ARCHIVE_PATH = Require-ReleaseArchive $ARCHIVE_PATH
Require-FixtureIndex

$PKG_DIR = Expand-ReleaseArchive $ARCHIVE_PATH 'sub_index_search'
Write-FixtureReleaseConfig $PKG_DIR

$env:XLINGS_HOME = $PKG_DIR
$env:Path = "$PKG_DIR\bin;$(Get-MinimalSystemPath)"

xlings self init
$env:Path = "$PKG_DIR\subos\current\bin;$PKG_DIR\bin;$(Get-MinimalSystemPath)"

Log "Running xlings update..."
xlings update
if ($LASTEXITCODE -ne 0) { Fail "xlings update failed" }

# ── Verify d2x sub-index package is installable ──
Log "Installing d2x:d2mcpp..."
$installDir = Join-Path $env:TEMP 'sub_index_search_test'
if (Test-Path $installDir) { Remove-Item -Recurse -Force $installDir }
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Push-Location $installDir

xlings install d2x:d2mcpp -y
if ($LASTEXITCODE -ne 0) { Fail "xlings install d2x:d2mcpp failed" }

if (-not (Test-Path "$installDir\d2mcpp")) { Fail "d2mcpp directory not created after install" }

Pop-Location
Remove-Item -Recurse -Force $installDir -ErrorAction SilentlyContinue

Log "PASS: sub-index search + pkgindex-build (Windows)"

# Land an explicit success: this script checks for exit codes it expects to be
# non-zero, and `pwsh -command ". script.ps1"` returns whatever $LASTEXITCODE
# happens to hold as the step's exit code -- turning a passing test into a red
# job with "PASS" printed right above the failure.
exit 0
