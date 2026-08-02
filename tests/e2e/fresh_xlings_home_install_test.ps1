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
  $home = Join-Path $root "explicit-home"
  New-Item -ItemType Directory -Force (Join-Path $home "data") | Out-Null
  Copy-Item -Recurse $fixture (Join-Path $home "data\xim-pkgindex")
  Set-Content (Join-Path $home "data\xim-pkgindex\xim-indexrepos.lua") 'xim_indexrepos = {}'
  Remove-Item (Join-Path $home "data\xim-pkgindex\.xlings-index-cache.json") -Force -ErrorAction SilentlyContinue
  @{mirror="GLOBAL"; index_repos=@(@{name="xim"; url=$fixture})} |
    ConvertTo-Json -Depth 4 | Set-Content (Join-Path $home ".xlings.json")
  if (Test-Path (Join-Path $home "subos")) { throw "subos precondition failed" }
  $env:HOME = Join-Path $root "user"
  $env:USERPROFILE = $env:HOME
  $env:XLINGS_HOME = $home
  & $bin.FullName install xpkg-helper -y
  if ($LASTEXITCODE -ne 0) { throw "issue #471: first install exited $LASTEXITCODE" }
  $statePath = Join-Path $home "subos\default\.xlings.json"
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
