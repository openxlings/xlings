# Regression for mcpp#693: xlings must not abort silently when its working
# directory contains a character outside the process's ANSI code page.
#
# Measured on a real windows-latest runner (ACP 1252): both `xlings.exe
# --version` and the xlings shim `mcpp.exe` (a copy of xlings.exe) died with
# 0xC0000409 and printed nothing when run from such a directory. The cause is
# an uncaught std::system_error thrown while std::filesystem narrows a path
# through the ANSI code page, during start-up before any command runs. The
# fix has two parts, and this script exercises both:
#
#   - an embedded UTF-8 activeCodePage application manifest ([resources] in
#     mcpp.toml), which removes the throw on Windows 10 1903 and later by
#     making the process's own ANSI code page UTF-8;
#   - a try/catch around the body of main(), which reports the failure
#     instead of aborting silently should the manifest ever be ignored (an
#     older host, or a build where the manifest could not be embedded).
#
# See .agents/docs/2026-08-07-windows-resources-and-version-identity-design.md
# in the mcpp repository for [resources], and mcpp#693 for the report.
$ErrorActionPreference = "Stop"

# ACP is a property of the runner, not of this workflow. If it is ever
# already UTF-8 (65001) -- for example a runner with the "Beta: Use Unicode
# UTF-8" system setting enabled -- xlings would not exercise the throwing
# path even without today's fix, and this step would pass without having
# measured anything.
$acp = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage' -ErrorAction Stop).ACP
Write-Host "runner ANSI code page (registry ACP): $acp"
if ([string]::IsNullOrEmpty($acp)) { throw "could not read the registry ACP value" }
if ($acp -eq '65001') {
  throw "runner ANSI code page is already UTF-8 (65001); this step cannot exercise the code path it exists to test"
}

$bin = Get-ChildItem target -Recurse -Filter xlings.exe |
  Where-Object FullName -Like '*\bin\xlings.exe' |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $bin) { throw "xlings.exe not found under target\" }
$exe = $bin.FullName
Write-Host "xlings.exe: $exe"

$VERSION = (Select-String -Path (Join-Path $env:GITHUB_WORKSPACE "src\core\config.cppm") -Pattern 'VERSION = "([^"]*)"' |
  ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1)
if (-not $VERSION) { throw "could not read the xlings version from src\core\config.cppm" }
Write-Host "expected version: $VERSION"

# Built from code points so the workflow source stays plain ASCII: 0x6D4B
# 0x8BD5 spell "测试" ("test"); 0x1F9EA is the test-tube emoji from mcpp#693,
# a supplementary-plane character that needs a UTF-16 surrogate pair.
$dirName = "repro-" + [char]0x6D4B + [char]0x8BD5 + "-" + [char]::ConvertFromUtf32(0x1F9EA)
$testDir = Join-Path $env:RUNNER_TEMP $dirName
New-Item -ItemType Directory -Force -Path $testDir | Out-Null
Write-Host "test directory: $testDir"

Push-Location $testDir
try {
  $output = & $exe --version 2>&1 | Out-String
  $versionExit = $LASTEXITCODE
} finally {
  Pop-Location
}
Write-Host "xlings.exe --version exit code: $versionExit"
Write-Host "xlings.exe --version output: $output"
if ($versionExit -ne 0) {
  throw "xlings.exe --version exited $versionExit (expected 0) in a directory outside the ANSI code page"
}
if ($output -notmatch [regex]::Escape($VERSION)) {
  throw "xlings.exe --version did not print the expected version '$VERSION'"
}

# A second command that walks the project-config search from the working
# directory, for coverage beyond the start-up path --version alone exercises.
Push-Location $testDir
try {
  & $exe config | Out-Null
  $configExit = $LASTEXITCODE
} finally {
  Pop-Location
}
Write-Host "xlings.exe config exit code: $configExit"
if ($configExit -ne 0) {
  throw "xlings.exe config exited $configExit (expected 0) in a directory outside the ANSI code page"
}

# Confirm the manifest is actually embedded, rather than trusting that mcpp
# compiled [resources] as declared.
$mtRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$mt = Get-ChildItem $mtRoot -Directory -ErrorAction SilentlyContinue |
  Sort-Object Name -Descending |
  ForEach-Object { Join-Path $_.FullName "x64\mt.exe" } |
  Where-Object { Test-Path $_ } |
  Select-Object -First 1
if (-not $mt) { throw "mt.exe not found under $mtRoot" }
Write-Host "mt.exe: $mt"

$manifestOut = Join-Path $env:RUNNER_TEMP "xlings-manifest-check.xml"
& $mt -nologo "-inputresource:${exe};#1" "-out:$manifestOut"
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $manifestOut)) {
  throw "mt.exe could not extract the manifest from $exe (exit $LASTEXITCODE)"
}
$manifestText = Get-Content $manifestOut -Raw
Write-Host "extracted manifest:"
Write-Host $manifestText
if ($manifestText -notmatch 'activeCodePage') {
  throw "extracted manifest does not declare activeCodePage"
}

Write-Host "PASS: ACP=$acp, xlings.exe --version exit=$versionExit, version '$VERSION' found, xlings.exe config exit=$configExit, manifest carries activeCodePage"
exit 0
