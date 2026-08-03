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
# Locate the host executables under the FULL path first — the install below
# runs on a deliberately minimal one, and Get-MinimalSystemPath carries
# System32 but neither System32\WindowsPowerShell\v1.0 nor
# C:\Program Files\PowerShell\7. A host xlings cannot start is a host it is
# right to skip, which would make the assertions vacuous.
$psHosts = @()
foreach ($exe in @('powershell', 'pwsh')) {
    $found = Get-Command $exe -ErrorAction SilentlyContinue
    if (-not $found) { continue }
    $psHosts += [pscustomobject]@{
        Exe    = $exe
        Dir    = (Split-Path $found.Source -Parent)
        Path   = $null   # resolved below, under the install's own environment
        Backup = $null
    }
}
if ($psHosts.Count -eq 0) { Fail "no PowerShell host found on this machine" }

$hostDirs = ($psHosts | ForEach-Object { $_.Dir }) -join ';'
$INSTALLED_HOME = Join-Path $INSTALL_USER '.xlings'
$XLINGS_PS_PROFILE = Join-Path $INSTALLED_HOME 'config\shell\xlings-profile.ps1'

try {
    $origProfile = $env:USERPROFILE
    $origPath = $env:Path
    $env:USERPROFILE = $INSTALL_USER
    $env:Path = "$hostDirs;$(Get-MinimalSystemPath)"
    Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue
    try {
        # Resolve $PROFILE per host INSIDE the redirected environment, because
        # it is not environment-independent: pwsh 7 derives it from
        # USERPROFILE and lands inside this sandbox, while Windows PowerShell
        # 5.1 may answer with the runner's real Documents folder. Resolving
        # before the redirect compared the install's work against the wrong
        # path for whichever host follows USERPROFILE.
        #
        # `-Command '$PROFILE'` and not the bare-word form: 5.1 does not
        # expand a bare word in argument mode (see shell_profile.cppm), and a
        # probe that silently answers "" would make this test agree with a
        # broken install instead of catching it.
        # A real user profile has a Documents folder; this throwaway one does
        # not, and Windows PowerShell 5.1 answers an empty $PROFILE when it is
        # missing (.NET Framework's GetFolderPath returns "" for a folder that
        # does not exist; .NET Core, i.e. pwsh 7, returns the path regardless).
        # Without this the sandbox is not a stand-in for a user's home, and
        # 5.1 drops out of the test for a reason that cannot happen on a real
        # machine.
        New-Item -ItemType Directory -Force (Join-Path $INSTALL_USER 'Documents') | Out-Null

        foreach ($h in $psHosts) {
            $reported = & $h.Exe -NoProfile -NonInteractive -Command '$PROFILE' 2>$null |
                        Select-Object -First 1
            if (-not $reported) {
                Fail "$($h.Exe) reported an empty `$PROFILE under USERPROFILE=$INSTALL_USER"
            }
            $h.Path = $reported.Trim()
            # Snapshot anything outside the sandbox so later CI steps do not
            # end up sourcing this throwaway home; restored in the finally.
            if (Test-Path $h.Path) { $h.Backup = Get-Content $h.Path -Raw }
            # Start unhooked. The CI job installs a bootstrap xlings into the
            # runner's own home first, and hooking is idempotent on a marker —
            # an already-hooked profile would correctly be left alone, and the
            # assertions would be measuring nothing.
            Remove-Item -Force $h.Path -ErrorAction SilentlyContinue
        }

        # --verbose so the per-host probe (command, exit code, raw reply) is in
        # the log. When a host silently fails to be hooked, that line is the
        # difference between a diagnosis and another CI round-trip.
        & "$PKG_DIR\bin\xlings.exe" self install --verbose
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
        # Path is null for hosts we never got to resolve (an earlier one
        # failed); there is nothing of theirs to put back.
        if (-not $h.Path) { continue }
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

# Land an explicit success: this script checks for exit codes it expects to be
# non-zero, and `pwsh -command ". script.ps1"` returns whatever $LASTEXITCODE
# happens to hold as the step's exit code -- turning a passing test into a red
# job with "PASS" printed right above the failure.
exit 0
