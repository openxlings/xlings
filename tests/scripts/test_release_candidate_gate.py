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
print("release candidate gates: ok")
