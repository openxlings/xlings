$ErrorActionPreference = "Stop"
$bin = Get-ChildItem target -Recurse -Filter xlings.exe | Where-Object FullName -Like '*\bin\xlings.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $bin) { throw "xlings.exe not found" }
$root = Join-Path ([IO.Path]::GetTempPath()) "xlings-subos-cmd-$([guid]::NewGuid())"
try {
  $env:XLINGS_HOME = Join-Path $root ".xlings"
  $env:USERPROFILE = $root
  & $bin.FullName self init
  & $bin.FullName subos new probe
  & $bin.FullName subos use probe --sandbox --cmd '$env:USERPROFILE | Set-Content -NoNewline "$env:USERPROFILE\marker"; exit 37'
  if ($LASTEXITCODE -ne 37) { throw "expected exit 37, got $LASTEXITCODE" }
  $sandboxUser = if ([string]::IsNullOrEmpty($env:USERNAME)) { "user" } else { $env:USERNAME }
  $sandboxHome = Join-Path $env:XLINGS_HOME "subos\probe\home\$sandboxUser"
  if ((Get-Content -Raw (Join-Path $sandboxHome "marker")) -ne $sandboxHome) { throw "USERPROFILE was not redirected" }

  # Force the cmd.exe fallback and exercise cmd's own quote/metacharacter
  # grammar. The CreateProcess CRT quoting used for PowerShell must not be
  # reused for the source following cmd /s /c.
  $env:XLINGS_SHELL = "cmd.exe"
  & $bin.FullName subos use probe --sandbox --cmd 'echo quoted^&pipe>"%USERPROFILE%\cmd-marker" & exit /b 37'
  if ($LASTEXITCODE -ne 37) { throw "cmd fallback expected exit 37, got $LASTEXITCODE" }
  if ((Get-Content -Raw (Join-Path $sandboxHome "cmd-marker")).Trim() -ne 'quoted&pipe') {
    throw "cmd fallback changed quoting or metacharacter semantics"
  }
  Remove-Item Env:XLINGS_SHELL
  Write-Host "subos cmd Windows contract: ok"
} finally { Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue }
