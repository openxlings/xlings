# Temporary verification for issue #724 (not for merge): `luban new agent` on
# Windows fails in the xlings WSL carrier while installing xim:bun@1.3.11.
#
# Runs against the PUBLISHED release (what the reporter has), one phase per
# workflow step so a hung phase keeps the others' output. Everything printed
# is also written under $RUNNER_TEMP/x724/out for the artifact.
param([Parameter(Mandatory)][string]$Phase)

$ErrorActionPreference = 'Continue'
$PSNativeCommandArgumentPassing = 'Standard'
$env:WSL_UTF8 = '1'
$env:NO_COLOR = '1'
$env:XLINGS_NON_INTERACTIVE = '1'

$root = Join-Path $env:RUNNER_TEMP 'x724'
$out  = Join-Path $root 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$PROXY = 'socks5h://127.0.0.1:7890'

function Banner([string]$text) { Write-Host "`n===== $text =====" }

# Run a native command, show it, keep its output in $out\<file>, return its exit code.
function Cap([string]$file, [string]$label, [scriptblock]$cmd) {
    Banner $label
    $path = Join-Path $out $file
    & $cmd *>&1 | Tee-Object -Append -FilePath $path | ForEach-Object { Write-Host $_ }
    $rc = $LASTEXITCODE
    Write-Host "[exit $rc] $label"
    Add-Content (Join-Path $out 'exitcodes.txt') "$Phase`t$label`t$rc"
    return $rc
}

function Get-Distro {
    $names = (wsl.exe --list --quiet | Out-String) -replace "`0", '' -split "`r?`n"
    return ($names | Where-Object { $_ -like 'xlings-*' } | Select-Object -First 1)
}

# A shell script run as root in the carrier, handed over base64 so no quoting
# of wsl.exe's command line is involved.
function Cr([string]$file, [string]$label, [string]$script) {
    $distro = Get-Distro
    if (-not $distro) { Write-Host "no xlings-* distribution is registered"; return 99 }
    $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($script -replace "`r`n", "`n")))
    return Cap $file $label { wsl.exe -d $distro -u root --exec /bin/sh -c "echo $b64 | base64 -d | sh" }
}

$RELEASE_HOME = Join-Path $root 'home'
function Use-Home {
    $env:XLINGS_HOME = $RELEASE_HOME
    $env:Path = "$RELEASE_HOME\bin;$env:Path"
}

switch ($Phase) {

'setup' {
    # The downloaded release is untrusted data: its own directory, never run from.
    $zip = Get-ChildItem (Join-Path $root 'dl') -Filter 'xlings-*-windows-x86_64.zip' | Select-Object -First 1
    Expand-Archive $zip.FullName (Join-Path $root 'unzip') -Force
    $dir = Get-ChildItem (Join-Path $root 'unzip') -Directory | Select-Object -First 1
    Move-Item $dir.FullName $RELEASE_HOME
    Use-Home
    Add-Content $env:GITHUB_ENV "XLINGS_HOME=$RELEASE_HOME"
    Add-Content $env:GITHUB_PATH "$RELEASE_HOME\bin"
    Cap 'setup.txt' 'xlings --version' { xlings --version } | Out-Null
    Cap 'setup.txt' 'xlings self init' { xlings self init } | Out-Null
    Cap 'setup.txt' 'luban --version' { luban --version } | Out-Null
    Cap 'wsl-status.txt' 'wsl --status' { wsl.exe --status } | Out-Null
    Cap 'wsl-status.txt' 'wsl --version' { wsl.exe --version } | Out-Null
}

# X1: the report's own command, as typed (the proxy is the report's; nothing
# listens there on the runner, and it is only applied after the system is built).
'x1' {
    Use-Home
    Cap 'x1-luban-new.txt' 'X1 luban new agent --edition agent-workspace --proxy ...' {
        luban new agent --edition agent-workspace --proxy $PROXY -y } | Out-Null
    Cap 'x1-windows-logs.txt' 'X1 windows-side logs' {
        Get-ChildItem "$RELEASE_HOME\logs" -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
            "--- $($_.FullName)"; Get-Content $_.FullName -Tail 30 } } | Out-Null
}

