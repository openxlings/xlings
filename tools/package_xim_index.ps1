#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)]
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'

$mirror = if ($env:XLINGS_RELEASE_MIRROR) { $env:XLINGS_RELEASE_MIRROR } else { 'GLOBAL' }
$ref = if ($env:XLINGS_RELEASE_PKGINDEX_REF) { $env:XLINGS_RELEASE_PKGINDEX_REF } else { 'main' }

if ($env:XLINGS_RELEASE_PKGINDEX_URL) {
    $url = $env:XLINGS_RELEASE_PKGINDEX_URL
} elseif ($mirror -eq 'CN') {
    $url = 'https://gitee.com/sunrisepeak/xim-pkgindex.git'
} else {
    $url = 'https://github.com/openxlings/xim-pkgindex.git'
}

$tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) "xlings-pkgindex-$([System.Guid]::NewGuid().ToString('N').Substring(0,8))"
New-Item -ItemType Directory -Force -Path $tmpRoot | Out-Null

try {
    $cloneDir = Join-Path $tmpRoot 'xim-pkgindex'
    Write-Host "[release] Bundling xim-pkgindex snapshot: $url ($ref)"
    & git clone --depth 1 --branch $ref $url $cloneDir
    if ($LASTEXITCODE -ne 0) {
        if (Test-Path $cloneDir) { Remove-Item -Recurse -Force $cloneDir }
        & git clone $url $cloneDir
        if ($LASTEXITCODE -ne 0) { throw "git clone failed for $url" }
        & git -C $cloneDir checkout --quiet $ref
        if ($LASTEXITCODE -ne 0) { throw "git checkout failed for $ref" }
    }

    & git -C $cloneDir remote set-url origin $url
    if ($LASTEXITCODE -ne 0) { throw "git remote set-url failed for $url" }

    $dest = Join-Path $OutDir 'data\xim-pkgindex'
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
    Move-Item -LiteralPath $cloneDir -Destination $dest -Force

    if (-not (Test-Path "$dest\pkgs")) {
        throw "bundled xim-pkgindex missing pkgs/"
    }

    # Y-asset: make the bundled index artifact-managed (strip .git + version
    # marker). The runtime then refreshes it via `xlings update` from
    # xlings-res/xim-index instead of git pull. See src/core/xim/indexfetch.cppm.
    $configPath = Join-Path $PSScriptRoot '..\src\core\config.cppm'
    $ver = 'bundled'
    if (Test-Path $configPath) {
        $m = Select-String -Path $configPath -Pattern 'VERSION = "([^"]*)"' | Select-Object -First 1
        if ($m) { $ver = $m.Matches[0].Groups[1].Value }
    }
    Remove-Item -Recurse -Force (Join-Path $dest '.git') -ErrorAction SilentlyContinue
    Set-Content -NoNewline -Path (Join-Path $dest '.xlings-index-version') -Value $ver
    Write-Host "[release] bundled index is artifact-managed (version $ver, .git stripped)"
} finally {
    Remove-Item -Recurse -Force $tmpRoot -ErrorAction SilentlyContinue
}
