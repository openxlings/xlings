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
if "sed '0," in smokes[0]:
    raise SystemExit("candidate smoke uses a GNU-only sed address")
for smoke in smokes:
    for contract in ("self install", "install candidate-helper",
                     "search candidate-helper", "use candidate-helper",
                     "info local:candidate-helper", "remove candidate-helper",
                     "subos new candidate-probe", "--sandbox"):
        if contract not in smoke:
            raise SystemExit(f"candidate smoke omits lifecycle contract: {contract}")
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
windows_contract = (root / "tests/e2e/subos_cmd_contract_test.ps1").read_text()
if not windows_contract.rstrip().endswith("exit 0"):
    raise SystemExit("Windows command contract leaks the expected exit 37")
for powershell_test in (
    root / "tests/candidate-install/smoke.ps1",
    root / "tests/e2e/fresh_xlings_home_install_test.ps1",
):
    if re.search(r"(?im)^\s*\$home\s*=", powershell_test.read_text()):
        raise SystemExit(f"PowerShell test overwrites read-only HOME: {powershell_test}")
print("release candidate gates: ok")
