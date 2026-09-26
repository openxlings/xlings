# E2E: Windows shim identity (issue #615)
#
# Every shim in <home>\subos\<s>\bin\*.exe is a HARD LINK to the entry binary
# <home>\bin\xlings.exe. Before the fix, upgrading replaced the entry by
# rename+copy and left every existing shim pointing at the OLD binary
# forever -- silently, since a hard link keeps working, it just runs stale
# code. This script proves: doctor reports the drift, --fix and self init
# both heal it, an upgrade through the fixed client re-points every shim as
# part of the upgrade (not as a separate manual step), and a stale shim that
# is itself a new build hands execution off to the entry rather than running
# stale code.
param(
    [Parameter(Mandatory=$true)][string]$Archive,
    [string]$LegacyBinary = "$env:USERPROFILE\.xlings\bin\xlings.exe"
)
$ErrorActionPreference = "Stop"

# Resolve and copy out the legacy (pre-fix) binary BEFORE we repoint
# USERPROFILE/HOME below. $LegacyBinary's default depends on the REAL
# USERPROFILE (the CI runner's bootstrap xlings, v2026.8.27.5, predates this
# fix), so it has to be captured while that is still in effect.
$legacyResolved = Resolve-Path $LegacyBinary -ErrorAction SilentlyContinue
if (-not $legacyResolved) { throw "legacy binary not found: $LegacyBinary" }
$legacyPath = $legacyResolved.Path

$work = Join-Path ([IO.Path]::GetTempPath()) "xlings-shim-identity-$([guid]::NewGuid())"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$legacyCopy = Join-Path $work "legacy-xlings.exe"
Copy-Item $legacyPath $legacyCopy

