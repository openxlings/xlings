#Requires -Version 5.1
# Luban OS on Windows (part 4 §8.2): a Luban system in the Luban host machine
# (WSL 2), from the release under test -- `luban status` says what this PC
# can run, a system is made, a command runs in it, it is described and
# removed; a machine is either started on the host machine's KVM or refused
# with its reason and next step (PLATFORM-CAPS, CARRIER-WSL2-LUBAN). And #724:
# a hook's downloaded program runs in the host machine, a failure there reaches
# this PC with its log, and an older guest is brought to this release.
#
# Needs WSL 2 (the CI lane enables it with Vampire/setup-wsl) and the Linux
# build of the same commit for the host machine: $env:XLINGS_CARRIER_GUEST_XLINGS
# names its bin/xlings, with bin/luban beside it.
# xtest: covers=PLATFORM-CAPS,CARRIER-WSL2-LUBAN,HOOK-RUNTIME-CONTRACT,CARRIER-RELAY-EVENTS,CARRIER-GUEST-CURRENT requires=windows,wsl2,network

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

# ── #724 (design 2026-11-21-issue-724) ──────────────────────────────
$distro = (& wsl.exe --list --quiet | Out-String) -split "`r?`n" | Where-Object { $_ -like 'xlings-*' } | Select-Object -First 1
if (-not $distro) { Fail "no host machine distribution after luban new" }
function Guest([string[]]$argv) { return Run (@('wsl.exe', '-d', $distro, '-u', 'root', '--exec') + $argv) }

# Its userland is at this release's revision: /lib64 is the home's runtime.
$lib64 = Guest @('/bin/busybox', 'readlink', '/lib64')
if ($lib64.Out.Trim() -ne '/xlings/abi/linux-x86_64-glibc') { Fail "the host machine's /lib64 is not the home's runtime: $($lib64.Out)" }

# A recipe whose install hook starts a program it was given -- PT_INTERP the
# standard path, not rewritten yet (bun's postinstall did this) -- installs there.
$fix = Join-Path $PKG_DIR 'hmfix'
New-Item -ItemType Directory -Force "$fix\pkgs\h" | Out-Null
Set-Content -Encoding ascii "$fix\xim-indexrepos.lua" 'xim_indexrepos = {}'
@'
package = {
    spec = "1", name = "hookrt", type = "package", archs = {"x86_64", "aarch64"},
    xpm = { linux = { deps = { runtime = { "xim:glibc" } }, ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.elfpatch")
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, "bin"))
    local program = path.join(dir, "bin", "hookrt-getconf")
    os.cp(path.join(pkginfo.dep_install_dir("glibc"), "bin", "getconf"), program)
    elfpatch.set_interpreter(program, "/lib64/ld-linux-x86-64.so.2")
    os.exec(program .. " PAGESIZE")
    return true
end
'@ | Set-Content -Encoding ascii "$fix\pkgs\h\hookrt.lua"
@'
package = {
    spec = "1", name = "hookfail", type = "package", archs = {"x86_64", "aarch64"},
    xpm = { linux = { ["1.0.0"] = {} } },
}
function install()
    print("hookfail: the line a failure's log holds")
    error("hookfail: failed on purpose")
end
'@ | Set-Content -Encoding ascii "$fix\pkgs\h\hookfail.lua"
$g = Guest @('/xlings/bin/xlings', '__carrier-grant', $fix, '/grant/hmfix', 'ro')
if ($g.Code -ne 0) { Fail "the fixture index was not granted to the host machine" }
$null = Guest @('/bin/busybox', 'cp', '-r', '/grant/hmfix', '/xlings/hmfix')
$null = Guest @('/xlings/bin/xlings', 'config', '--index-repo', 'hmfix:/xlings/hmfix')
$hookrt = Guest @('/xlings/bin/xlings', 'install', '-y', 'hmfix:hookrt@1.0.0')
if ($hookrt.Code -ne 0) { Fail "a hook's downloaded program did not run in the host machine" }

# Asked through the interface, a failure there comes back as its events: the
# guest's error, not "no error detail", its hook log on this PC.
$call = '{"argv":["new","x724f","--carrier","wsl2","--from","hmfix:hookfail@1.0.0","-y"]}'
$failed = Run @('xlings', 'interface', 'subos_command', '--args', $call)
if ($failed.Code -eq 0) { Fail "a failing hook in the host machine was reported as success" }
if ($failed.Out -match 'no error detail') { Fail "the host machine's failure came back without its detail: $($failed.Out)" }
if ($failed.Out -notmatch 'install hook failed') { Fail "the hook's failure is not among the events: $($failed.Out)" }
$fetched = Get-ChildItem "$PKG_DIR\logs\carriers\wsl2" -Recurse -File -Filter '*hookfail*' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $fetched) { Fail "the hook's log was not fetched into this home's logs\carriers\wsl2" }
# In the NDJSON its backslashes are escaped.
if (-not $failed.Out.Contains(($fetched.FullName | ConvertTo-Json).Trim('"'))) {
    Fail "the error does not name the fetched log $($fetched.FullName): $($failed.Out)"
}
if ((Get-Content $fetched.FullName -Raw) -notmatch 'the line a failure') { Fail "the fetched log is not the hook's" }
# Arguments reach the guest as they were given: quotes, spaces, not ASCII --
# the guest's own error repeats it.
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$odd = '--引号 "q" x'
$args1 = (@{ argv = @('config', 'box', $odd) } | ConvertTo-Json -Compress)
$echo = Run @('xlings', 'interface', 'subos_command', '--args', $args1)
if (-not $echo.Out.Contains('--引号 \"q\" x')) {
    Fail "an argument with quotes, spaces and non-ASCII did not reach the guest as given: $($echo.Out)"
}