# X2: what the carrier has (and does not have) after X1.
'x2' {
    Use-Home
    Cr 'x2-carrier-facts.txt' 'X2 loader, abi, bun payload, hook log' @'
export XLINGS_HOME=/xlings
echo "--- system loader paths"
ls -ld /lib /lib64 /usr/lib /usr/lib64 2>&1
ls -l /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-x86-64.so.2 2>&1
echo "--- /bin /usr/bin"
ls /bin | head -5; ls /bin | wc -l
echo "--- xlings abi"
ls -l /xlings/abi 2>&1
for d in /xlings/abi/*/; do echo "$d"; ls -l "$d" 2>&1 | head -15; done
echo "--- bun payload"
ls -la /xlings/data/xpkgs/xim-x-bun/ 2>&1
find /xlings/data/xpkgs/xim-x-bun -maxdepth 6 -name 'bun*' -path '*@oven*' 2>&1 | head
echo "--- hook logs"
find /xlings/logs -type f 2>&1 | head -30
for f in $(find /xlings/logs -type f -name '*bun*'); do echo "--- $f"; tail -n 60 "$f"; done
echo "--- run the downloaded bun directly, then through the abi loader"
BUN=$(find /xlings/data/xpkgs/xim-x-bun -path '*@oven/bun-linux-x64/bin/bun' 2>/dev/null | head -1)
echo "bun=$BUN"
if [ -n "$BUN" ]; then
  "$BUN" --version; echo "direct exit=$?"
  ABI=$(ls -d /xlings/abi/*/ 2>/dev/null | head -1)
  "${ABI}ld-linux-x86-64.so.2" --library-path "${ABI%/}" "$BUN" --version; echo "via abi loader exit=$?"
fi
'@ | Out-Null
}

# X3: what bun's own install script does (host side, nothing of xlings in it).
'x3' {
    $d = Join-Path $root 'bunpkg'
    New-Item -ItemType Directory -Force $d | Out-Null
    Push-Location $d
    Cap 'x3-bun-install-js.txt' 'X3 npm pack bun@1.3.11' { npm pack bun@1.3.11 --silent } | Out-Null
    Pop-Location
    $tgz = Get-ChildItem $d -Filter 'bun-*.tgz' | Select-Object -First 1
    $x = Join-Path $root 'bunpkg-x'
    New-Item -ItemType Directory -Force $x | Out-Null
    tar -xzf $tgz.FullName -C $x
    Cap 'x3-bun-install-js.txt' 'X3 package.json scripts' { Get-Content (Join-Path $x 'package/package.json') | Select-String '"(postinstall|bin|version)"' } | Out-Null
    Cap 'x3-bun-install-js.txt' 'X3 install.js: how it finds, runs and falls back' {
        Select-String -Path (Join-Path $x 'package/install.js') -Pattern 'renameSync|rename\(|"node_modules|node_modules/|spawnSync|execFileSync|--version|function requireBun|function installBun|process.cwd|__dirname|mkdtemp|npm install' |
            ForEach-Object { "{0,4}: {1}" -f $_.LineNumber, $_.Line.Trim() } } | Out-Null
}

# X5/X6/X7: the failure as three consumers see it, before anything is fixed.
'x5' {
    Use-Home
    Cr 'x5-carrier-cli-bun.txt' 'X5 plain xlings CLI in the carrier: install xim:bun@1.3.11' @'
export XLINGS_HOME=/xlings
/xlings/bin/xlings install xim:bun@1.3.11 -y > /tmp/x5.log 2>&1; rc=$?
tail -n 70 /tmp/x5.log
echo "rc=$rc"
'@ | Out-Null
}

'x6' {
    Use-Home
    Cr 'x6-carrier-interface.txt' 'X6 inside the carrier: interface subos_command (what the carrier answers)' @'
export XLINGS_HOME=/xlings
/xlings/bin/xlings interface subos_command --args '{"argv":["new","agent6","--from","luban:agent-workspace","--proxy","http://127.0.0.1:7890"]}' > /tmp/x6.ndjson 2>/tmp/x6.err; rc=$?
echo "rc=$rc  lines=$(wc -l < /tmp/x6.ndjson)  stderr-bytes=$(wc -c < /tmp/x6.err)"
echo "--- error events"
grep -i '"kind":"error"\|"code":"E_' /tmp/x6.ndjson | cut -c1-600
echo "--- last 12 events"
tail -n 12 /tmp/x6.ndjson | cut -c1-400
echo "--- stderr tail"
tail -n 20 /tmp/x6.err
'@ | Out-Null
}

