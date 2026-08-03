# Fresh-install smoke suites — Windows.
#
# Verifies RELEASED xlings (floating latest, exactly what a new user gets) on a
# machine with no xlings state, from `quick_install.ps1` through the mainstream
# feature set. It does NOT test the code in the working tree — see
# xlings-ci-windows.yml for that.
#
#   usage: pwsh -File tests/fresh-install/smoke.ps1 -Suite <core|gcc|llvm>
#
# Note the `gcc` suite cannot test a version SWITCH on Windows: the package
# index ships exactly one Windows gcc (15.1.0, via mingw64). It asserts group
# registration and mutual consistency instead. Real group switching is covered
# by the Linux and CentOS 7 legs.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('core', 'gcc', 'llvm')]
    [string]$Suite
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')

# ── Versions under test ───────────────────────────────────────────────
# Pinned rather than `latest` so an assertion can name an exact expected
# string. Two distinct versions per package, because the switch assertions are
# differential: one version alone would pass even if `use` did nothing.
# Overridable so a failure can be re-run against other versions locally.
$QuickInstallUrl = Get-EnvOr 'QUICK_INSTALL_URL' 'https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1'
$NinjaVersion    = Get-EnvOr 'NINJA_VERSION' '1.12.1'
$McppOld         = Get-EnvOr 'MCPP_OLD'      '2026.7.28.2'
$McppNew         = Get-EnvOr 'MCPP_NEW'      '2026.7.29.1'
$MingwVersion    = Get-EnvOr 'MINGW_VERSION' '13.0.0'
$LlvmOld         = Get-EnvOr 'LLVM_OLD'      '20.1.7'
$LlvmNew         = Get-EnvOr 'LLVM_NEW'      '22.1.8'

$XlingsHome = Join-Path $env:USERPROFILE '.xlings'

# ── Phase 0: bootstrap ────────────────────────────────────────────────

function Invoke-Bootstrap {
    Write-Section 'Bootstrap - quick_install on a cold machine'

    if (Test-Path $XlingsHome) {
        Fail "not a fresh environment: $XlingsHome already exists"
    }

    # No version argument: resolve to the newest release, which is the whole
    # point - this is the command printed in the README.
    #
    # ErrorActionPreference is relaxed to 'Continue' for exactly this call so
    # the installer runs under the same conditions a user gets from a bare
    # `irm ... | iex`. Under 'Stop' any non-terminating Write-Error inside the
    # installer would abort the run and report a failure the user would never
    # actually see.
    Write-Log "$ irm $QuickInstallUrl | iex"
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { Invoke-RestMethod $QuickInstallUrl | Invoke-Expression }
    finally { $ErrorActionPreference = $prevEap }

    $shim = Join-Path $XlingsHome 'subos\current\bin\xlings.exe'
    if (-not (Test-Path $shim)) {
        Fail "quick_install left no xlings shim at $shim - the published release is unusable"
    }

    # The installer writes profile hooks, but this process started before they
    # existed; put the same directories on PATH by hand.
    $env:PATH = "$XlingsHome\subos\current\bin;$XlingsHome\bin;$env:PATH"

    if (-not (Get-Command xlings -ErrorAction SilentlyContinue)) {
        Fail 'xlings is not on PATH after quick_install'
    }
    Write-Ok "xlings $(Get-ToolVersion xlings) installed at $XlingsHome"

    # Runners reach github.com but not the gitee/gitcode endpoints, so force
    # GLOBAL rather than letting region detection guess.
    Invoke-Step 'config mirror' @('xlings', 'config', '--mirror', 'GLOBAL')
    # The release archive carries an index snapshot frozen at build time;
    # refresh it so recently added package versions resolve.
    Invoke-Step 'index update'  @('xlings', 'update')

    Invoke-IndexOverlay
}

