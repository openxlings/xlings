#Requires -Version 5.1
# Luban OS on Windows (part 4 §8.2): a Luban system in the Luban host machine
# (WSL 2), from the release under test -- `luban status` says what this PC
# can run, a system is made, a command runs in it, it is described and
# removed; a machine is either started on the host machine's KVM or refused
# with its reason and next step (PLATFORM-CAPS, CARRIER-WSL2-LUBAN).
#
# Needs WSL 2 (the CI lane enables it with Vampire/setup-wsl) and the Linux
# build of the same commit for the host machine: $env:XLINGS_CARRIER_GUEST_XLINGS
# names its bin/xlings, with bin/luban beside it.
# xtest: covers=PLATFORM-CAPS,CARRIER-WSL2-LUBAN requires=windows,wsl2,network

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\release_test_lib.ps1"

$ARCHIVE_PATH = if ($args.Count -ge 1) { $args[0] } else { Join-Path $ROOT_DIR 'build\release.zip' }
$ARCHIVE_PATH = Require-ReleaseArchive $ARCHIVE_PATH
if (-not $env:XLINGS_CARRIER_GUEST_XLINGS -or -not (Test-Path $env:XLINGS_CARRIER_GUEST_XLINGS)) {
    Fail "XLINGS_CARRIER_GUEST_XLINGS does not name the Linux build of this commit"
}
$PKG_DIR = Expand-ReleaseArchive $ARCHIVE_PATH 'luban_wsl2'
$env:XLINGS_HOME = $PKG_DIR
$env:Path = "$PKG_DIR\bin;$(Get-MinimalSystemPath)"
& xlings self init | Out-Null
$env:WSL_UTF8 = '1'

function Run([string[]]$argv) {
    $out = (& $argv[0] $argv[1..($argv.Count - 1)] 2>&1 | Out-String)
    $code = $LASTEXITCODE
    Write-Host "> $($argv -join ' ') (exit $code)`n$out"
    return @{ Out = $out; Code = $code }
}

# What this PC can run: with WSL 2, a sandbox is ready.
$st = Run @('luban', 'status')
if ($st.Out -notmatch 'sandbox\s+ready') { Fail "with WSL 2 the sandbox is not ready: $($st.Out)" }
$caps = (Run @('luban', 'status', '--json')).Out | ConvertFrom-Json
$sandbox = $caps.modes | Where-Object { $_.name -eq 'sandbox' }
if (-not $sandbox.ready) { Fail "luban status --json: the sandbox is not ready" }
if ($sandbox.traits.carrier -ne 'wsl2') { Fail "the sandbox does not run in the wsl2 carrier: $($sandbox | ConvertTo-Json -Compress)" }

# A system, made and used in the host machine.
$made = Run @('luban', 'new', 'box', '--edition', 'tiny', '-y')
if ($made.Code -ne 0) { Fail "luban new box --edition tiny failed" }
$uname = Run @('luban', 'run', 'box', '--', 'uname', '-s')
if ($uname.Code -ne 0 -or $uname.Out -notmatch 'Linux') { Fail "a command did not run in the system: $($uname.Out)" }
$desc = Run @('luban', 'status', 'box', '--json')
if ($desc.Code -ne 0) { Fail "luban status box failed" }
$null = $desc.Out | ConvertFrom-Json

# A machine: on the host machine's KVM, or refused with why and what to do.
$caps = (Run @('luban', 'status', '--json')).Out | ConvertFrom-Json
$machine = $caps.modes | Where-Object { $_.name -eq 'machine' }
if (-not $machine.ready) {
    if (-not $machine.reason -or -not $machine.route) { Fail "machine not ready, and it does not say why and what to do" }
    $m = Run @('luban', 'enter', 'box', '--mode', 'machine', '--temp')
    if ($m.Code -eq 0) { Fail "a machine started where the report said it cannot" }
    if ($m.Out -notmatch [regex]::Escape($machine.reason.Substring(0, [Math]::Min(30, $machine.reason.Length)))) {
        Fail "the refusal does not say what the report says: $($m.Out)"
    }
    Write-Host "machine not ready here, as reported: $($machine.reason)"
} else {
    Write-Host "machine ready here: $($machine.how)"
}

# Removed there; the host machine stays the home's.
$rm = Run @('luban', 'rm', 'box', '-y')
if ($rm.Code -ne 0) { Fail "luban rm box failed" }
$after = Run @('luban', 'ls')
if ($after.Out -match '\bbox\b') { Fail "box is still listed: $($after.Out)" }
$distro = (& wsl.exe --list --quiet | Out-String) -split "`r?`n" | Where-Object { $_ -like 'xlings-*' } | Select-Object -First 1
if ($distro) { & wsl.exe --unregister $distro | Out-Null }
Write-Host "PASS: Luban OS on Windows -- status, a system in the Luban host machine, a command in it, removed; the machine as reported"
exit 0
