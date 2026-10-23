#!/usr/bin/env python3
"""Write a release into the official index: xlings, luban, luban-os and xim:xdesk.

    tools/bump_index.py <version> <xim-pkgindex checkout> [--sha256-dir DIR]

Their versions have one writer, this release (xlings design 2026-10-23 part 3, R1):
each platform block of each recipe gets

    ["latest"] = { ref = "<version>" },
    ["<version>"] = {
        url = "XLINGS_RES",
        sha256 = { <arch> = "<digest>", ... },
    },

from the release's `.sha256` sidecars (`gh release download`, or the files in
--sha256-dir). Every digest is read before anything is written: a missing asset
writes nothing, so no platform points at a file that is not there. A block that
already has the version is left alone, so a second run changes nothing.
Run it after the CN mirror is complete (`tools/mirror-latest.sh`), then open
the index PR.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

# recipe -> asset prefix, platform block -> {arch in the recipe: asset suffix}.
# macOS assets say arm64, the index says aarch64.
RELEASE = {
    "pkgs/x/xlings.lua": ("xlings", {
        "linux": {"x86_64": "linux-x86_64.tar.gz", "aarch64": "linux-aarch64.tar.gz"},
        "macosx": {"aarch64": "macosx-arm64.tar.gz"},
        "windows": {"x86_64": "windows-x86_64.zip"},
    }),
    "pkgs/luban/luban.lua": ("luban", {
        "linux": {"x86_64": "linux-x86_64.tar.gz", "aarch64": "linux-aarch64.tar.gz"},
        "macosx": {"aarch64": "macosx-arm64.tar.gz"},
        "windows": {"x86_64": "windows-x86_64.zip"},
    }),
    "pkgs/luban/luban-os.lua": ("luban-os", {
        "linux": {"x86_64": "linux-x86_64.tar.gz"},
        "macosx": {"aarch64": "macosx-arm64.tar.gz"},
        "windows": {"x86_64": "windows-x86_64.zip"},
    }),
    "pkgs/x/xdesk.lua": ("xdesk", {
        "linux": {"x86_64": "linux-x86_64.tar.gz"},
    }),
}

LATEST = re.compile(r'(\n            \["latest"\] = \{ ref = ")([^"]+)(" \},\n)')
DIGEST = re.compile(r"^[0-9a-f]{64}$")


def asset_names(version):
    for _recipe, (prefix, blocks) in RELEASE.items():
        for arches in blocks.values():
            for suffix in arches.values():
                yield f"{prefix}-{version}-{suffix}"


def read_digests(version, sha256_dir):
    """Every asset's digest, or the names of the ones that are missing."""
    digests, missing = {}, []
    for asset in asset_names(version):
        sidecar = asset + ".sha256"
        if sha256_dir:
            path = Path(sha256_dir) / sidecar
            text = path.read_text() if path.is_file() else ""
        else:
            run = subprocess.run(["gh", "release", "download", f"v{version}", "-R", "openxlings/xlings",
                                  "-p", sidecar, "-O", "-"], capture_output=True, text=True)
            text = run.stdout if run.returncode == 0 else ""
        digest = text.split()[0] if text.split() else ""
        if DIGEST.match(digest):
            digests[asset] = digest
        else:
            missing.append(sidecar)
    return digests, missing


def block_span(text, platform):
    """The platform block of `xpm`: from its `<platform> = {` to the next platform's."""
    xpm = text.index("\n    xpm = {\n")
    start = re.compile(rf"\n        {platform} = \{{\n").search(text, xpm)
    if not start:
        return None
    end = re.compile(r"\n        [a-z]+ = \{\n").search(text, start.end())
    return start.end(), (end.start() if end else len(text))


def with_version(text, recipe, version, blocks, digests, prefix):
    for platform, arches in blocks.items():
        span = block_span(text, platform)
        if not span:
            raise SystemExit(f"{recipe}: no {platform} block under xpm")
        begin, end = span
        if f'\n            ["{version}"] = {{\n' in text[begin - 1:end]:
            continue
        latest = LATEST.search(text, begin - 1, end)
        if not latest:
            raise SystemExit(f"{recipe}: no [\"latest\"] in {platform}")
        entries = "".join(f'                    {arch} = "{digests[f"{prefix}-{version}-{suffix}"]}",\n'
                          for arch, suffix in sorted(arches.items()))
        entry = (f'{latest.group(1)}{version}{latest.group(3)}'
                 f'            ["{version}"] = {{\n'
                 f'                url = "XLINGS_RES",\n'
                 f'                sha256 = {{\n{entries}'
                 f'                }},\n'
                 f'            }},\n')
        text = text[:latest.start()] + entry + text[latest.end():]
    return text


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("version")
    parser.add_argument("index", type=Path)
    parser.add_argument("--sha256-dir", help="read <asset>.sha256 here instead of from the GitHub release")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", args.version):
        raise SystemExit(f"not a release version: {args.version}")

    digests, missing = read_digests(args.version, args.sha256_dir)
    if missing:
        print(f"bump_index: {len(missing)} sidecar(s) of {args.version} missing; nothing written:", file=sys.stderr)
        for name in missing:
            print(f"  {name}", file=sys.stderr)
        return 1

    updates = {}
    for recipe, (prefix, blocks) in RELEASE.items():
        path = args.index / recipe
        before = path.read_text(encoding="utf-8")
        after = with_version(before, recipe, args.version, blocks, digests, prefix)
        if after != before:
            updates[path] = after
    for path, text in updates.items():
        path.write_text(text, encoding="utf-8")
    changed = ", ".join(str(p.relative_to(args.index)) for p in updates) or "nothing (already there)"
    print(f"bump_index: {args.version}: {changed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
