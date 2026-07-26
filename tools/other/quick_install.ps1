# One-line installer for xlings (Windows).
#   powershell -ExecutionPolicy Bypass -c "irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex"
#Requires -Version 5

param(
    [string]$Version = $env:XLINGS_VERSION
)

$ErrorActionPreference = "Stop"

# --------------- helpers ---------------

function Log-Info  { param([string]$Msg) Write-Host "[xlings]: $Msg" -ForegroundColor Green }
function Log-Warn  { param([string]$Msg) Write-Host "[xlings]: $Msg" -ForegroundColor Yellow }
function Log-Error { param([string]$Msg) Write-Host "[xlings]: $Msg" -ForegroundColor Red }

function Ensure-XlingsProfile {
    param(
        [string]$ShellExe,
        [string]$SourceLine,
        [string]$Label
    )
    $prof = & $ShellExe -NoProfile -Command '$PROFILE' 2>$null
    if (-not $prof) { return }
    $dir = Split-Path $prof -Parent
    if (!(Test-Path $dir)) {
        New-Item -ItemType Directory -Force $dir | Out-Null
    }
    if (!(Test-Path $prof)) {
        New-Item -ItemType File -Force $prof | Out-Null
    }
    $existing = Get-Content $prof -Raw -ErrorAction SilentlyContinue
    if (!$existing -or $existing -notlike '*xlings-profile*') {
        Add-Content $prof $SourceLine
        Log-Info "$Label profile configured"
    } else {
        Log-Info "$Label profile already set"
    }
}

$OFFICIAL_REPO = "openxlings/xlings"
$RESOURCE_REPO = "xlings-res/xlings"
$GITHUB_MIRROR = $env:XLINGS_GITHUB_MIRROR
$GITHUB_API = "https://api.github.com"
$GITCODE_API = "https://api.gitcode.com/api/v5"

# --------------- banner ---------------

Write-Host @"

 __   __  _      _
 \ \ / / | |    (_)
  \ V /  | |     _  _ __    __ _  ___
   > <   | |    | || '_ \  / _  |/ __|
  / . \  | |____| || | | || (_| |\__ \
 /_/ \_\ |______|_||_| |_| \__, ||___/
                            __/ |
                           |___/

repo:  https://github.com/openxlings/xlings
forum: https://forum.d2learn.org

"@ -ForegroundColor Cyan

# --------------- detect architecture ---------------

$arch = if ([System.Environment]::Is64BitOperatingSystem) { "x86_64" } else { "x86" }
# ARM64 detection (Windows 11+)
if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64" -or $env:PROCESSOR_ARCHITEW6432 -eq "ARM64") {
    $arch = "arm64"
}

# --------------- resolve download URL ---------------

function Invoke-JsonGet {
    param([string]$Url)

    $response = Invoke-WebRequest -Uri $Url -UseBasicParsing -UserAgent "xlings-quick-install" -ErrorAction Stop
    return $response.Content | ConvertFrom-Json
}

function Get-VersionNumber {
    param([string]$Tag)

    if (-not $Tag) { return $null }
    return (($Tag -replace '^refs/tags/', '') -replace '^v', '')
}

function Get-VersionSortKey {
    param([string]$VersionNum)

    try {
        return [version]$VersionNum
    } catch {
        return [version]"0.0"
    }
}

function Get-TagCandidates {
    param(
        [string]$RequestedVersion,
        [bool]$PreferVTag
    )

    $versionNum = Get-VersionNumber $RequestedVersion
    if (-not $versionNum) { return @() }

    if ($PreferVTag) {
        $candidates = @("v$versionNum", $versionNum)
    } else {
        $candidates = @($versionNum, "v$versionNum")
    }

    $uniqueCandidates = $candidates | Select-Object -Unique
    return $uniqueCandidates
}

function Convert-GitCodeAssetUrl {
    param([string]$Url)

    # GitCode release metadata currently reports attach browser_download_url on
    # api.gitcode.com, but direct downloads from that host return 404. The same
    # /releases/download/... path on gitcode.com redirects to file-cdn.gitcode.com.
    if ($Url -like "https://api.gitcode.com/*/releases/download/*") {
        return $Url.Replace("https://api.gitcode.com/", "https://gitcode.com/")
    }

    return $Url
}

function Get-ReleaseAsset {
    param(
        [object]$Release,
        [string]$AssetName
    )

    foreach ($asset in @($Release.assets)) {
        if ($asset.name -eq $AssetName -and $asset.browser_download_url) {
            return Convert-GitCodeAssetUrl ([string]$asset.browser_download_url)
        }
    }

    return $null
}