try {
  $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

  $userHome = Join-Path $work "home"
  $env:USERPROFILE = $userHome
  $env:HOME = $userHome
  $env:XLINGS_HOME = Join-Path $userHome ".xlings-test"
  $env:XLINGS_NON_INTERACTIVE = "1"
  $env:NO_COLOR = "1"
  $env:XLINGS_LOCK_TIMEOUT = "60"

  Expand-Archive $Archive $work
  $bin = Get-ChildItem $work -Recurse -Filter xlings.exe | Where-Object FullName -Like '*\bin\xlings.exe' | Select-Object -First 1
  if (-not $bin) { throw "candidate binary missing" }

  Push-Location $bin.Directory.Parent.FullName
  try { & $bin.FullName self install } finally { Pop-Location }
  if ($LASTEXITCODE -ne 0) { throw "self install failed" }

  $entry = Join-Path $env:XLINGS_HOME "bin\xlings.exe"
  if (-not (Test-Path $entry)) { throw "entry binary missing after self install" }

  function Hash($p) { (Get-FileHash -Algorithm SHA256 $p).Hash.ToUpperInvariant() }

  function Assert-SameAsEntry($p, $why) {
    if ((Hash $p) -ne (Hash $entry)) { throw "${why}: $p differs from the entry" }
  }

  # A native exe's stderr, merged via 2>&1, is surfaced to PowerShell as an
  # ErrorRecord on 5.1; under $ErrorActionPreference = Stop that would abort
  # the script on a successful call that merely logs a warning. Relax it only
  # for the duration of the native call.
  function Invoke-Captured($exe, [string[]]$exeArgs) {
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
      $out = & $exe @exeArgs 2>&1 | Out-String
      $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $prevEAP }
    return @{ Code = $code; Out = $out }
  }

  function Add-DistinguishingBytes($path) {
    # Appends bytes so the file's hash differs while its code (and marker)
    # stays byte-identical to what it was copied from -- "a different
    # build", not "a different xlings".
    $bytes = [byte[]](@(0x5A) * 64)
    $stream = [IO.File]::OpenWrite($path)
    try {
      $stream.Seek(0, [IO.SeekOrigin]::End) | Out-Null
      $stream.Write($bytes, 0, $bytes.Length)
    } finally { $stream.Close() }
  }

  Write-Host "Setup: installing the fixture package and a second subos"
  $fixturePkg = Join-Path $repoRoot "tests\candidate-install\candidate-helper.lua"
  & $entry config --add-xpkg $fixturePkg
  if ($LASTEXITCODE -ne 0) { throw "fixture import failed" }
  & $entry install candidate-helper -y
  if ($LASTEXITCODE -ne 0) { throw "fixture install failed" }
  & $entry use candidate-helper 0.0.1
  if ($LASTEXITCODE -ne 0) { throw "fixture activation failed" }
  $candidateShim = Join-Path $env:XLINGS_HOME "subos\default\bin\candidate-helper.exe"
  if (-not (Test-Path $candidateShim)) { throw "fixture shim missing" }

  & $entry subos new s2
  if ($LASTEXITCODE -ne 0) { throw "subos new s2 failed" }
  $s2Xlings = Join-Path $env:XLINGS_HOME "subos\s2\bin\xlings.exe"
  if (-not (Test-Path $s2Xlings)) { throw "s2 xlings shim missing" }

  $defaultXlings = Join-Path $env:XLINGS_HOME "subos\default\bin\xlings.exe"
  if (-not (Test-Path $defaultXlings)) { throw "default xlings shim missing" }

  # ---------------------------------------------------------------------
  # S1: legacy stale shims -> doctor reports an error -> --fix heals them
  # (the #615 state: an upgrade under the OLD, unfixed xlings left shims
  # like these behind forever).
  # ---------------------------------------------------------------------
  Write-Host "S1: corrupting shims with the legacy (pre-fix) binary"
  if ((Hash $legacyCopy) -eq (Hash $entry)) {
    throw "legacy binary is identical to the candidate; S1 proves nothing"
  }
  $s1Targets = @($candidateShim, $defaultXlings, $s2Xlings)
  foreach ($t in $s1Targets) {
    # Each target is a HARD LINK to the entry binary. Copying onto it in
    # place would overwrite the entry's own content through the link, so
    # remove the link first and let the copy create a fresh file.
    Remove-Item -Force $t
    Copy-Item $legacyCopy $t
  }

  $doctor = Invoke-Captured $entry @('self', 'doctor')
  if ($doctor.Code -eq 0) {
    Write-Host $doctor.Out
    throw "S1: self doctor exited 0 with stale (legacy) shims present"
  }
  if ($doctor.Out -notmatch '(?i)outdated shim') {
    Write-Host $doctor.Out
    throw "S1: self doctor did not report an 'outdated shim' error"
  }

  Write-Host "S1: self doctor --fix heals every stale shim"
  $fix = Invoke-Captured $entry @('self', 'doctor', '--fix')
  if ($fix.Code -ne 0) {
    Write-Host $fix.Out
    throw "S1: self doctor --fix failed"
  }
  Assert-SameAsEntry $candidateShim "S1 fix"
  Assert-SameAsEntry $defaultXlings "S1 fix"
  Assert-SameAsEntry $s2Xlings "S1 fix"

  Write-Host "S1: healed shims still run correctly from a non-ASCII working directory"
  # Built from code points rather than typed as a literal: a script file
  # read without a BOM can mis-decode a literal CJK/emoji argument on
  # Windows PowerShell 5.1, which would make this a test of the source
  # file's encoding instead of the shim.
  $unicodeDir = Join-Path $work ("dir-" + [char]0x4E2D + [char]0x6587 + "-" + [char]::ConvertFromUtf32(0x1F9EA))
  New-Item -ItemType Directory -Force -Path $unicodeDir | Out-Null
  Push-Location $unicodeDir
  try {
    $entryVersion = (& $entry --version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw "S1: entry --version failed" }
    $shimVersion = (& $defaultXlings --version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw "S1: healed default xlings shim failed from a unicode directory" }
    if ($shimVersion -ne $entryVersion) {
      throw "S1: healed shim --version ('$shimVersion') != entry --version ('$entryVersion')"
    }
    $helperOut = & $candidateShim | Out-String
    if ($LASTEXITCODE -ne 0) { throw "S1: healed candidate-helper shim failed from a unicode directory" }
    if ($helperOut -notmatch 'candidate-helper') {
      throw "S1: healed candidate-helper shim output missing 'candidate-helper'"
    }
  } finally { Pop-Location }

  # ---------------------------------------------------------------------
  # S2: self init heals stale shims too (it rebuilds the shim table from
  # scratch, same as install/use of the global scope).
  # ---------------------------------------------------------------------
  Write-Host "S2: self init also heals a stale shim"
  Remove-Item -Force $s2Xlings
  Copy-Item $legacyCopy $s2Xlings
  & $entry self init
  if ($LASTEXITCODE -ne 0) { throw "S2: self init failed" }
  Assert-SameAsEntry $s2Xlings "S2 self init"

  # ---------------------------------------------------------------------
  # S3: upgrading through the FIXED client re-points every shim as part of
  # replacing the entry -- this is the actual regression #615 describes.
  # ---------------------------------------------------------------------
  Write-Host "S3: upgrading through the fixed client re-points every shim"
  $payloadDir = Join-Path $work "payload"
  New-Item -ItemType Directory -Force -Path $payloadDir | Out-Null
  $payload = Join-Path $payloadDir "xlings.exe"
  Copy-Item $entry $payload
  Add-DistinguishingBytes $payload
  if ((Hash $payload) -eq (Hash $entry)) { throw "S3: payload is identical to the entry; the test proves nothing" }

  $fixtureRecipe = Join-Path $work "xlings-fixture.lua"
