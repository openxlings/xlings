param([Parameter(Mandatory=$true)][string]$Archive,
      [Parameter(Mandatory=$true)][string]$Sidecar)
$ErrorActionPreference = "Stop"
$line = (Get-Content -Raw $Sidecar).Trim()
if ($line -notmatch '^(?<hash>[0-9a-fA-F]{64})(?:\s+\*?\S+)?$') { throw "malformed sidecar" }
if ((Get-FileHash -Algorithm SHA256 $Archive).Hash -ne $Matches.hash) { throw "checksum mismatch" }
$work = Join-Path ([IO.Path]::GetTempPath()) "xlings-candidate-$([guid]::NewGuid())"
try {
  Expand-Archive $Archive $work
  $bin = Get-ChildItem $work -Recurse -Filter xlings.exe | Where-Object FullName -Like '*\bin\xlings.exe' | Select-Object -First 1
  if (-not $bin) { throw "candidate binary missing" }
  $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
  $userHome = Join-Path $work "home"
  $env:USERPROFILE = $userHome
  $env:HOME = $userHome
  $env:XLINGS_HOME = Join-Path $userHome "explicit-cold-home"
  $env:XLINGS_NON_INTERACTIVE = "1"
  $env:NO_COLOR = "1"
  & $bin.FullName --version
  if ($LASTEXITCODE -ne 0) { throw "version smoke failed" }
  Push-Location $bin.Directory.Parent.FullName
  try { & $bin.FullName self install } finally { Pop-Location }
  if ($LASTEXITCODE -ne 0) { throw "self install failed" }
  $installed = Join-Path $env:XLINGS_HOME "bin\xlings.exe"
  if (-not (Test-Path $installed)) { throw "installed candidate missing" }
  & $installed self doctor
  if ($LASTEXITCODE -ne 0) { throw "self doctor failed" }
  $fixtureSource = Join-Path $repoRoot "tests\fixtures\xim-pkgindex\pkgs\x\xpkg-helper.lua"
  $fixture = Join-Path $work "candidate-helper.lua"
  (Get-Content -Raw $fixtureSource).Replace('name = "xpkg-helper"', 'name = "candidate-helper"') |
    Set-Content $fixture
  & $installed config --add-xpkg $fixture
  if ($LASTEXITCODE -ne 0) { throw "fixture import failed" }
  $search = & $installed search candidate-helper | Out-String
  if ($LASTEXITCODE -ne 0 -or $search -notmatch 'local:candidate-helper') {
    throw "fixture search failed"
  }
  & $installed install candidate-helper -y
  if ($LASTEXITCODE -ne 0) { throw "fixture install failed" }
  & $installed use candidate-helper 0.0.1
  if ($LASTEXITCODE -ne 0) { throw "fixture activation failed" }
  $candidateShim = Join-Path $env:XLINGS_HOME "subos\default\bin\candidate-helper.exe"
  if (-not (Test-Path $candidateShim)) { throw "fixture shim missing" }
  & $candidateShim | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "fixture execution failed" }
  $list = & $installed list | Out-String
  if ($list -notmatch 'candidate-helper@0\.0\.1') { throw "installed fixture absent from list" }
  $info = & $installed info local:candidate-helper | Out-String
  if ($LASTEXITCODE -ne 0 -or $info -notmatch '0\.0\.1') { throw "fixture info failed" }
  & $installed remove candidate-helper -y
  if ($LASTEXITCODE -ne 0) { throw "fixture remove failed" }
  $list = & $installed list | Out-String
  if ($list -match 'candidate-helper@0\.0\.1') { throw "removed fixture remains in list" }

  & $installed subos new candidate-probe
  if ($LASTEXITCODE -ne 0) { throw "subos new failed" }
  $marker = Join-Path $work "plain-marker"
  & $installed subos use candidate-probe --cmd "Set-Content -NoNewline -Path '$marker' -Value candidate; exit 37"
  if ($LASTEXITCODE -ne 37 -or (Get-Content -Raw $marker) -ne 'candidate') {
    throw "plain subos command contract failed"
  }
  $sandboxMarker = Join-Path $env:XLINGS_HOME "subos\candidate-probe\home\$env:USERNAME\marker"
  & $installed subos use candidate-probe --sandbox --cmd 'Set-Content -NoNewline -Path (Join-Path $env:USERPROFILE "marker") -Value sandbox; exit 37'
  if ($LASTEXITCODE -ne 37 -or (Get-Content -Raw $sandboxMarker) -ne 'sandbox') {
    throw "sandbox subos command contract failed"
  }
  & $installed subos info candidate-probe | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "subos info failed" }
  & $installed subos remove candidate-probe
  if ($LASTEXITCODE -ne 0) { throw "subos remove failed" }
} finally { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
