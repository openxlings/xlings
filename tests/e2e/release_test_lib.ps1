#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ROOT_DIR = (Resolve-Path "$PSScriptRoot\..\..").Path
$script:RUNTIME_ROOT = Join-Path $ROOT_DIR 'tests\e2e\runtime'
$script:FIXTURE_INDEX_DIR = Join-Path $ROOT_DIR 'tests\fixtures\xim-pkgindex'

function Log($msg) {
    Write-Host "[release-e2e] $msg"
}

function Fail($msg) {
    Write-Error "[release-e2e] FAIL: $msg"
    exit 1
}

function Get-MinimalSystemPath {
    $base = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    # Include Git usr/bin for tar, unzip, curl etc. (needed by xlings package extraction)
    $gitUsrBin = Join-Path $env:ProgramFiles 'Git\usr\bin'
    $gitCmd = Join-Path $env:ProgramFiles 'Git\cmd'
    if (Test-Path $gitUsrBin) { $base = "$gitUsrBin;$gitCmd;$base" }
    return $base
}

function Require-ReleaseArchive($path) {
    if (-not $path) { $path = Join-Path $ROOT_DIR 'build\release.zip' }
    if (-not (Test-Path $path)) { Fail "release archive not found: $path" }
    return $path
}

function Find-XlingsBinary {
    # `target\<triple>\<fingerprint>\bin\xlings.exe` -- the fingerprint changes
    # whenever a build input does (toolchain, mcpp version, .xlings.json), so a
    # run can leave MORE THAN ONE of these behind. `Select-Object -First 1` over
    # an unsorted Get-ChildItem then picks by directory-enumeration order, which
    # is not the newest and not stable. Sort by write time and take the newest.
    #
    # Everything here also insists on a FILE. Test-Path and Get-ChildItem both
    # match directories, and a directory reaches the caller as a path that
    # PowerShell reports as
    #
    #   The term '.\bin\xlings.exe' is not recognized as ... executable program
    #
    # which reads as "the binary was never built" rather than "we picked the
    # wrong thing" -- the same misdirection as an ENOENT that names the binary
    # instead of its missing loader.
    if ($env:XLINGS_BIN) {
        if (-not (Test-Path $env:XLINGS_BIN -PathType Leaf)) {
            Fail "XLINGS_BIN is set but is not a file: $env:XLINGS_BIN"
        }
        $p = (Resolve-Path $env:XLINGS_BIN).Path
        Write-Host "  [lib] xlings binary (XLINGS_BIN): $p"
        return $p
    }

    foreach ($glob in @("$ROOT_DIR\target\*\*\bin\xlings.exe",
                        "$ROOT_DIR\build\*\*\release\xlings.exe")) {
        $found = @(Get-ChildItem $glob -File -ErrorAction SilentlyContinue |
                   Sort-Object LastWriteTime -Descending)
        if ($found.Count -gt 0) {
            if ($found.Count -gt 1) {
                Write-Host "  [lib] $($found.Count) candidate binaries; taking the newest:"
                $found | ForEach-Object {
                    Write-Host ("        {0}  {1}" -f $_.LastWriteTime.ToString('s'), $_.FullName)
                }
            }
            Write-Host "  [lib] xlings binary: $($found[0].FullName)"
            return $found[0].FullName
        }
    }

    Fail "xlings.exe binary not found under target\ or build\; set XLINGS_BIN"
}

function Expand-ReleaseArchive($path, $name) {
    $extractRoot = Join-Path $RUNTIME_ROOT $name
    if (Test-Path $extractRoot) { Remove-Item -Recurse -Force $extractRoot }
    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
    Expand-Archive -Path $path -DestinationPath $extractRoot -Force

    $pkgDir = Get-ChildItem -Directory "$extractRoot\xlings-*-windows-x86_64" | Select-Object -First 1
    if (-not $pkgDir) { Fail "extracted release package dir not found under $extractRoot" }
    return $pkgDir.FullName
}

function Get-DefaultD2xVersion {
    return '0.1.1'
}

function Write-FixtureReleaseConfig($pkgDir) {
    $config = @{
        version     = '0.4.0'
        mirror      = 'GLOBAL'
        activeSubos = 'default'
        subos       = @{ default = @{ dir = '' } }
        index_repos = @(
            @{ name = 'xim'; url = $FIXTURE_INDEX_DIR }
        )
    }
    $config | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $pkgDir '.xlings.json') -Encoding UTF8
}

function Require-FixtureIndex {
    if (Test-Path "$FIXTURE_INDEX_DIR\pkgs") {
        Log "reuse existing fixture index: $FIXTURE_INDEX_DIR"
        return
    }
    $ref = if ($env:XIM_PKGINDEX_REF) { $env:XIM_PKGINDEX_REF } else { 'xlings_0.4.0' }
    $url = if ($env:XIM_PKGINDEX_URL) { $env:XIM_PKGINDEX_URL } else { 'https://github.com/openxlings/xim-pkgindex.git' }
    if (Test-Path $FIXTURE_INDEX_DIR) { Remove-Item -Recurse -Force $FIXTURE_INDEX_DIR }
    $parentDir = Split-Path $FIXTURE_INDEX_DIR
    New-Item -ItemType Directory -Force -Path $parentDir | Out-Null
    Log "cloning $url (ref: $ref) -> $FIXTURE_INDEX_DIR"
    git clone --depth 1 --branch $ref $url $FIXTURE_INDEX_DIR
    if (-not (Test-Path "$FIXTURE_INDEX_DIR\pkgs")) {
        Fail "fixture index repo missing at $FIXTURE_INDEX_DIR"
    }
}