@'
package = {
    spec = "1",
    name = "xlings",
    description = "Local fixture for tests/e2e/windows_shim_identity_test.ps1",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["9.9.9"] = {} },
        macosx  = { ["9.9.9"] = {} },
        windows = { ["9.9.9"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    os.cp(os.getenv("XLINGS_FIXTURE_PAYLOAD"), path.join(bindir, "xlings.exe"))
    return true
end

function config()
    xvm.add("xlings", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("xlings")
    return true
end
'@ | Set-Content -NoNewline -Path $fixtureRecipe

  $env:XLINGS_FIXTURE_PAYLOAD = $payload
  try {
    & $entry config --add-xpkg $fixtureRecipe
    if ($LASTEXITCODE -ne 0) { throw "S3: fixture recipe import failed" }
    & $entry install local:xlings@9.9.9 --use -y
    if ($LASTEXITCODE -ne 0) { throw "S3: install --use of the fixture xlings failed" }
  } finally {
    Remove-Item Env:XLINGS_FIXTURE_PAYLOAD -ErrorAction SilentlyContinue
  }

  if ((Hash $entry) -ne (Hash $payload)) {
    throw "S3: the entry binary was not replaced by the upgrade"
  }
  # The regression: before the fix these three kept the PREVIOUS content.
  Assert-SameAsEntry $defaultXlings "S3 upgrade"
  Assert-SameAsEntry $candidateShim "S3 upgrade"
  Assert-SameAsEntry $s2Xlings "S3 upgrade"

  $doctorAfterUpgrade = Invoke-Captured $entry @('self', 'doctor')
  if ($doctorAfterUpgrade.Out -match '(?i)outdated shim') {
    Write-Host $doctorAfterUpgrade.Out
    throw "S3: self doctor still reports 'outdated shim' after the upgrade should have re-pointed every shim"
  }

  # ---------------------------------------------------------------------
  # S4: a stale shim that is itself a NEW build (carries the marker) hands
  # execution off to the entry at startup instead of running its own,
  # possibly-stale code.
  # ---------------------------------------------------------------------
  Write-Host "S4: a new-build stale shim hands off to the entry"
  $handoffDir = Join-Path $work "handoff"
  New-Item -ItemType Directory -Force -Path $handoffDir | Out-Null
  $handoff = Join-Path $handoffDir "xlings.exe"
  Copy-Item $entry $handoff
  Add-DistinguishingBytes $handoff
  if ((Hash $handoff) -eq (Hash $entry)) { throw "S4: handoff build is identical to the entry; the test proves nothing" }

  Remove-Item -Force $s2Xlings
  Copy-Item $handoff $s2Xlings

  $env:XLINGS_HANDOFF_TRACE = "1"
  try {
    $versionRun = Invoke-Captured $s2Xlings @('--version')
    if ($versionRun.Code -ne 0) {
      Write-Host $versionRun.Out
      throw "S4: stale new-build shim --version failed"
    }
    if ($versionRun.Out -notmatch 'xlings: handoff') {
      Write-Host $versionRun.Out
      throw "S4: stale new-build shim did not trace the handoff"
    }

    $shimUnknown = Invoke-Captured $s2Xlings @('no-such-subcommand-615')
    $entryUnknown = Invoke-Captured $entry @('no-such-subcommand-615')
    if ($shimUnknown.Code -eq 0 -or $entryUnknown.Code -eq 0) {
      Write-Host $shimUnknown.Out
      Write-Host $entryUnknown.Out
      throw "S4: an unknown subcommand unexpectedly exited 0"
    }
    if ($shimUnknown.Code -ne $entryUnknown.Code) {
      Write-Host $shimUnknown.Out
      Write-Host $entryUnknown.Out
      throw "S4: handoff exit code ($($shimUnknown.Code)) != entry exit code ($($entryUnknown.Code)) for the same unknown subcommand"
    }
  } finally {
    Remove-Item Env:XLINGS_HANDOFF_TRACE -ErrorAction SilentlyContinue
  }

  Write-Host "S4: self doctor --fix heals the new-build stale shim too"
  $fix2 = Invoke-Captured $entry @('self', 'doctor', '--fix')
  if ($fix2.Code -ne 0) {
    Write-Host $fix2.Out
    throw "S4: self doctor --fix failed"
  }
  Assert-SameAsEntry $s2Xlings "S4 fix"

  Write-Host "All shim-identity checks passed (#615)"
} finally { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }

# Land an explicit success rather than leaving whatever $LASTEXITCODE
# happens to hold from the last native call this script made (see
# smoke.ps1 / release_self_install_test.ps1 for the same pattern): a
# `pwsh -command ". script.ps1"` step reports this as its exit code.
exit 0
