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

  # Re-install THROUGH the installed binary, so the shim being rewritten is the
  # running image. Windows cannot delete a running executable; the code used to
  # discard the failed delete and then hard-link and copy over an occupied path,
  # reporting "failed to create shim ... the process cannot access the file
  # because it is being used by another process" with the update half applied.
  # That is issue #473, and it is the shape `xlings self update` hits every time.
  #
  # No synthetic file lock: a handle opened with FileShare::Read blocks the
  # rename too, which a real running image does not, so it would fail this even
  # when the fix is correct. Running the binary is the only faithful version.
  Push-Location $bin.Directory.Parent.FullName
  try { $selfUpdate = & $installed self install 2>&1 | Out-String } finally { Pop-Location }
  if ($LASTEXITCODE -ne 0) {
    Write-Host $selfUpdate
    throw "self install over the running binary failed (issue #473)"
  }
  if ($selfUpdate -match 'failed to create shim|cannot free shim path') {
    Write-Host $selfUpdate
    throw "shim rewrite reported a locked path while replacing the running binary"
  }
  # Positive evidence that the step COMPLETED, not merely that it started.
  #
  # This used to accept `install:` -- which is printed BEFORE the reinstall
  # confirmation, so a cancelled run matched it. And every run was cancelled
  # until 2026.8.22.3: `ask_yes_no` returned its `false` default on EOF, so
  # this test passed on the exit code of a cancellation and never once
  # overwrote a running binary, which is the one thing it exists to check
  # (issue #473).
  #
  # `- ok` is printed only by the path that finished.
  if ($selfUpdate -notmatch '- ok|fixing links') {
    Write-Host $selfUpdate
    throw "self install over the running binary did not complete"
  }
  if ($selfUpdate -match '\[xlings:self\] cancelled') {
    Write-Host $selfUpdate
    throw "self install was cancelled, so the running-binary overwrite never happened"
  }
  if (-not (Test-Path $installed)) { throw "the running binary was displaced without a replacement" }
  & $installed --version | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "the replaced binary does not run" }
  $fixture = Join-Path $repoRoot "tests\candidate-install\candidate-helper.lua"
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

# This script checks exit codes it expects to be non-zero (`-ne 37`), so land
# an explicit success rather than leaving whatever $LASTEXITCODE happens to
# hold: `pwsh -command ". script.ps1"` returns it as the step's exit code, and
# the job then goes red with a passing test in the log above it.
exit 0
