#Requires -Version 5.1
# E2E test: verify that shims recover project context via XLINGS_PROJECT_DIR
# when CWD is outside the project directory.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\release_test_lib.ps1"

$SCENARIO_DIR = Join-Path $ROOT_DIR 'tests\e2e\scenarios\local_repo'
$HOME_DIR     = Join-Path $RUNTIME_ROOT 'shim_project_context_home'
$PROJECT_BIN  = Join-Path $SCENARIO_DIR '.xlings\subos\_\bin'
$PROJECT_INDEX_DIR = Join-Path $ROOT_DIR 'tests\e2e\fixtures\project_index'

# --- Find xlings binary ---
$XLINGS_BIN = Find-XlingsBinary

# --- Backup scenario config ---
$BACKUP_FILE = [System.IO.Path]::GetTempFileName()
Copy-Item "$SCENARIO_DIR\.xlings.json" $BACKUP_FILE

function Cleanup {
    if (Test-Path "$SCENARIO_DIR\.xlings") { Remove-Item -Recurse -Force "$SCENARIO_DIR\.xlings" -ErrorAction SilentlyContinue }
    if (Test-Path $HOME_DIR) { Remove-Item -Recurse -Force $HOME_DIR -ErrorAction SilentlyContinue }
    if (Test-Path $BACKUP_FILE) {
        Copy-Item $BACKUP_FILE "$SCENARIO_DIR\.xlings.json" -ErrorAction SilentlyContinue
        Remove-Item $BACKUP_FILE -ErrorAction SilentlyContinue
    }
}

try {

# --- Setup isolated XLINGS_HOME ---
if (Test-Path $HOME_DIR) { Remove-Item -Recurse -Force $HOME_DIR }
if (Test-Path "$SCENARIO_DIR\.xlings") { Remove-Item -Recurse -Force "$SCENARIO_DIR\.xlings" }
New-Item -ItemType Directory -Force -Path $HOME_DIR | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $HOME_DIR 'bin') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $HOME_DIR 'subos\default\bin') | Out-Null

# Copy xlings binary
Copy-Item $XLINGS_BIN (Join-Path $HOME_DIR 'bin\xlings.exe')

# Write home config
$homeConfig = @{
    mirror      = 'GLOBAL'
    index_repos = @(
        @{ name = 'xim'; url = $PROJECT_INDEX_DIR }
    )
}
$homeConfig | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $HOME_DIR '.xlings.json') -Encoding UTF8

# --- Install node in project context ---
$env:XLINGS_HOME = $HOME_DIR
Push-Location $SCENARIO_DIR
try {
    & $XLINGS_BIN update
    if ($LASTEXITCODE -ne 0) { Fail "xlings update failed" }

    & $XLINGS_BIN -y install
    if ($LASTEXITCODE -ne 0) { Fail "xlings install failed" }
} finally {
    Pop-Location
    Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue
}

# --- Verify: project subos bin has node shim ---
if (-not (Test-Path "$PROJECT_BIN\node.exe")) { Fail "project node shim missing after install" }

# --- Test 1: With XLINGS_PROJECT_DIR, shim works from outside project ---
$OUTSIDE_DIR = Join-Path $env:TEMP "shim_ctx_test_$(Get-Random)"
New-Item -ItemType Directory -Force -Path $OUTSIDE_DIR | Out-Null

$env:XLINGS_HOME = $HOME_DIR
$env:XLINGS_PROJECT_DIR = $SCENARIO_DIR
Push-Location $OUTSIDE_DIR
try {
    $nodeVer = & "$PROJECT_BIN\node.exe" --version 2>&1 | Out-String
    $nodeVer = $nodeVer.Trim()
} finally {
    Pop-Location
    Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue
    Remove-Item Env:XLINGS_PROJECT_DIR -ErrorAction SilentlyContinue
}

Log "node --version (with XLINGS_PROJECT_DIR): $nodeVer"
if ($nodeVer -ne "v22.17.1") { Fail "shim with XLINGS_PROJECT_DIR did not resolve expected version (got: $nodeVer)" }

# --- Test 2: Without XLINGS_PROJECT_DIR, shim fails from outside project ---
$env:XLINGS_HOME = $HOME_DIR
Remove-Item Env:XLINGS_PROJECT_DIR -ErrorAction SilentlyContinue
Push-Location $OUTSIDE_DIR
try {
    $nodeErr = & "$PROJECT_BIN\node.exe" --version 2>&1 | Out-String
    $nodeErr = $nodeErr.Trim()
    $nodeRc = $LASTEXITCODE
} finally {
    Pop-Location
    Remove-Item Env:XLINGS_HOME -ErrorAction SilentlyContinue
}

Log "node --version (without XLINGS_PROJECT_DIR): $nodeErr"
if ($nodeRc -eq 0) {
    Fail "shim without XLINGS_PROJECT_DIR should have failed"
}
if ($nodeErr -notmatch "xlings:") {
    Fail "expected xlings shim error without XLINGS_PROJECT_DIR (got: $nodeErr)"
}

# Cleanup temp dir
Remove-Item -Recurse -Force $OUTSIDE_DIR -ErrorAction SilentlyContinue

Log "PASS: shim project context via XLINGS_PROJECT_DIR"

# Reset exit code so script exits 0 on success
$host.SetShouldExit(0)

} finally {
    Cleanup
}

# Land an explicit success: this script checks for exit codes it expects to be
# non-zero, and `pwsh -command ". script.ps1"` returns whatever $LASTEXITCODE
# happens to hold as the step's exit code -- turning a passing test into a red
# job with "PASS" printed right above the failure.
exit 0
