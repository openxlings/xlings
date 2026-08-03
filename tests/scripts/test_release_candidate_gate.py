#!/usr/bin/env python3
from pathlib import Path
import re

text = (Path(__file__).resolve().parents[2] / ".github/workflows/release.yml").read_text()
jobs = ["candidate-linux-x86_64", "candidate-linux-aarch64",
        "candidate-macos-arm64", "candidate-windows-x86_64"]
for job in jobs:
    if not re.search(rf"^  {re.escape(job)}:", text, re.MULTILINE):
        raise SystemExit(f"missing release gate job: {job}")
match = re.search(r"^  create-release:\n    needs: \[([^]]+)\]", text, re.MULTILINE)
if not match or any(job not in match.group(1) for job in jobs):
    raise SystemExit("create-release is not gated by every native candidate")
prefix = text[:text.index("  create-release:")]
if "Generate source-bound sidecars" not in prefix:
    raise SystemExit("sidecars are not generated before candidate validation")
root = Path(__file__).resolve().parents[2]
smokes = [(root / "tests/candidate-install/smoke.sh").read_text(),
          (root / "tests/candidate-install/smoke.ps1").read_text()]
candidate_fixture = root / "tests/candidate-install/candidate-helper.lua"
if not candidate_fixture.is_file():
    raise SystemExit("candidate lifecycle fixture is missing")
for smoke in smokes:
    if "tests/fixtures/xim-pkgindex" in smoke:
        raise SystemExit("candidate smoke depends on an optional package-index clone")
    if "candidate-helper.lua" not in smoke:
        raise SystemExit("candidate smoke does not use the in-tree lifecycle fixture")
if "sed '0," in smokes[0]:
    raise SystemExit("candidate smoke uses a GNU-only sed address")
# Deliberately NOT asserting that the smoke scripts contain particular command
# strings. That kind of check misfires whenever someone refactors the script
# and stays green whenever the commands stop working, which is the opposite of
# what it looks like it is doing. What the smoke scripts actually do is
# verified by running them -- every PR workflow below does, on its own native
# platform. This file guards the *wiring*: that the gate jobs exist, that
# create-release cannot publish without them, and that the fixtures the smoke
# scripts need are in-tree rather than fetched by some other workflow step.
for workflow in ("xlings-ci-linux.yml", "xlings-ci-macos.yml",
                 "xlings-ci-windows.yml", "xlings-ci-aarch64.yml"):
    contents = (root / ".github/workflows" / workflow).read_text()
    if "candidate-install" not in contents or "smoke" not in contents:
        raise SystemExit(f"PR workflow does not reuse candidate smoke: {workflow}")
arm_workflow = (root / ".github/workflows/xlings-ci-aarch64.yml").read_text()
native_job = arm_workflow.split("  native-contracts:", 1)[1].split(
    "  cross-build-and-run:", 1)[0]
if "xlings install mcpp" in native_job:
    raise SystemExit("native aarch64 contracts depend on unavailable compiler assets")
if "aarch64_compat_contract_test.sh" not in native_job:
    raise SystemExit("native aarch64 job does not run the runtime contract")
if "prepare_fixture_index.sh" not in native_job:
    raise SystemExit("native aarch64 job does not prepare its package fixture")
# A PowerShell test whose last native command is one it EXPECTED to fail leaves
# $LASTEXITCODE non-zero, and `pwsh -command ". script.ps1"` hands that back as
# the step's exit code -- so the job goes red with "ok" printed directly above
# "Process completed with exit code 1". Each such script has to land its own
# exit. This has now happened twice; the guard covers all of them rather than
# the one that was noticed.
for powershell_test in sorted((root / "tests").rglob("*.ps1")):
    # tests/e2e/runtime/ holds leftover run artifacts, not tests -- including
    # whole extracted xlings homes with their own .ps1 files.
    if "runtime" in powershell_test.relative_to(root).parts:
        continue
    # Libraries are dot-sourced INTO a test; an `exit 0` there would end the
    # caller mid-run, which is the opposite of the fix.
    if powershell_test.stem.endswith("lib"):
        continue
    text = powershell_test.read_text()
    expects_failure = any(marker in text for marker in (
        "$LASTEXITCODE -ne 0", "-ne 37", "should fail", "must stop it",
        "ExitCode -eq 0", "throw \"a "))
    if expects_failure and not text.rstrip().endswith("exit 0"):
        raise SystemExit(
            f"{powershell_test.relative_to(root)} runs a command it expects to "
            f"fail but does not end with `exit 0`; the leaked $LASTEXITCODE "
            f"will fail the CI step even when the test passes")
for powershell_test in (
    root / "tests/candidate-install/smoke.ps1",
    root / "tests/e2e/fresh_xlings_home_install_test.ps1",
):
    if re.search(r"(?im)^\s*\$home\s*=", powershell_test.read_text()):
        raise SystemExit(f"PowerShell test overwrites read-only HOME: {powershell_test}")
portable_fixture_files = (
    root / "tests/e2e/remove_multi_version_test.sh",
    root / "tests/e2e/remove_self_guard_test.sh",
    root / "tests/e2e/fixtures/project_index/pkgs/n/node.lua",
    root / "tests/e2e/fixtures/project_index/pkgs/n/ninja.lua",
    root / "tests/e2e/fixtures/build_deps_split/bdconsumer.lua",
    root / "tests/e2e/fixtures/build_deps_split/bdtool.lua",
    root / "tests/e2e/fixtures/build_deps_split/rttool.lua",
)
for fixture_file in portable_fixture_files:
    archs = re.search(r"archs\s*=\s*\{([^}]*)\}", fixture_file.read_text())
    if not archs or '"aarch64"' not in archs.group(1):
        raise SystemExit(f"macOS E2E fixture omits aarch64: {fixture_file}")
print("release candidate gates: ok")
