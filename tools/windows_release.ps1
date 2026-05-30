# Build a bootstrap xlings package for Windows x86_64.
#
# Directory layout:
#   xlings-<ver>-windows-x86_64/
#   ├── .xlings.json
#   ├── data/xim-pkgindex/        # bundled index snapshot
#   └── bin/
#       └── xlings.exe
#
# Remaining runtime directories are created lazily by `xlings self init`.
#
# Output:  build/xlings-<ver>-windows-x86_64.zip
# Usage:   pwsh ./tools/windows_release.ps1

$ErrorActionPreference = "Stop"

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT_DIR = (Resolve-Path "$SCRIPT_DIR\..").Path

$VERSION = (Select-String -Path "$PROJECT_DIR\src\core\config.cppm" -Pattern 'VERSION = "([^"]*)"' |
  ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1)
if (-not $VERSION) { $VERSION = "0.2.0" }

$ARCH = "x86_64"
$PKG_NAME = "xlings-$VERSION-windows-$ARCH"
$OUT_DIR = "$PROJECT_DIR\build\$PKG_NAME"

function Info($msg)  { Write-Host "[release] $msg" }
function Fail($msg)  { Write-Error "[release] FAIL: $msg"; exit 1 }

Set-Location $PROJECT_DIR

# -- 1. Build C++ ------------------------------------------------
Info "Version: $VERSION  |  Arch: $ARCH"
Info "Building C++ binary..."
$MCPP_BIN = if ($env:MCPP_BIN) { $env:MCPP_BIN } else { "mcpp" }
if (-not (Get-Command $MCPP_BIN -ErrorAction SilentlyContinue)) {
  Fail "mcpp not found; run xlings install first"
}
if (Test-Path "$PROJECT_DIR\target") { Remove-Item -Recurse -Force "$PROJECT_DIR\target" }
$mcppArgs = @("build", "--print-fingerprint", "--no-cache")
if ($env:MCPP_TARGET) { $mcppArgs += @("--target", $env:MCPP_TARGET) }
& $MCPP_BIN @mcppArgs
if ($LASTEXITCODE -ne 0) { Fail "mcpp build failed" }

$BIN_FILE = Get-ChildItem "$PROJECT_DIR\target" -Recurse -Filter "xlings.exe" |
  Where-Object { $_.FullName -match "[\\/]+bin[\\/]+xlings\.exe$" } |
  Sort-Object FullName |
  Select-Object -First 1
if (-not $BIN_FILE) { Fail "C++ binary not found under target\*\bin\xlings.exe" }
$BIN_SRC = $BIN_FILE.FullName

# -- 2. Assemble package -----------------------------------------
Info "Assembling $OUT_DIR ..."
if (Test-Path $OUT_DIR) { Remove-Item -Recurse -Force $OUT_DIR }

$dirs = @(
  "$OUT_DIR\bin"
)
foreach ($d in $dirs) { New-Item -ItemType Directory -Force -Path $d | Out-Null }

Copy-Item $BIN_SRC "$OUT_DIR\bin\"

# Release packages default to GLOBAL. Users can switch mirror locally after
# install, and CI also keeps GLOBAL for github.com endpoints.
$MIRROR = if ($env:XLINGS_RELEASE_MIRROR) { $env:XLINGS_RELEASE_MIRROR } else { "GLOBAL" }
if ($MIRROR -eq "CN") {
  $RES_SERVER_URL = "https://gitcode.com/xlings-res"
  $INDEX_REPO_URL = "https://gitee.com/sunrisepeak/xim-pkgindex.git"
  $REPO_URL = "https://gitee.com/sunrisepeak/xlings.git"
} else {
  $RES_SERVER_URL = "https://github.com/xlings-res"
  $INDEX_REPO_URL = "https://github.com/openxlings/xim-pkgindex.git"
  $REPO_URL = "https://github.com/openxlings/xlings.git"
}

