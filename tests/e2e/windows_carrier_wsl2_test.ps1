#Requires -Version 5.1
# E2E-08: the wsl2 carrier on a real Windows host (SubOS design part 3 §5.3).
#
# With WSL2: a Linux SubOS is created, used and removed through the carrier,
# and inside the carrier no Windows program runs and no Windows drive is
# mounted (CARRIER-WSL-LIFECYCLE, CARRIER-WSL-INTEROP-OFF).
# Without it (the hosted runner's usual state): the carrier is refused with
# exit 125 and the route that brings it, and a native SubOS is made as before
# (CARRIER-UNAVAILABLE).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\release_test_lib.ps1"

$ARCHIVE_PATH = if ($args.Count -ge 1) { $args[0] } else { Join-Path $ROOT_DIR 'build\release.zip' }
$ARCHIVE_PATH = Require-ReleaseArchive $ARCHIVE_PATH
Require-FixtureIndex
$PKG_DIR = Expand-ReleaseArchive $ARCHIVE_PATH 'carrier_wsl2'
Write-FixtureReleaseConfig $PKG_DIR
$env:XLINGS_HOME = $PKG_DIR
$env:Path = "$PKG_DIR\bin;$(Get-MinimalSystemPath)"
xlings self init | Out-Null

$env:WSL_UTF8 = '1'
$wsl2 = $false
try {
    $status = (& wsl.exe --status 2>&1 | Out-String)
    $wsl2 = ($LASTEXITCODE -eq 0) -and ($status -notmatch 'WSL 2 is not supported') -and ($status -notmatch 'kernel component')
    Write-Host "wsl --status (exit $LASTEXITCODE):`n$status"
} catch {
    Write-Host "wsl.exe is not available: $_"
}

$out = (& xlings subos new carrierbox --carrier wsl2 2>&1 | Out-String)
$rc = $LASTEXITCODE
Write-Host "subos new --carrier wsl2 (exit $rc):`n$out"

if ($wsl2 -and $rc -eq 0) {
    $info = (& xlings subos info carrierbox 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { Fail "subos info through the carrier failed: $info" }
    $list = (& xlings subos list --json 2>&1 | Out-String)
    if ($list -notmatch '"carrier":"wsl2"') { Fail "the carrier SubOS is not listed with its carrier: $list" }
    $distro = (& wsl.exe --list --quiet | Out-String) -split "`r?`n" | Where-Object { $_ -like 'xlings-*' } | Select-Object -First 1
    if (-not $distro) { Fail "no xlings carrier distribution registered" }
    # Interop off: a Windows program does not run inside, and C: is not there.
    & wsl.exe -d $distro -u root --exec /mnt/c/Windows/System32/cmd.exe /c exit 0 2>$null
    if ($LASTEXITCODE -eq 0) { Fail "a Windows program ran inside the carrier (interop is on)" }
    & xlings subos remove carrierbox -y
    if ($LASTEXITCODE -ne 0) { Fail "subos remove through the carrier failed" }
    & wsl.exe --unregister $distro | Out-Null
    Write-Host "PASS: wsl2 carrier lifecycle and interop-off on a real WSL2 host"
    exit 0
}

if ($rc -ne 125) { Fail "without WSL2 the carrier must refuse with exit 125 (got $rc)" }
if ($out -notmatch 'wsl|WSL') { Fail "the refusal does not name WSL or its route: $out" }
if (Test-Path "$PKG_DIR\subos\carrierbox") { Fail "a refused carrier SubOS left a directory behind" }
& xlings subos new nativebox
if ($LASTEXITCODE -ne 0) { Fail "a native SubOS could not be made after the carrier was refused" }
& xlings subos remove nativebox -y | Out-Null
Write-Host "PASS: wsl2 carrier unavailable here -> refused with its route; native SubOS unaffected"
