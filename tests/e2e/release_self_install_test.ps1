#Requires -Version 5.1
# E2E-02: Release Self Install
# Extracts the release zip, runs self install, verifies shims and paths.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\release_test_lib.ps1"

$ARCHIVE_PATH = if ($args.Count -ge 1) { $args[0] } else { Join-Path $ROOT_DIR 'build\release.zip' }
$ARCHIVE_PATH = Require-ReleaseArchive $ARCHIVE_PATH

$PKG_DIR = Expand-ReleaseArchive $ARCHIVE_PATH 'release_self_install'
$INSTALL_USER = Join-Path $RUNTIME_ROOT 'release_self_install_user'
if (Test-Path $INSTALL_USER) { Remove-Item -Recurse -Force $INSTALL_USER }
New-Item -ItemType Directory -Force -Path $INSTALL_USER | Out-Null

if (-not (Test-Path "$PKG_DIR\.xlings.json")) { Fail "bootstrap config missing in release package" }
if (-not (Test-Path "$PKG_DIR\bin\xlings.exe")) { Fail "bootstrap binary missing in release package" }

# Install with minimal env
$origProfile = $env:USERPROFILE
$origPath = $env:Path
$env:USERPROFILE = $INSTALL_USER
$env:Path = Get-MinimalSystemPath
Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue
try {
    & "$PKG_DIR\bin\xlings.exe" self install
    if ($LASTEXITCODE -ne 0) { Fail "self install exited with code $LASTEXITCODE" }
} finally {
    $env:USERPROFILE = $origProfile
    $env:Path = $origPath
}

$INSTALLED_HOME = Join-Path $INSTALL_USER '.xlings'
if (-not (Test-Path "$INSTALLED_HOME\bin\xlings.exe")) { Fail "installed home missing bin\xlings.exe" }
if (-not (Test-Path "$INSTALLED_HOME\subos\current")) { Fail "installed home missing subos\current link" }

# 0.4.8 collapsed to a single canonical entry point. The xim/xvm/xself/
# xsubos/xinstall shims were removed (see src/core/compact/xself.cppm).
if (-not (Test-Path "$INSTALLED_HOME\subos\default\bin\xlings.exe")) {
    Fail "shim xlings.exe missing after self install"
}
$legacy = @('xim.exe', 'xvm.exe', 'xsubos.exe', 'xself.exe', 'xinstall.exe')
foreach ($s in $legacy) {
    if (Test-Path "$INSTALLED_HOME\subos\default\bin\$s") {
        Fail "legacy alias shim '$s' should NOT be created (removed in 0.4.8)"
    }
}

# Verify installed binary works
$installedPath = "$INSTALLED_HOME\subos\current\bin;$INSTALLED_HOME\bin;$(Get-MinimalSystemPath)"
$origProfile = $env:USERPROFILE
$origPath = $env:Path
$env:USERPROFILE = $INSTALL_USER
$env:Path = $installedPath
Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue
try {
    & "$INSTALLED_HOME\bin\xlings.exe" -h | Out-Null
    $configOut = & "$INSTALLED_HOME\bin\xlings.exe" config 2>&1 | Out-String
    if ($configOut -notmatch [regex]::Escape($INSTALLED_HOME)) {
        Fail "installed home config output mismatch"
    }
} finally {
    $env:USERPROFILE = $origProfile
    $env:Path = $origPath
}

Log "PASS: release self install scenario"