# An older guest (the release before this one) and its userland at revision
# 1: the next use brings both to this release, from this PC's build.
$prev = Join-Path $PKG_DIR 'prev'
New-Item -ItemType Directory -Force $prev | Out-Null
$rel = (Invoke-RestMethod 'https://api.github.com/repos/openxlings/xlings/releases/latest').tag_name.TrimStart('v')
Invoke-WebRequest "https://github.com/openxlings/xlings/releases/download/v$rel/xlings-$rel-linux-x86_64.tar.gz" -OutFile "$prev\old.tar.gz"
tar -xzf "$prev\old.tar.gz" -C $prev --wildcards '*/bin/xlings'
$oldBin = Get-ChildItem $prev -Recurse -File -Filter xlings | Select-Object -First 1
$null = Guest @('/xlings/bin/xlings', '__carrier-grant', $oldBin.DirectoryName, '/grant/prev', 'ro')
$null = Guest @('/bin/busybox', 'sh', '-c', 'cp /grant/prev/xlings /xlings/bin/.old && mv /xlings/bin/.old /xlings/bin/xlings && rm -f /lib64 && echo ''{"revision":1}'' > /xlings/state/carrier-userland.json')
$oldV = Guest @('/xlings/bin/xlings', '--version')
Write-Host "the guest now reports: $($oldV.Out.Trim())"
(Get-Item "$PKG_DIR\carriers\wsl2\probe.json").LastWriteTime = (Get-Date).AddHours(-1)
$again = Run @('luban', 'run', 'box', '--', 'uname', '-s')
if ($again.Code -ne 0) { Fail "the next use after the guest went back failed: $($again.Out)" }
$mine = (Run @('xlings', '--version')).Out
$theirs = (Guest @('/xlings/bin/xlings', '--version')).Out
$v = [regex]::Match($mine, '\d{4}\.\d+\.\d+\.\d+').Value
if ($theirs -notmatch [regex]::Escape($v)) { Fail "the guest was not brought to $v`: $theirs" }
$lib64 = Guest @('/bin/busybox', 'readlink', '/lib64')
if ($lib64.Out.Trim() -ne '/xlings/abi/linux-x86_64-glibc') { Fail "the userland was not brought forward: $($lib64.Out)" }
$probe = Get-Content "$PKG_DIR\carriers\wsl2\probe.json" -Raw | ConvertFrom-Json
if (-not $probe.update.ok) { Fail "probe.json does not record the update: $($probe | ConvertTo-Json -Compress)" }

# Removed there; the host machine stays the home's.
$rm = Run @('luban', 'rm', 'box', '-y')
if ($rm.Code -ne 0) { Fail "luban rm box failed" }
# By name, from the data: with nothing left, the text suggests `luban new box`.
$after = Run @('luban', 'ls', '--json')
$listed = (($after.Out -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 1) | ConvertFrom-Json)
$listed = if ($listed.PSObject.Properties.Name -contains 'result') { $listed.result } else { $listed }
if ($listed.environments | Where-Object { $_.name -eq 'box' }) { Fail "box is still listed: $($after.Out)" }
if ($distro) { & wsl.exe --unregister $distro | Out-Null }
Write-Host "PASS: Luban OS on Windows -- status, a system in the Luban host machine, a command in it, removed; the machine as reported"
exit 0
