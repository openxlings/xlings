# The Luban OS app's release payload on Windows (part 4 §8.2):
# build\luban-os-<version>-windows-x86_64.zip and its .sha256 -- bin\luban-os.exe,
# which links only Windows' own DLLs (SDL3 is built in).
#
#   luban\tools\luban_os_release.ps1 [binary]   # default: the newest dist build
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$version = (Select-String -Path "$project\src\core\config.cppm" -Pattern 'VERSION = "([^"]*)"' | Select-Object -First 1).Matches[0].Groups[1].Value
if (-not $version) { throw "[luban-os] no version in src/core/config.cppm" }
$binary = if ($args.Count -ge 1) { $args[0] } else {
    (Get-ChildItem "$project\target" -Recurse -Filter 'luban-os.exe' |
        Where-Object { $_.FullName -match '[\\/]bin[\\/]luban-os[\\/]luban-os\.exe$' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
if (-not $binary -or -not (Test-Path $binary)) { throw "[luban-os] no luban-os.exe build (mcpp build -p luban-os --profile dist)" }
$name = "luban-os-$version-windows-x86_64"
$out = Join-Path $project "build\$name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path "$out\bin" | Out-Null
Copy-Item $binary "$out\bin\luban-os.exe"
"luban-os $version (openxlings/xlings, luban/host/luban-os): the Luban OS window for Windows; Windows' own DLLs only" |
    Set-Content "$out\README.provenance"
$zip = Join-Path $project "build\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $out -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
"$hash  $name.zip" | Set-Content "$zip.sha256"
Write-Host "[luban-os] $hash  $name.zip"