$configSrc = "$PROJECT_DIR\config\xlings.json"
if (Test-Path $configSrc) {
  $base = Get-Content $configSrc -Raw | ConvertFrom-Json
  $base | Add-Member -NotePropertyName "version" -NotePropertyValue $VERSION -Force
  $base | Add-Member -NotePropertyName "mirror" -NotePropertyValue $MIRROR -Force
  $base | Add-Member -NotePropertyName "activeSubos" -NotePropertyValue "default" -Force
  $base | Add-Member -NotePropertyName "subos" -NotePropertyValue @{default=@{dir=""}} -Force
  if (-not $base.xim) { $base | Add-Member -NotePropertyName "xim" -NotePropertyValue @{} -Force }
  $base.xim | Add-Member -NotePropertyName "res-server" -NotePropertyValue $RES_SERVER_URL -Force
  $base.xim | Add-Member -NotePropertyName "index-repo" -NotePropertyValue $INDEX_REPO_URL -Force
  $base | Add-Member -NotePropertyName "repo" -NotePropertyValue $REPO_URL -Force
  $base | ConvertTo-Json -Depth 10 -Compress | Set-Content "$OUT_DIR\.xlings.json" -Encoding UTF8
} else {
  @"
{"activeSubos":"default","subos":{"default":{"dir":""}},"version":"$VERSION","need_update":false,"mirror":"$MIRROR","xim":{"mirrors":{"index-repo":{"GLOBAL":"https://github.com/openxlings/xim-pkgindex.git","CN":"https://gitee.com/sunrisepeak/xim-pkgindex.git"},"res-server":{"GLOBAL":"https://github.com/xlings-res","CN":"https://gitcode.com/xlings-res"}},"res-server":"$RES_SERVER_URL","index-repo":"$INDEX_REPO_URL"},"repo":"$REPO_URL"}
"@ | Set-Content "$OUT_DIR\.xlings.json" -Encoding UTF8
}

& "$PROJECT_DIR\tools\package_xim_index.ps1" -OutDir $OUT_DIR

Info "Package assembled: $OUT_DIR"

# -- 4. Verification ---------------------------------------------
Info "=== Verification ==="

$requiredBins = @("bin\xlings.exe")
foreach ($f in $requiredBins) {
  if (-not (Test-Path "$OUT_DIR\$f")) { Fail "$f is missing" }
}
Info "OK: all binaries present"

if (-not (Test-Path "$OUT_DIR\.xlings.json")) { Fail ".xlings.json missing" }
Info "OK: .xlings.json present"

if (-not (Test-Path "$OUT_DIR\data\xim-pkgindex\pkgs")) { Fail "bundled xim-pkgindex missing" }
if (-not (Test-Path "$OUT_DIR\data\xim-pkgindex\pkgs\p\patchelf.lua")) { Fail "bundled xim-pkgindex missing patchelf" }
Info "OK: bundled xim-pkgindex snapshot present"

$env:XLINGS_HOME = $OUT_DIR
$env:PATH = "$OUT_DIR\bin;$env:PATH"

$helpOut = & "$OUT_DIR\bin\xlings.exe" -h 2>&1 | Out-String
if ($helpOut -notmatch "subos") { Fail "xlings -h missing 'subos' command" }
Info "OK: xlings -h shows subos/self commands"

$initOut = & "$OUT_DIR\bin\xlings.exe" self init 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { Fail "xlings self init failed" }
if ($initOut -notmatch "init ok") { Fail "self init output missing success marker" }
$requiredRuntimeDirs = @(
  "data\xpkgs",
  "data\runtimedir",
  "data\xim-index-repos",
  "data\local-indexrepo",
  "subos\default\bin",
  "subos\default\lib",
  "subos\default\usr",
  "subos\default\generations",
  "config\shell"
)
foreach ($d in $requiredRuntimeDirs) {
  if (-not (Test-Path "$OUT_DIR\$d")) { Fail "directory $d missing after self init" }
}
if (-not (Test-Path "$OUT_DIR\subos\current")) { Fail "subos\current junction missing after self init" }
if (-not (Test-Path "$OUT_DIR\subos\default\bin\xlings.exe")) { Fail "subos/default/bin/xlings.exe missing after self init" }
Info "OK: self init materialized bootstrap home"

# -- 5. Create archive -------------------------------------------
Info ""
Info "All checks passed. Creating release archive..."

$ARCHIVE = "$PROJECT_DIR\build\$PKG_NAME.zip"
if (Test-Path $ARCHIVE) { Remove-Item $ARCHIVE }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
  $OUT_DIR,
  $ARCHIVE,
  [System.IO.Compression.CompressionLevel]::Optimal,
  $true
)

Info ""
Info "Done."
Info "  Package:  $OUT_DIR"
Info "  Archive:  $ARCHIVE"
Info ""
Info "  Unpack & install:"
Info "    Expand-Archive $PKG_NAME.zip -DestinationPath ."
Info "    cd $PKG_NAME"
Info "    .\bin\xlings.exe self install"
