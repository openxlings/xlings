#Requires -Version 5.1
# E2E-01: Bootstrap Home (portable + installed)
# Verifies portable mode, move resilience, and self install from portable.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\release_test_lib.ps1"

$ROOT_DIR      = (Resolve-Path "$PSScriptRoot\..\..").Path
$RUNTIME_DIR   = Join-Path $ROOT_DIR 'tests\e2e\runtime\bootstrap_home'
$PORTABLE_DIR  = Join-Path $RUNTIME_DIR 'portable'
$MOVED_DIR     = Join-Path $RUNTIME_DIR 'portable_moved'
$INSTALL_USER  = Join-Path $RUNTIME_DIR 'install_user'
$FIXTURE_INDEX = Join-Path $ROOT_DIR 'tests\fixtures\xim-pkgindex'

$BIN_SRC = Find-XlingsBinary

function Fail($msg) { Write-Error "FAIL: $msg"; exit 1 }

if (-not (Test-Path $BIN_SRC)) { Fail "built xlings binary not found at $BIN_SRC" }

# Prepare fixture index (clone if not present)
if (-not (Test-Path "$FIXTURE_INDEX\pkgs")) {
    $fixtureRef = if ($env:XIM_PKGINDEX_REF) { $env:XIM_PKGINDEX_REF } else { 'xlings_0.4.0' }
    $fixtureUrl = if ($env:XIM_PKGINDEX_URL) { $env:XIM_PKGINDEX_URL } else { 'https://github.com/openxlings/xim-pkgindex.git' }
    if (Test-Path $FIXTURE_INDEX) { Remove-Item -Recurse -Force $FIXTURE_INDEX }
    New-Item -ItemType Directory -Force -Path (Split-Path $FIXTURE_INDEX) | Out-Null
    git clone --depth 1 --branch $fixtureRef $fixtureUrl $FIXTURE_INDEX
    if (-not (Test-Path "$FIXTURE_INDEX\pkgs")) { Fail "fixture index repo missing at $FIXTURE_INDEX" }
}

# ── Clean slate ─────────────────────────────────────────────────
if (Test-Path $RUNTIME_DIR) { Remove-Item -Recurse -Force $RUNTIME_DIR }
New-Item -ItemType Directory -Force -Path "$PORTABLE_DIR\bin" | Out-Null
New-Item -ItemType Directory -Force -Path $INSTALL_USER | Out-Null

if (-not (Test-Path $BIN_SRC -PathType Leaf)) {
    Fail "source binary is not a file: $BIN_SRC"
}
Copy-Item $BIN_SRC "$PORTABLE_DIR\bin\xlings.exe" -Force
# Assert the copy landed as a non-empty FILE. Without this the next failure is
# `.\bin\xlings.exe is not recognized`, which reads as "never built" and sends
# you looking at the build instead of at this line.
$copied = Get-Item "$PORTABLE_DIR\bin\xlings.exe" -ErrorAction SilentlyContinue
if (-not $copied -or $copied.PSIsContainer -or $copied.Length -eq 0) {
    Fail "copy did not produce a usable binary at $PORTABLE_DIR\bin\xlings.exe (src=$BIN_SRC)"
}

# Write bootstrap config
@{
    version     = '0.4.0'
    mirror      = 'GLOBAL'
    activeSubos = 'default'
    subos       = @{ default = @{ dir = '' } }
    index_repos = @(
        @{ name = 'projectrepo'; url = $FIXTURE_INDEX }
    )
} | ConvertTo-Json -Depth 10 | Set-Content "$PORTABLE_DIR\.xlings.json" -Encoding UTF8

# ── Portable mode test ──────────────────────────────────────────
Push-Location $PORTABLE_DIR
try {
    $env:XLINGS_HOME = $null
    Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue

    $configOut = & .\bin\xlings.exe config 2>&1 | Out-String
    if ($configOut -notmatch [regex]::Escape($PORTABLE_DIR)) {
        Fail "portable home was not auto-detected"
    }

    & .\bin\xlings.exe self init
    if ($LASTEXITCODE -ne 0) { Fail "self init failed in portable home" }

    $requiredDirs = @(
        'data\xpkgs',
        'data\runtimedir',
        'data\xim-index-repos',
        'data\local-indexrepo',
        'subos\default\bin',
        'subos\default\lib',
        'subos\default\usr',
        'subos\default\generations',
        'config\shell'
    )
    foreach ($d in $requiredDirs) {
        $full = Join-Path $PORTABLE_DIR $d
        if (-not (Test-Path $full)) { Fail "missing portable runtime dir: $d" }
    }

    # On Windows subos\current is a junction
    if (-not (Test-Path "$PORTABLE_DIR\subos\current")) { Fail "portable subos\current link missing" }
    if (-not (Test-Path "$PORTABLE_DIR\subos\default\bin\xlings.exe")) { Fail "portable builtin shim missing" }
} finally {
    Pop-Location
}

# ── Move test ───────────────────────────────────────────────────
Move-Item $PORTABLE_DIR $MOVED_DIR

Push-Location $MOVED_DIR
try {
    Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue

    $helpOut = & .\bin\xlings.exe -h 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Fail "portable home failed after move" }
    if ($helpOut -notmatch 'subos') { Fail "moved portable help output invalid" }

    & .\bin\xlings.exe update 2>&1 | Out-Null
    # update may exit non-zero if not a git repo, that's fine
} finally {
    Pop-Location
}

# ── Install test ────────────────────────────────────────────────
Push-Location $MOVED_DIR
try {
    $minPath = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    $env:XLINGS_HOME = $null
    Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue
    $origHome = $env:USERPROFILE
    $env:USERPROFILE = $INSTALL_USER
    $env:Path = $minPath
    try {
        & .\bin\xlings.exe self install
        if ($LASTEXITCODE -ne 0) { Fail "self install from portable home failed" }
    } finally {
        $env:USERPROFILE = $origHome
    }
} finally {
    Pop-Location
}

$INSTALLED_HOME = Join-Path $INSTALL_USER '.xlings'
if (-not (Test-Path "$INSTALLED_HOME\bin\xlings.exe")) { Fail "installed home missing bin\xlings.exe" }
if (-not (Test-Path "$INSTALLED_HOME\subos\default\bin\xlings.exe")) { Fail "installed home missing builtin shim" }

# Verify config from installed home
$installedPath = "$INSTALLED_HOME\subos\current\bin;$INSTALLED_HOME\bin;$env:SystemRoot\System32;$env:SystemRoot"
$origHome = $env:USERPROFILE
$env:USERPROFILE = $INSTALL_USER
Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue
$env:Path = $installedPath
try {
    $configOut = & "$INSTALLED_HOME\bin\xlings.exe" config 2>&1 | Out-String
    if ($configOut -notmatch [regex]::Escape($INSTALLED_HOME)) {
        Fail "installed home config output mismatch"
    }
} finally {
    $env:USERPROFILE = $origHome
}

Write-Host "PASS: bootstrap home works for portable and installed modes"

# Land an explicit success: this script checks for exit codes it expects to be
# non-zero, and `pwsh -command ". script.ps1"` returns whatever $LASTEXITCODE
# happens to hold as the step's exit code -- turning a passing test into a red
# job with "PASS" printed right above the failure.
exit 0
