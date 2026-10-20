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

. "$PSScriptRoot\..\..\..\tests\e2e\release_test_lib.ps1"

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
# Before the host machine has started, whether it has KVM is not known -- said,
# never guessed; without the app there is no desktop, and the step says so.
$machine0 = $caps.modes | Where-Object { $_.name -eq 'machine' }
if ($machine0.ready -or $machine0.reason -notmatch 'not known') { Fail "machine before the host machine started: $($machine0 | ConvertTo-Json -Compress)" }
$desktop0 = $caps.modes | Where-Object { $_.name -eq 'desktop' }
if ($desktop0.ready -or $desktop0.route -ne 'xlings install luban-os') { Fail "desktop without the app: $($desktop0 | ConvertTo-Json -Compress)" }

# What a failure here needs to be read: the same step through the xlings CLI
# (its own diagnostics), this home's logs, and the host machine's.
function Show-Diagnostics([string]$name) {
    Run @('xlings', 'subos', 'new', "$name-diag", '--from', 'subos:luban-tiny', '--carrier', 'wsl2', '-y', '-v') | Out-Null
    Get-ChildItem "$PKG_DIR\logs" -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "--- $($_.FullName)"; Get-Content $_.FullName -Tail 40
    }
    $d = (& wsl.exe --list --quiet | Out-String) -split "`r?`n" | Where-Object { $_ -like 'xlings-*' } | Select-Object -First 1
    if ($d) { Run @('wsl.exe', '-d', $d, '-u', 'root', '--exec', '/bin/sh', '-c', 'tail -n 60 /xlings/logs/*.log /xlings/logs/*/*.log 2>/dev/null') | Out-Null }
}

# A system, made and used in the host machine.
$made = Run @('luban', 'new', 'box', '--edition', 'tiny', '-y')
if ($made.Code -ne 0) { Show-Diagnostics 'box'; Fail "luban new box --edition tiny failed" }
$uname = Run @('luban', 'run', 'box', '--', 'uname', '-s')
if ($uname.Code -ne 0 -or $uname.Out -notmatch 'Linux') { Fail "a command did not run in the system: $($uname.Out)" }
$desc = Run @('luban', 'status', 'box', '--json')
if ($desc.Code -ne 0) { Fail "luban status box failed" }
$null = $desc.Out | ConvertFrom-Json

# The Luban OS window (part 4 §8.2): the same system's console in the app,
# through the host machine's agent -- a command typed, its answer on screen.
$APP = if ($args.Count -ge 2) { $args[1] } else { '' }
if ($APP -and (Test-Path $APP)) {
    Copy-Item $APP "$PKG_DIR\bin\luban-os.exe"
    # With the app, a desktop can be shown.
    $desktop1 = ((Run @('luban', 'status', '--json')).Out | ConvertFrom-Json).modes | Where-Object { $_.name -eq 'desktop' }
    if (-not $desktop1.ready) { Fail "the app is installed and the desktop is not ready: $($desktop1 | ConvertTo-Json -Compress)" }
    $script = Join-Path $PKG_DIR 'app-script.txt'
    $state = Join-Path $PKG_DIR 'app-state.json'
    @(
        'wait console 120'
        'type uname -s'
        'key Return'
        'see 120s Linux'
        "state $state"
    ) | Set-Content -Encoding ascii $script
    $env:SDL_VIDEODRIVER = 'offscreen'
    $env:SDL_RENDER_DRIVER = 'software'
    $env:LUBAN_OS_LANG = 'en_US.UTF-8'
    $app = Run @("$PKG_DIR\bin\luban-os.exe", '--headless', '--attach', 'box', '--script', $script)
    if ($app.Code -ne 0) { Fail "the Luban OS window did not show box's console with its answer" }
    $st = Get-Content $state -Raw | ConvertFrom-Json
    if ($st.view -ne 'console') { Fail "the window is not on box's console: $($st.view)" }
    Write-Host "the Luban OS window: box's console through the Luban host machine"
} else {
    Fail "no Luban OS app to test (the lane's second argument)"
}

# A machine: on the host machine's KVM, or refused with why and what to do.
$caps = (Run @('luban', 'status', '--json')).Out | ConvertFrom-Json
$machine = $caps.modes | Where-Object { $_.name -eq 'machine' }
if (-not $machine.ready) {
    if (-not $machine.reason -or -not $machine.route) { Fail "machine not ready, and it does not say why and what to do" }
    # The host machine has started (box is in it): its answer is known now.
    if ($machine.reason -match 'not known') { Fail "the host machine started and its KVM is still not known" }
    $m = Run @('luban', 'enter', 'box', '--mode', 'machine', '--temp')
    if ($m.Code -eq 0) { Fail "a machine started where the report said it cannot" }
    if ($m.Out -notmatch [regex]::Escape($machine.reason.Substring(0, [Math]::Min(30, $machine.reason.Length)))) {
        Fail "the refusal does not say what the report says: $($m.Out)"
    }
    Write-Host "machine not ready here, as reported: $($machine.reason)"
} else {
    Write-Host "machine ready here: $($machine.how)"
    # Started from the window, through the host machine; its console there.
    $mscript = Join-Path $PKG_DIR 'app-machine.txt'
    @('wait console 300', 'see 300s box', "state $state") | Set-Content -Encoding ascii $mscript
    & luban config box --mode machine | Out-Null
    $m = Run @("$PKG_DIR\bin\luban-os.exe", '--headless', '--attach', 'box', '--script', $mscript)
    if ($m.Code -ne 0) { Fail "the Luban OS window did not start box as a machine" }
    & luban stop box | Out-Null
    & luban config box --mode sandbox | Out-Null
}

# Removed there; the host machine stays the home's.
$rm = Run @('luban', 'rm', 'box', '-y')
if ($rm.Code -ne 0) { Fail "luban rm box failed" }
# By name, from the data: with nothing left, the text suggests `luban new box`.
$after = Run @('luban', 'ls', '--json')
$listed = (($after.Out -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 1) | ConvertFrom-Json)
$listed = if ($listed.PSObject.Properties.Name -contains 'result') { $listed.result } else { $listed }
if ($listed.environments | Where-Object { $_.name -eq 'box' }) { Fail "box is still listed: $($after.Out)" }
$distro = (& wsl.exe --list --quiet | Out-String) -split "`r?`n" | Where-Object { $_ -like 'xlings-*' } | Select-Object -First 1
if ($distro) { & wsl.exe --unregister $distro | Out-Null }
Write-Host "PASS: Luban OS on Windows -- status, a system in the Luban host machine, a command in it, removed; the machine as reported"
exit 0
