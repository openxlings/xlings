$ErrorActionPreference = "Stop"
$bin = Get-ChildItem target -Recurse -Filter xlings.exe |
  Where-Object FullName -Like '*\bin\xlings.exe' |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $bin) { throw "xlings.exe not found" }
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$fixture = Join-Path $repoRoot "tests\fixtures\xim-pkgindex"
if (-not (Test-Path (Join-Path $fixture "pkgs"))) { throw "fixture index missing" }
$root = Join-Path ([IO.Path]::GetTempPath()) "xlings-fresh-home-$([guid]::NewGuid())"
try {
  $explicitHome = Join-Path $root "explicit-home"
  New-Item -ItemType Directory -Force (Join-Path $explicitHome "data") | Out-Null
  Copy-Item -Recurse $fixture (Join-Path $explicitHome "data\xim-pkgindex")
  Set-Content (Join-Path $explicitHome "data\xim-pkgindex\xim-indexrepos.lua") 'xim_indexrepos = {}'
  Remove-Item (Join-Path $explicitHome "data\xim-pkgindex\.xlings-index-cache.json") -Force -ErrorAction SilentlyContinue
  @{mirror="GLOBAL"; index_repos=@(@{name="xim"; url=$fixture})} |
    ConvertTo-Json -Depth 4 | Set-Content (Join-Path $explicitHome ".xlings.json")
  if (Test-Path (Join-Path $explicitHome "subos")) { throw "subos precondition failed" }
  $env:HOME = Join-Path $root "user"
  $env:USERPROFILE = $env:HOME
  $env:XLINGS_HOME = $explicitHome
  & $bin.FullName install xpkg-helper -y
  if ($LASTEXITCODE -ne 0) { throw "issue #471: first install exited $LASTEXITCODE" }
  $statePath = Join-Path $explicitHome "subos\default\.xlings.json"
  if (-not (Test-Path $statePath)) { throw "workspace state missing" }
  $state = Get-Content -Raw $statePath | ConvertFrom-Json
  $entry = $state.workspace.'xpkg-helper'
  if ($entry.active -ne '0.0.1' -or @($entry.installed).Count -ne 1 -or
      @($entry.installed)[0] -ne '0.0.1') { throw "workspace state mismatch" }
  $list = & $bin.FullName list | Out-String
  if ($list -notmatch 'xpkg-helper@0\.0\.1') { throw "inventory missing package" }
  Write-Host "fresh XLINGS_HOME first install: ok"
} finally {
  Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}

# Land an explicit success: this script checks for exit codes it expects to be
# non-zero, and `pwsh -command ". script.ps1"` returns whatever $LASTEXITCODE
# happens to hold as the step's exit code -- turning a passing test into a red
# job with "PASS" printed right above the failure.
exit 0