function Resolve-ReleaseCandidates {
    $githubWebBase = if ($GITHUB_MIRROR) { $GITHUB_MIRROR.TrimEnd([char[]]"/") } else { "https://github.com" }

    # Probe only the two supported Windows package sources. GitCode is the
    # low-latency resource mirror for users who have trouble reaching GitHub.
    @(
        [pscustomobject]@{
            Name       = "GitHub openxlings/xlings"
            Repo       = $OFFICIAL_REPO
            ApiBase    = $GITHUB_API
            WebBase    = $githubWebBase
            PreferVTag = $true
        },
        [pscustomobject]@{
            Name       = "GitCode xlings-res/xlings"
            Repo       = $RESOURCE_REPO
            ApiBase    = $GITCODE_API
            WebBase    = $null
            PreferVTag = $false
        }
    )
}

function Get-ReleaseMetadata {
    param(
        [object]$Source,
        [string]$RequestedVersion
    )

    if ($RequestedVersion) {
        $lastError = $null
        foreach ($tag in (Get-TagCandidates -RequestedVersion $RequestedVersion -PreferVTag $Source.PreferVTag)) {
            try {
                return Invoke-JsonGet "$($Source.ApiBase)/repos/$($Source.Repo)/releases/tags/$tag"
            } catch {
                $lastError = $_.Exception.Message
            }
        }
        throw "release '$RequestedVersion' not found on $($Source.Name): $lastError"
    }

    return Invoke-JsonGet "$($Source.ApiBase)/repos/$($Source.Repo)/releases/latest"
}

function Measure-ReleaseLatency {
    param(
        [object]$Source,
        [string]$RequestedVersion
    )

    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $release = Get-ReleaseMetadata -Source $Source -RequestedVersion $RequestedVersion
        $timer.Stop()
        return [pscustomobject]@{
            Release = $release
            ProbeMs = [int]$timer.ElapsedMilliseconds
        }
    } catch {
        $timer.Stop()
        throw $_
    }
}

function Resolve-SourceCandidate {
    param(
        [object]$Source,
        [string]$RequestedVersion,
        [string]$PackageArch
    )

    $probe = Measure-ReleaseLatency -Source $Source -RequestedVersion $RequestedVersion
    $release = $probe.Release
    $tag = [string]$release.tag_name
    if (-not $tag) {
        throw "release metadata from $($Source.Name) does not include tag_name"
    }

    $versionNum = Get-VersionNumber $tag
    $zipName = "xlings-$versionNum-windows-$PackageArch.zip"
    $assetUrl = Get-ReleaseAsset -Release $release -AssetName $zipName
    if (-not $assetUrl) {
        throw "asset '$zipName' not found in $($Source.Name) release '$tag'"
    }

    if ($Source.Repo -eq $OFFICIAL_REPO -and $GITHUB_MIRROR) {
        $assetUrl = "$($Source.WebBase)/$($Source.Repo)/releases/download/$tag/$zipName"
    }

    [pscustomobject]@{
        Source  = $Source.Name
        Tag     = $tag
        Version = $versionNum
        VersionKey = (Get-VersionSortKey $versionNum)
        ProbeMs = $probe.ProbeMs
        ZipName = $zipName
        Url     = $assetUrl
    }
}

function Sort-ReleaseCandidates {
    param(
        [object[]]$Candidates,
        [string]$RequestedVersion
    )

    if (-not $Candidates -or $Candidates.Count -eq 0) { return @() }

    $eligible = @($Candidates)
    if (-not $RequestedVersion) {
        $latest = $eligible | Sort-Object -Property VersionKey -Descending | Select-Object -First 1
        $eligible = @($eligible | Where-Object { $_.VersionKey -eq $latest.VersionKey })
    }

    return @($eligible | Sort-Object -Property ProbeMs)
}

function Test-ZipPackage {
    param([string]$Path)

    if (-not (Test-Path $Path)) { return $false }
    $file = Get-Item $Path
    if ($file.Length -lt 4) { return $false }

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $first = $stream.ReadByte()
        $second = $stream.ReadByte()
        return ($first -eq 0x50 -and $second -eq 0x4b)
    } finally {
        $stream.Dispose()
    }
}

