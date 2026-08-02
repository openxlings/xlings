$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$script = Get-Content -Raw (Join-Path $root "tools\other\quick_install.ps1")

foreach ($needle in @("ChecksumUrl", "Get-FileHash", "checksum sidecar is missing or malformed", "windows-`$arch", "IsOutputRedirected", "NO_COLOR")) {
    if (-not $script.Contains($needle)) { throw "missing installer contract: $needle" }
}
Write-Host "quick_install.ps1 contracts: ok"
