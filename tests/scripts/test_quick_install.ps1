# The Windows quick installer, verified by RUNNING it.
#
# Its predecessor asserted that quick_install.ps1 CONTAINS the strings
# "Get-FileHash" and "checksum sidecar is missing or malformed" -- and it
# replaced the CI step that used to do `irm ... | iex` for real, so the path
# most Windows users take lost its executable coverage at the same moment a
# hard failure mode (a missing sidecar aborts the install) was added to it.
#
# Here the installer runs end to end against a local HTTP listener holding a
# real zip: source selection, download, checksum verification, extraction.
# Same code path a user reaches; only the host differs.
#
# What is asserted is HOW FAR IT GETS, because the checksum gate sits between
# download and extraction:
#   good sidecar     -> reaches extraction ("Extracting...")
#   tampered sidecar -> stops at the checksum, never extracts
#   missing sidecar  -> stops at the checksum, never extracts
#
# The final `self install` needs a real PE, which a test fixture cannot
# fabricate, so the run is expected to fail THERE and nowhere earlier. That is
# the line the checksum gate has to be on the correct side of.
$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$script = Join-Path $root "tools\other\quick_install.ps1"
$version = "9999.0.0.1"

# Parses? A syntax error would otherwise surface as "all sources failed".
$null = [System.Management.Automation.Language.Parser]::ParseFile(
    $script, [ref]$null, [ref]$null)

$arch = if ([System.Environment]::Is64BitOperatingSystem) { "x86_64" } else { "x86" }
if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64" -or $env:PROCESSOR_ARCHITEW6432 -eq "ARM64") {
    $arch = "arm64"
}
if ($arch -ne "x86_64") {
    Write-Host "quick_install: no release target for windows-$arch, skipped"
    exit 0
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("xlings-qi-" + [System.Guid]::NewGuid().ToString("N"))
$serve = Join-Path $work "serve"
$stage = Join-Path $work "stage"
$pkg = Join-Path $stage "xlings-$version"
$fakeHome = Join-Path $work "fakehome"
New-Item -ItemType Directory -Force -Path (Join-Path $pkg "bin"), $serve, $fakeHome | Out-Null

# Enough of a package for the installer's own validity check to pass. It is
# not a runnable PE, so `self install` is expected to fail -- see the header.
[System.IO.File]::WriteAllBytes((Join-Path $pkg "bin\xlings.exe"), [byte[]]@(0x4D, 0x5A))

$zipName = "xlings-$version-windows-$arch.zip"
$zipPath = Join-Path $serve $zipName
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath)
$goodHash = (Get-FileHash -Algorithm SHA256 -Path $zipPath).Hash.ToLowerInvariant()
Set-Content -Path "$zipPath.sha256" -Value "$goodHash  $zipName" -Encoding ascii

$port = 22000 + (Get-Random -Maximum 20000)
$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://127.0.0.1:$port/")
$listener.Start()

$serveScript = {
    param($listener, $serve)
    while ($listener.IsListening) {
        try { $context = $listener.GetContext() } catch { break }
        try {
            $name = [System.IO.Path]::GetFileName($context.Request.Url.LocalPath)
            $path = Join-Path $serve $name
            if (Test-Path $path) {
                $bytes = [System.IO.File]::ReadAllBytes($path)
                $context.Response.ContentLength64 = $bytes.Length
                $context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
            } else {
                $context.Response.StatusCode = 404
            }
        } catch { }
        $context.Response.Close()
    }
}
$serveJob = Start-ThreadJob -ScriptBlock $serveScript -ArgumentList $listener, $serve

function Invoke-Installer {
    $env:XLINGS_HOME = Join-Path $work "home"
    $env:USERPROFILE = $fakeHome
    $env:XLINGS_NON_INTERACTIVE = "1"
    $env:NO_COLOR = "1"
    $env:XLINGS_BASE_URL = "http://127.0.0.1:$port"
    $output = & powershell -NoLogo -NonInteractive -ExecutionPolicy Bypass `
        -File $script $version 2>&1 | Out-String
    return $output
}

function Assert-StoppedAtChecksum {
    param([string]$Output, [string]$What)
    if ($Output -match "Extracting\.\.\.") {
        Write-Host $Output
        throw "$What was accepted -- extraction ran past the checksum gate"
    }
    if ($Output -notmatch "checksum|SHA256") {
        Write-Host $Output
        throw "$What did not produce a checksum diagnostic"
    }
}

try {
    # --- a good archive gets past verification and into extraction ---------
    $output = Invoke-Installer
    if ($output -notmatch "Extracting\.\.\.") {
        Write-Host $output
        throw "a valid archive never reached extraction"
    }
    if ($output -match "checksum mismatch") {
        Write-Host $output
        throw "a matching checksum was reported as a mismatch"
    }
    if ($output -match "$([char]27)\[") {
        throw "NO_COLOR=1 output still carries ANSI escapes"
    }

    # --- a tampered sidecar must stop it -----------------------------------
    $first = if ($goodHash[0] -ne '0') { '0' } else { '1' }
    $flipped = $first + $goodHash.Substring(1)
    Set-Content -Path "$zipPath.sha256" -Value "$flipped  $zipName" -Encoding ascii
    Assert-StoppedAtChecksum -Output (Invoke-Installer) -What "a checksum mismatch"

    # --- a missing sidecar must stop it, not be skipped --------------------
    Remove-Item -Force "$zipPath.sha256"
    $output = Invoke-Installer
    if ($output -match "Extracting\.\.\.") {
        Write-Host $output
        throw "a missing sidecar was treated as 'nothing to verify'"
    }

    Write-Host "quick_install.ps1: ok (verified; tampered and missing sidecars both rejected)"
} finally {
    $listener.Stop()
    $listener.Close()
    if ($serveJob) { Remove-Job -Job $serveJob -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
