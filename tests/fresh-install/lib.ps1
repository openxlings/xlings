# Assertion helpers for the fresh-install smoke suites (Windows).
#
# Dot-sourced by smoke.ps1. Mirrors lib.sh one-for-one so the two platforms
# assert the same things; see lib.sh for the reasoning behind each helper.
#
# Written for both pwsh 7 and Windows PowerShell 5.1 — no `??`, no ternary — so
# a user reproducing a CI failure locally is not forced to install pwsh first.

$ErrorActionPreference = 'Stop'

function Write-Section { param([string]$Text) Write-Host "`n== $Text" -ForegroundColor Yellow }
function Write-Log     { param([string]$Text) Write-Host "   $Text" -ForegroundColor DarkGray }
function Write-Ok      { param([string]$Text) Write-Host "   OK  $Text" -ForegroundColor Green }

function Fail {
    param([string]$Text)
    Write-Host "`nFAIL $Text" -ForegroundColor Red
    exit 1
}

# Get-EnvOr — env var if set and non-empty, else the fallback.
function Get-EnvOr {
    param([string]$Name, [string]$Default)
    $v = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrEmpty($v)) { return $Default }
    return $v
}

# Invoke-Step — echo, run, and fail on a non-zero native exit code.
#
# The whole command is one array rather than exe-plus-remaining-arguments:
# PowerShell would try to bind a bare `-y` or `-g` as a parameter of THIS
# function and abort with "a parameter cannot be found that matches parameter
# name 'y'" before the real command ever ran.
#
# The $LASTEXITCODE check is not optional. PowerShell does not treat a non-zero
# exit from a native executable as a terminating error no matter what
# $ErrorActionPreference says, so an unchecked call fails silently.
function Invoke-Step {
    param([string]$Desc, [string[]]$Command)
    Write-Log "$ $($Command -join ' ')"
    $exe = $Command[0]
    $rest = @()
    if ($Command.Count -gt 1) { $rest = $Command[1..($Command.Count - 1)] }
    & $exe @rest
    if ($LASTEXITCODE -ne 0) {
        Fail "${Desc}: '$($Command -join ' ')' exited $LASTEXITCODE"
    }
}

# Write-TextFile — UTF-8 with NO byte-order mark.
#
# `Set-Content -Encoding utf8` emits a BOM on Windows PowerShell 5.1, and a BOM
# at the head of .xlings.json makes the JSON parse fail on a character the
# error message cannot show.
function Write-TextFile {
    param([string]$Path, [string]$Content)
    [System.IO.File]::WriteAllText($Path, $Content, (New-Object System.Text.UTF8Encoding($false)))
}

# Get-VersionFrom — first dotted numeric run in a block of text.
#   "gcc (GCC) 15.1.0"     -> 15.1.0
#   "clang version 20.1.7" -> 20.1.7
#   "mcpp 2026.7.29.1"     -> 2026.7.29.1
function Get-VersionFrom {
    param([string]$Text)
    $m = [regex]::Match($Text, '[0-9]+(\.[0-9]+)+')
    if ($m.Success) { return $m.Value }
    return ''
}

# Get-ToolVersion — the version <Tool> reports, or fail with its raw output.
function Get-ToolVersion {
    param([string]$Tool)
    if (-not (Get-Command $Tool -ErrorAction SilentlyContinue)) { Fail "'$Tool' is not on PATH" }
    $global:LASTEXITCODE = 0
    $out = (& $Tool --version 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { Fail "'$Tool --version' exited ${LASTEXITCODE}:`n$out" }
    $ver = Get-VersionFrom $out
    if (-not $ver) { Fail "'$Tool --version' printed no version number:`n$out" }
    return $ver
}

function Assert-ToolVersion {
    param([string]$Tool, [string]$Expected)
    $got = Get-ToolVersion $Tool
    if ($got -ne $Expected) { Fail "$Tool reports $got, expected $Expected" }
    Write-Ok "$Tool -> $got"
}

# Assert-Switch — switch to <Pkg>@<Version>, then assert EVERY listed command
# reports exactly that version.
#
# For a release group (gcc ships gcc/g++/c++ as one unit) this is the real
# assertion: a switch that moves `gcc` but strands `g++` passes any check that
# only looks at `gcc --version`. All members are probed before failing, so the
# error names every stranded member at once.
#
# Callers invoke this twice with two DIFFERENT versions — that is what makes it
# differential. A `use` that silently no-ops cannot satisfy both calls.
function Assert-Switch {
    param([string]$Pkg, [string]$Version, [string[]]$Tools)
    Invoke-Step "xlings use $Pkg@$Version" @('xlings', 'use', "$Pkg@$Version")

    $stranded = @()
    foreach ($t in $Tools) {
        $got = Get-ToolVersion $t
        if ($got -eq $Version) { Write-Log "  $t -> $got" }
        else { $stranded += "     $t reports $got (expected $Version)" }
    }
    if ($stranded.Count -gt 0) {
        Fail "$Pkg@${Version}: release group did not switch as a unit:`n$($stranded -join "`n")"
    }
    Write-Ok "$Pkg@$Version - [$($Tools -join ', ')] switched as a unit"
}

# Assert-GroupConsistent — assert every listed command reports the SAME
# version, without switching.
#
# For a single-version package there is nothing to switch between, but the
# group can still be mis-registered: wiring up `gcc` while leaving `g++`
# pointing at some other toolchain is the same defect Assert-Switch catches on
# platforms that do have two versions.
function Assert-GroupConsistent {
    param([string]$Label, [string[]]$Tools)
    $seen = @{}
    foreach ($t in $Tools) { $seen[$t] = Get-ToolVersion $t; Write-Log "  $t -> $($seen[$t])" }
    $distinct = $seen.Values | Sort-Object -Unique
    if ($distinct.Count -ne 1) {
        $detail = ($seen.GetEnumerator() | ForEach-Object { "     $($_.Key) reports $($_.Value)" }) -join "`n"
        Fail "${Label}: release group members disagree on version:`n$detail"
    }
    Write-Ok "$Label - [$($Tools -join ', ')] all report $($distinct[0])"
}

# Assert-Runs — execute a freshly compiled binary and check its output.
function Assert-Runs {
    param([string]$Desc, [string]$Path, [string]$Expected)
    if (-not (Test-Path $Path)) { Fail "${Desc}: '$Path' was not produced" }
    $global:LASTEXITCODE = 0
    $out = (& $Path 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { Fail "${Desc}: '$Path' exited $LASTEXITCODE" }
    if ($out -notlike "*$Expected*") {
        Fail "${Desc}: '$Path' printed '$out', expected it to contain '$Expected'"
    }
    Write-Ok "$Desc -> $out"
}

# New-TempDir — a fresh empty directory (New-TemporaryFile makes a *file*).
function New-TempDir {
    $p = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Path $p -Force | Out-Null
    return $p
}