function Download-ReleasePackage {
    param(
        [object[]]$Sources,
        [string]$RequestedVersion,
        [string]$PackageArch,
        [string]$TempDir
    )

    $lastError = $null
    $candidates = @()

    foreach ($source in $Sources) {
        try {
            Log-Info "Probing $($source.Name) latency..."
            $candidate = Resolve-SourceCandidate -Source $source -RequestedVersion $RequestedVersion -PackageArch $PackageArch
            Log-Info "Available: $($candidate.Source) $($candidate.Tag) ($($candidate.ProbeMs)ms)"
            $candidates += $candidate
        } catch {
            $lastError = $_.Exception.Message
            Log-Warn "$($source.Name) unavailable: $lastError"
        }
    }

    $orderedCandidates = Sort-ReleaseCandidates -Candidates $candidates -RequestedVersion $RequestedVersion
    if (-not $orderedCandidates -or $orderedCandidates.Count -eq 0) {
        throw "all Windows release sources failed. Last error: $lastError"
    }

    if (-not $RequestedVersion) {
        Log-Info "Selected release version: $($orderedCandidates[0].Tag)"
    }

    foreach ($candidate in $orderedCandidates) {
        $zipPath = $null

        try {
            $zipPath = Join-Path $TempDir $candidate.ZipName

            Log-Info "Version:      $($candidate.Tag)"
            Log-Info "Package:      $($candidate.ZipName)"
            Log-Info "Source:       $($candidate.Source) ($($candidate.ProbeMs)ms)"
            Log-Info "Download URL: $($candidate.Url)"
            Log-Info "Downloading..."

            $progressPref = $ProgressPreference
            $ProgressPreference = 'SilentlyContinue'
            try {
                Invoke-WebRequest -Uri $candidate.Url -OutFile $zipPath -UseBasicParsing -UserAgent "xlings-quick-install" -ErrorAction Stop
            } finally {
                $ProgressPreference = $progressPref
            }

            if (-not (Test-ZipPackage $zipPath)) {
                throw "downloaded file is not a zip package"
            }

            return [pscustomobject]@{
                Candidate = $candidate
                ZipPath   = $zipPath
            }
        } catch {
            $lastError = $_.Exception.Message
            Log-Warn "$($candidate.Source) download failed: $lastError"
            if ($zipPath -and (Test-Path $zipPath)) {
                Remove-Item -Force $zipPath -ErrorAction SilentlyContinue
            }
        }
    }

    throw "all Windows release sources failed. Last error: $lastError"
}

# --------------- download & extract ---------------

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) "xlings-install-$([System.Guid]::NewGuid().ToString('N').Substring(0,8))"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

try {
    if ($Version) {
        Log-Info "Using specified version: $Version"
    } else {
        Log-Info "No version specified; probing release source latency..."
    }

    $download = Download-ReleasePackage -Sources (Resolve-ReleaseCandidates) -RequestedVersion $Version -PackageArch $arch -TempDir $tempDir
    $zipPath = $download.ZipPath
    $selected = $download.Candidate
    Log-Info "Selected source: $($selected.Source)"

    Log-Info "Extracting..."
    Expand-Archive -Path $zipPath -DestinationPath $tempDir -Force

    $extractDir = Get-ChildItem -Path $tempDir -Directory -Filter "xlings-*" | Select-Object -First 1
    $xlingsBin = if ($extractDir) { Join-Path $extractDir.FullName "bin\xlings.exe" } else { $null }
    if (-not $extractDir -or -not (Test-Path $xlingsBin)) {
        throw "extracted package is invalid (missing bin\xlings.exe)"
    }

    Log-Info "Running installer..."
    Push-Location $extractDir.FullName
    try {
        & $xlingsBin self install
        if ($LASTEXITCODE -ne 0) {
            throw "xlings self install failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }

    try {
        $xlHome = [System.Environment]::GetEnvironmentVariable('XLINGS_HOME', 'User')
        if ($xlHome) {
            $sourceLine = "`n# xlings`nif(Test-Path '$xlHome\config\shell\xlings-profile.ps1'){. '$xlHome\config\shell\xlings-profile.ps1'}"

            Ensure-XlingsProfile -ShellExe powershell -SourceLine $sourceLine -Label "Windows PowerShell"

            $pwshCmd = Get-Command pwsh -ErrorAction SilentlyContinue
            if ($pwshCmd) {
                Ensure-XlingsProfile -ShellExe pwsh -SourceLine $sourceLine -Label "pwsh 7+"
            }
        }
    } catch {
        Log-Warn "shell profile setup skipped: $($_.Exception.Message)"
    }
} catch {
    Log-Error "Installation failed: $($_.Exception.Message)"
    throw
} finally {
    Log-Info "Cleaning up temporary files..."
    Remove-Item -Recurse -Force $tempDir -ErrorAction SilentlyContinue
}