'x7' {
    Use-Home
    $json = '{"argv":["new","agent7","--from","luban:agent-workspace","--carrier","wsl2","--proxy","http://127.0.0.1:7890"]}'
    Cap 'x7-host-interface.txt' 'X7 from Windows: xlings interface subos_command (relayed from the carrier)' {
        xlings interface subos_command --args $json } | Out-Null
    $lines = Get-Content (Join-Path $out 'x7-host-interface.txt')
    Banner 'X7 summary'
    "events: $($lines.Count)"
    "error events:"
    $lines | Select-String -Pattern '"kind":"error"|E_INTERNAL|no error detail|\[bun\]|hook failed' | ForEach-Object { $_.Line.Substring(0, [Math]::Min(500, $_.Line.Length)) }
}

# X4: the counterfactual. The one thing the carrier lacks, supplied: a system
# loader path (and the libraries it loads) that points at the abi family the
# home already has. Nothing else changes.
'x4' {
    Use-Home
    Cr 'x4-counterfactual.txt' 'X4 /lib64 -> the abi family, then install xim:bun@1.3.11 again' @'
export XLINGS_HOME=/xlings
FAM=$(ls -d /xlings/abi/*/ 2>/dev/null | head -1); FAM=${FAM%/}
echo "family=$FAM"
if [ -z "$FAM" ]; then echo "no abi family: cannot run the counterfactual"; exit 3; fi
rm -rf /lib64
ln -s "$FAM" /lib64
ls -ld /lib64; ls -l /lib64/ld-linux-x86-64.so.2
/xlings/bin/xlings install xim:bun@1.3.11 -y -v > /tmp/x4.log 2>&1; rc=$?
tail -n 40 /tmp/x4.log
echo "install rc=$rc"
/xlings/subos/current/bin/bun --version; echo "bun --version exit=$?"
'@ | Out-Null
}

# X8: the report's command again, with only that link added.
'x8' {
    Use-Home
    Cap 'x8-luban-new-fixed.txt' 'X8 luban new agent8 ... with /lib64 linked' {
        luban new agent8 --edition agent-workspace --proxy $PROXY -y -v } | Out-Null
}

# mk: the carrier itself, made the cheapest way (a tiny system on it).
'mk' {
    Use-Home
    Cap 'mk.txt' 'mk xlings subos new diag --from subos:luban-tiny --carrier wsl2' {
        xlings subos new diag --from subos:luban-tiny --carrier wsl2 -y } | Out-Null
    Cr 'mk.txt' 'mk carrier facts' @'
/xlings/bin/xlings --version
ls -ld /lib /lib64 /grant 2>&1
ls /sbin /bin | head -40
cat /proc/mounts | head -30
'@ | Out-Null
}

# W1: the guest's xlings handed over by the host -- a Windows directory
# mounted read-only in the guest (what a grant is), `self install` run from it.
# The guest is first set back to the previous release so a change is visible.
'w1' {
    Use-Home
    $script = @'
export XLINGS_HOME=/xlings XLINGS_NON_INTERACTIVE=1 NO_COLOR=1
echo "--- before"; /xlings/bin/xlings --version
mkdir -p /grant/old /grant/new
mount -t drvfs '@OLD@' /grant/old -o ro; echo "mount old rc=$?"
mount -t drvfs '@NEW@' /grant/new -o ro; echo "mount new rc=$?"
find /grant/old /grant/new -maxdepth 4 | head -30
old=$(find /grant/old -path '*/bin/xlings' | head -1); new=$(find /grant/new -path '*/bin/xlings' | head -1)
ls -l "$old" "$new"
echo "--- the previous release, run from the mount"; "$old" --version; echo "rc=$?"
echo "--- set the guest back: the previous release installs itself"
"$old" self install </dev/null; echo "self install (old) rc=$?"
/xlings/bin/xlings --version
echo "--- forward: this release installs itself from the mount"
"$new" self install </dev/null; echo "self install (new) rc=$?"
/xlings/bin/xlings --version
ls -l /xlings/bin | head -20
echo "--- the home still answers"
/xlings/bin/xlings subos list 2>&1 | head -10
umount /grant/old /grant/new 2>&1
'@
    $script = $script.Replace('@OLD@', $env:X724_OLD_GUEST_DIR).Replace('@NEW@', $env:X724_GUEST_DIR)
    Cr 'w1-guest-self-install.txt' 'W1 guest self install from a mounted Windows directory' $script | Out-Null
}

# W2: ending wsl.exe on the host -- what becomes of the process it started in
# the guest.
'w2' {
    Use-Home
    $distro = Get-Distro
    foreach ($how in 'kill', 'taskkill-tree') {
        $tag = if ($how -eq 'kill') { '611' } else { '612' }
        $p = Start-Process wsl.exe -ArgumentList @('-d', $distro, '-u', 'root', '--exec', '/bin/sh', '-c', "sleep $tag & sleep $tag; wait") -PassThru -WindowStyle Hidden
        Start-Sleep 5
        Cr 'w2-kill.txt' "W2 [$how] before: guest processes" "ps | grep 'sleep $tag' | grep -v grep; echo count=`$(ps | grep 'sleep $tag' | grep -v grep | wc -l)" | Out-Null
        if ($how -eq 'kill') { Stop-Process -Id $p.Id -Force } else { taskkill.exe /PID $p.Id /T /F | Out-Host }
        Start-Sleep 5
        Cr 'w2-kill.txt' "W2 [$how] after: guest processes" "ps | grep 'sleep $tag' | grep -v grep; echo count=`$(ps | grep 'sleep $tag' | grep -v grep | wc -l)" | Out-Null
    }
    # A terminal-attached child (what stream::run starts): stdin closed by the host.
    $psi = [Diagnostics.ProcessStartInfo]::new('wsl.exe', "-d $distro -u root --exec /bin/sh -c `"sleep 613; echo done`"")
    $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.UseShellExecute = $false
    $q = [Diagnostics.Process]::Start($psi)
    Start-Sleep 5
    $q.Kill()
    Start-Sleep 5
    Cr 'w2-kill.txt' 'W2 [piped, Kill()] after: guest processes' "ps | grep 'sleep 613' | grep -v grep; echo count=`$(ps | grep 'sleep 613' | grep -v grep | wc -l)" | Out-Null
}

# W3: in the carrier, what npm installs of bun without its lifecycle scripts.
'w3' {
    Use-Home
    Cr 'w3-npm-ignore-scripts.txt' 'W3 npm install --ignore-scripts bun@1.3.11 in the carrier' @'
export XLINGS_HOME=/xlings XLINGS_NON_INTERACTIVE=1 NO_COLOR=1 HOME=/root
export PATH=/xlings/bin:/xlings/subos/current/bin:/usr/bin:/bin:/usr/sbin:/sbin
/xlings/bin/xlings install -y xim:node 2>&1 | tail -5
which node npm; node --version
node -p 'process.report.getReport().header.glibcVersionRuntime'
rm -rf /root/b; mkdir -p /root/b; cd /root/b
npm install --prefix /root/b --ignore-scripts --no-fund --no-audit bun@1.3.11 2>&1 | tail -8; echo "npm rc=$?"
ls /root/b/node_modules/@oven/
ls -l /root/b/node_modules/bun/bin/
bun=/root/b/node_modules/@oven/bun-linux-x64/bin/bun
ls -l $bun; head -c 4096 $bun | tr -c '[:print:]' '\n' | grep -m1 'ld-'
echo "--- run it as it is (no standard loader here)"; $bun --version; echo "rc=$?"
echo "--- through the home's loader"; /xlings/abi/linux-x86_64-glibc/ld-linux-x86-64.so.2 $bun --version; echo "rc=$?"
echo "--- with the standard directory linked"; ln -s /xlings/abi/linux-x86_64-glibc /lib64; $bun --version; echo "rc=$?"; rm -f /lib64
'@ | Out-Null
}

'collect' {
    Use-Home
    Get-ChildItem "$RELEASE_HOME\logs" -Recurse -File -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $out ('winlog-' + $_.Name)) -ErrorAction SilentlyContinue }
    Cr 'carrier-logs.txt' 'carrier logs' @'
find /xlings/logs -type f | head -50
for f in $(find /xlings/logs -type f | head -40); do echo "--- $f"; tail -n 40 "$f" | cut -c1-400; done
'@ | Out-Null
    Banner 'exit codes'
    Get-Content (Join-Path $out 'exitcodes.txt') -ErrorAction SilentlyContinue
}

default { throw "unknown phase $Phase" }
}
