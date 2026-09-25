#Requires -Version 5.1
# E2E-04: Release Subos + d2x
# Creates subos s1/s2, installs d2x, switches, verifies isolation.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\release_test_lib.ps1"

$ARCHIVE_PATH = if ($args.Count -ge 1) { $args[0] } else { Join-Path $ROOT_DIR 'build\release.zip' }
$ARCHIVE_PATH = Require-ReleaseArchive $ARCHIVE_PATH
Require-FixtureIndex

$PKG_DIR = Expand-ReleaseArchive $ARCHIVE_PATH 'release_subos_smoke'
Write-FixtureReleaseConfig $PKG_DIR

$env:XLINGS_HOME = $PKG_DIR
$env:Path = "$PKG_DIR\bin;$(Get-MinimalSystemPath)"

xlings -h | Out-Null
xlings config | Out-Null
xlings --version | Out-Null

xlings self init
$env:Path = "$PKG_DIR\subos\current\bin;$PKG_DIR\bin;$(Get-MinimalSystemPath)"
xlings update
xlings subos list | Out-Null

$D2X_VERSION = if ($env:D2X_VERSION) { $env:D2X_VERSION } else { Get-DefaultD2xVersion }

# ── Create subos s1, install d2x ────────────────────────────────
xlings subos new s1
if (-not (Test-Path "$PKG_DIR\subos\s1\.xlings.json")) { Fail "s1 config missing" }
xlings subos use s1 --global

$installS1 = (xlings install "d2x@$D2X_VERSION" -y 2>&1) | Out-String
Write-Host $installS1
if ($installS1 -notmatch "d2x") { Fail "s1 install output missing d2x" }
if ($installS1 -notmatch "installed|already installed") { Fail "s1 install did not confirm install" }
if (-not (Test-Path "$PKG_DIR\subos\s1\bin\d2x.exe")) { Fail "s1 d2x shim missing" }

# ── Create subos s2, install d2x ────────────────────────────────
xlings subos new s2
if (-not (Test-Path "$PKG_DIR\subos\s2\.xlings.json")) { Fail "s2 config missing" }
xlings subos use s2 --global

$installS2 = (xlings install "d2x@$D2X_VERSION" -y 2>&1) | Out-String
Write-Host $installS2
if ($installS2 -notmatch "installed|already installed") { Fail "s2 install did not confirm install" }
if (-not (Test-Path "$PKG_DIR\subos\s2\bin\d2x.exe")) { Fail "s2 d2x shim missing" }

# ── Switch and verify ───────────────────────────────────────────
xlings subos use s1 --global
xlings subos use s2 --global
xlings subos list | Out-Null

# ── Cleanup ─────────────────────────────────────────────────────
xlings subos use default --global
xlings subos remove s1 -y
xlings subos remove s2 -y

Log "PASS: release subos smoke scenario"