# Invoke-IndexOverlay — swap in a candidate xim-pkgindex, if one was requested.
#
# Recipe bugs are only ever caught on a real install, and a recipe fix
# therefore cannot be validated until it is already published. This lets an
# unmerged xim-pkgindex branch be run through the whole suite before it lands.
# Only pkgs/ is replaced; the rest of the directory is metadata the fetch wrote.
function Invoke-IndexOverlay {
    $ref = Get-EnvOr 'XIM_PKGINDEX_REF' ''
    if (-not $ref) { return }

    Write-Section "Index override - xim-pkgindex@$ref"
    $dst = Join-Path $XlingsHome 'data\xim-pkgindex'
    if (-not (Test-Path (Join-Path $dst 'pkgs'))) {
        Fail "no published index at $dst\pkgs to override"
    }

    $tmp = New-TempDir
    Invoke-Step "clone xim-pkgindex@$ref" @(
        'git', 'clone', '-q', '--depth', '1', '--branch', $ref,
        'https://github.com/openxlings/xim-pkgindex.git', (Join-Path $tmp 'idx')
    )

    Remove-Item -Recurse -Force (Join-Path $dst 'pkgs')
    Copy-Item -Recurse -Force (Join-Path $tmp 'idx\pkgs') (Join-Path $dst 'pkgs')
    Write-Ok "pkgs/ replaced from xim-pkgindex@$ref"
}

# ── core: lifecycle + multi-version switch on mcpp ────────────────────

