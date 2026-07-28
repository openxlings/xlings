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

# ── PowerShell hosts ────────────────────────────────────────────────
# `self install` hooks every PowerShell host it can start. Windows PowerShell
# 5.1 and PowerShell 7+ read DIFFERENT startup files, and `xlings subos use`
# spawns pwsh.exe first — hooking only 5.1 left the shell xlings itself
# launches without XLINGS_BIN on PATH (#387).
#
# $PROFILE resolves from the user's real Documents folder; no environment
# variable redirects it, so this test necessarily touches the runner's own
# profiles. Snapshot them here and put them back below, or every later CI
# step would start by sourcing this throwaway home.
$psHosts = @()
foreach ($exe in @('powershell', 'pwsh')) {
    $found = Get-Command $exe -ErrorAction SilentlyContinue
    if (-not $found) { continue }
    $reported = & $exe -NoProfile -NonInteractive -Command '$PROFILE' 2>$null | Select-Object -First 1
    if (-not $reported) { Fail "$exe did not report a `$PROFILE path" }
    $reported = $reported.Trim()
    $backup = $null
    if (Test-Path $reported) { $backup = Get-Content $reported -Raw }
    $psHosts += [pscustomobject]@{
        Exe    = $exe
        Path   = $reported
        Dir    = (Split-Path $found.Source -Parent)
        Backup = $backup
    }
}
if ($psHosts.Count -eq 0) { Fail "no PowerShell host found on this machine" }

# Install with minimal env. The host directories are added explicitly:
# Get-MinimalSystemPath carries System32 (Windows PowerShell) but not
# C:\Program Files\PowerShell\7, and a pwsh xlings cannot start is a pwsh it
# is right to skip — which would make the assertion below vacuous.
$hostDirs = ($psHosts | ForEach-Object { $_.Dir }) -join ';'
$INSTALLED_HOME = Join-Path $INSTALL_USER '.xlings'
$XLINGS_PS_PROFILE = Join-Path $INSTALLED_HOME 'config\shell\xlings-profile.ps1'

try {
    # Start from an unhooked profile. The CI job installs a bootstrap xlings
    # into the runner's own home before this test runs, and hooking is
    # idempotent on a marker — so an already-hooked profile would correctly
    # be left alone, and the assertions below would be measuring nothing.
    foreach ($h in $psHosts) {
        Remove-Item -Force $h.Path -ErrorAction SilentlyContinue
    }

    $origProfile = $env:USERPROFILE
    $origPath = $env:Path
    $env:USERPROFILE = $INSTALL_USER
    $env:Path = "$hostDirs;$(Get-MinimalSystemPath)"
    Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue
    try {
        & "$PKG_DIR\bin\xlings.exe" self install
        if ($LASTEXITCODE -ne 0) { Fail "self install exited with code $LASTEXITCODE" }

        # The regression: EVERY host that was reachable must now source
        # xlings-profile.ps1 from the home just installed.
        foreach ($h in $psHosts) {
            if (-not (Test-Path $h.Path)) {
                Fail "self install left the $($h.Exe) profile unwritten: $($h.Path)"
            }
            $content = Get-Content $h.Path -Raw
            if ($content -notmatch [regex]::Escape($XLINGS_PS_PROFILE)) {
                Fail "$($h.Exe) profile does not source xlings-profile.ps1 ($($h.Path))"
            }
        }
        # Idempotency is not re-checked here on purpose: a second
        # `self install` of the same version stops at an interactive
        # confirmation, which in CI answers "no" and skips profile setup
        # entirely — the assertion would pass without ever running the code
        # it claims to cover. It is covered by
        # tests/unit/test_shell_profile.cpp instead.
    } finally {
        $env:USERPROFILE = $origProfile
        $env:Path = $origPath
    }
} finally {
    foreach ($h in $psHosts) {
        if ($null -eq $h.Backup) {
            Remove-Item -Force $h.Path -ErrorAction SilentlyContinue
        } else {
            Set-Content -Path $h.Path -Value $h.Backup -NoNewline
        }
    }
}
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