function Invoke-SuiteCore {
    Write-Section 'Discovery - search on a cold index'
    $global:LASTEXITCODE = 0
    $out = (xlings search mcpp 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { Fail "xlings search mcpp exited $LASTEXITCODE" }
    if ($out -notlike '*mcpp*') { Fail "xlings search mcpp did not mention mcpp:`n$out" }
    Write-Ok 'search found mcpp'

    Write-Section 'Install and run - ninja'
    # ninja is the install-and-run probe: tiny, self-contained, and present on
    # every platform in the matrix, so a failure here is xlings's, not the
    # package's.
    Invoke-Step 'install ninja' @('xlings', 'install', "ninja@$NinjaVersion", '-y', '-g')
    Assert-ToolVersion ninja $NinjaVersion

    Write-Section 'Multi-version switch - mcpp'
    Invoke-Step "install mcpp $McppOld" @('xlings', 'install', "mcpp@$McppOld", '-y', '-g')
    Invoke-Step "install mcpp $McppNew" @('xlings', 'install', "mcpp@$McppNew", '-y', '-g')
    # Two switches, two distinct versions: a `use` that silently no-ops fails
    # one of them no matter which version happened to be active.
    Assert-Switch mcpp $McppOld @('mcpp')
    Assert-Switch mcpp $McppNew @('mcpp')

    Write-Section 'Inventory - list'
    $global:LASTEXITCODE = 0
    $out = (xlings list 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { Fail "xlings list exited $LASTEXITCODE" }
    if ($out -notlike '*ninja*') { Fail "xlings list omits ninja:`n$out" }
    if ($out -notlike '*mcpp*')  { Fail "xlings list omits mcpp:`n$out" }
    Write-Ok 'list reports ninja and mcpp'

    Write-Section 'Project mode - .xlings.json workspace'
    $proj = New-TempDir
    Write-TextFile (Join-Path $proj '.xlings.json') @"
{
  "mirror": "GLOBAL",
  "workspace": {
    "ninja": "$NinjaVersion"
  }
}
"@

    Push-Location $proj
    try { Invoke-Step 'project install' @('xlings', 'install', '-y') }
    finally { Pop-Location }

    $projShim = Join-Path $proj '.xlings\subos\_\bin\ninja.exe'
    if (-not (Test-Path $projShim)) {
        Fail "project install produced no project-local shim at $projShim"
    }
    $got = Get-VersionFrom ((& $projShim --version 2>&1) | Out-String)
    if ($got -ne $NinjaVersion) {
        Fail "project-local ninja shim reports $got, expected $NinjaVersion"
    }
    Write-Ok "project-local shim resolves ninja $got"

    Write-Section 'Self-management - doctor'
    Invoke-Step 'self doctor' @('xlings', 'self', 'doctor')

    Write-Section 'Teardown - self uninstall'
    Invoke-Step 'self uninstall' @('xlings', 'self', 'uninstall', '-y')
    if (Test-Path $XlingsHome) { Fail "self uninstall left $XlingsHome behind" }
    Write-Ok "self uninstall removed $XlingsHome"
}

# ── gcc: group registration (no switch available on Windows) ──────────

function Invoke-SuiteGcc {
    # `mingw-w64`, NOT `gcc`. On Windows gcc.lua registers nothing — its
    # config() returns early with "config in mingw-w64.lua", and its lone
    # windows entry declares no payload. mingw-w64.lua is what actually
    # registers the gcc/g++/c++ shims, so that is the package a Windows user
    # installs to get a C++ compiler.
    Write-Section "Install mingw-w64 $MingwVersion (provides gcc/g++/c++)"
    Invoke-Step "install mingw-w64 $MingwVersion" @('xlings', 'install', "mingw-w64@$MingwVersion", '-y', '-g')

    Write-Section 'Release group - gcc/g++/c++ must agree'
    # Only one mingw-w64 version exists in the index, so there is nothing to
    # switch BETWEEN — hence consistency rather than a switch. A registration
    # that wires up `gcc` but leaves `g++` pointing elsewhere is the same
    # defect the Linux switch test catches.
    Assert-GroupConsistent 'mingw-w64' @('gcc', 'g++', 'c++')

    Write-Section 'The toolchain compiles and runs'
    $work = New-TempDir
    Write-TextFile (Join-Path $work 'hello.cpp') @'
#include <iostream>
#include <string>
#include <vector>
int main() {
    std::vector<std::string> parts{"fresh", "install", "ok"};
    for (const auto& p : parts) std::cout << p << ' ';
    std::cout << '\n';
}
'@

    Push-Location $work
    try { Invoke-Step 'g++ compile' @('g++', '-std=c++20', 'hello.cpp', '-o', 'hello.exe') }
    finally { Pop-Location }
    Assert-Runs "mingw-w64 $MingwVersion g++ binary" (Join-Path $work 'hello.exe') 'fresh install ok'
}

# ── llvm: version switch ──────────────────────────────────────────────

function Invoke-SuiteLlvm {
    Write-Section 'Install two llvm versions'
    Invoke-Step "install llvm $LlvmOld" @('xlings', 'install', "llvm@$LlvmOld", '-y', '-g')
    Invoke-Step "install llvm $LlvmNew" @('xlings', 'install', "llvm@$LlvmNew", '-y', '-g')

    Write-Section 'Version switch - clang/clang++ must move together'
    Assert-Switch llvm $LlvmOld @('clang', 'clang++')
    Assert-Switch llvm $LlvmNew @('clang', 'clang++')

    Write-Section 'The switched toolchain compiles and runs'
    # C, not C++, on purpose: this asserts the compiler driver and its runtime
    # work end to end without also betting on the C++ standard library wiring,
    # which is a separate concern from the version switch under test here.
    $work = New-TempDir
    Write-TextFile (Join-Path $work 'hello.c') @'
#include <stdio.h>
int main(void) { printf("fresh install ok\n"); return 0; }
'@

    Push-Location $work
    try { Invoke-Step 'clang compile' @('clang', 'hello.c', '-o', 'hello.exe') }
    finally { Pop-Location }
    Assert-Runs "clang $LlvmNew binary" (Join-Path $work 'hello.exe') 'fresh install ok'
}

# ── entry point ───────────────────────────────────────────────────────

Write-Host "+- fresh-install: $Suite suite on Windows $env:PROCESSOR_ARCHITECTURE" -ForegroundColor Yellow

Invoke-Bootstrap
switch ($Suite) {
    'core' { Invoke-SuiteCore }
    'gcc'  { Invoke-SuiteGcc }
    'llvm' { Invoke-SuiteLlvm }
}

Write-Section "PASS - $Suite suite"

# Land an explicit success: this script checks for exit codes it expects to be
# non-zero, and `pwsh -command ". script.ps1"` returns whatever $LASTEXITCODE
# happens to hold as the step's exit code -- turning a passing test into a red
# job with "PASS" printed right above the failure.
exit 0
