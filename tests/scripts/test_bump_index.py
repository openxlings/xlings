#!/usr/bin/env python3
"""tools/bump_index.py writes a release into the index (part 3, U3).

xlings, luban, luban-os and xim:xdesk have one writer of their versions: this
release. The entries it writes are the ones written by hand before it, byte for
byte; it writes all four recipes or none; a second run changes nothing.
"""
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/bump_index.py"
OLD, NEW = "2026.1.1.1", "2026.1.2.1"


def entry(version, digests):
    lines = "".join(f'                    {arch} = "{d}",\n' for arch, d in sorted(digests.items()))
    return (f'            ["{version}"] = {{\n                url = "XLINGS_RES",\n'
            f'                sha256 = {{\n{lines}                }},\n            }},\n')


def block(platform, version, digests, head=""):
    return (f'        {platform} = {{\n{head}            ["latest"] = {{ ref = "{version}" }},\n'
            f'{entry(version, digests)}        }},\n')


def recipe(blocks):
    return 'package = {\n    name = "x",\n\n    xpm = {\n' + "".join(blocks) + "    },\n}\n"


RECIPES = {
    "pkgs/x/xlings.lua": ("xlings", {"linux": ["x86_64", "aarch64"], "macosx": ["aarch64"], "windows": ["x86_64"]}),
    "pkgs/luban/luban.lua": ("luban", {"linux": ["x86_64", "aarch64"], "macosx": ["aarch64"], "windows": ["x86_64"]}),
    "pkgs/luban/luban-os.lua": ("luban-os", {"linux": ["x86_64"], "macosx": ["aarch64"], "windows": ["x86_64"]}),
    "pkgs/x/xdesk.lua": ("xdesk", {"linux": ["x86_64"]}),
}
ASSET = {("linux", "x86_64"): "linux-x86_64.tar.gz", ("linux", "aarch64"): "linux-aarch64.tar.gz",
         ("macosx", "aarch64"): "macosx-arm64.tar.gz", ("windows", "x86_64"): "windows-x86_64.zip"}
# A block may carry what is not a version (deps, a comment) before its latest.
HEAD = {"linux": '            deps = {\n                "xim:glibc",\n            },\n',
        "macosx": "            -- written by xlings' release\n"}


def digest(product, platform, arch, version):
    return hashlib.sha256(f"{product} {platform} {arch} {version}".encode()).hexdigest()


def index_at(work, version):
    for path, (product, platforms) in RECIPES.items():
        blocks = [block(p, version, {a: digest(product, p, a, version) for a in arches}, HEAD.get(p, ""))
                  for p, arches in platforms.items()]
        (work / path).parent.mkdir(parents=True, exist_ok=True)
        (work / path).write_text(recipe(blocks))


def sidecars(work, version, skip=None):
    out = work / "sha"
    out.mkdir(exist_ok=True)
    for product, platforms in RECIPES.values():
        for platform, arches in platforms.items():
            for arch in arches:
                name = f"{product}-{version}-{ASSET[(platform, arch)]}"
                if name != skip:
                    (out / f"{name}.sha256").write_text(f"{digest(product, platform, arch, version)}  {name}\n")
    return out


def run(work, version, sha):
    return subprocess.run([sys.executable, str(TOOL), version, str(work / "idx"), "--sha256-dir", str(sha)],
                          capture_output=True, text=True)


def expected(version_old, version_new):
    """What a hand-written bump looks like: latest moved, the new entry above the old one."""
    texts = {}
    for path, (product, platforms) in RECIPES.items():
        blocks = []
        for p, arches in platforms.items():
            old = block(p, version_old, {a: digest(product, p, a, version_old) for a in arches}, HEAD.get(p, ""))
            new = entry(version_new, {a: digest(product, p, a, version_new) for a in arches})
            moved = old.replace(f'{{ ref = "{version_old}" }},\n', f'{{ ref = "{version_new}" }},\n' + new)
            blocks.append(moved)
        texts[path] = recipe(blocks)
    return texts


def test_writes_every_platform_of_every_product_as_by_hand():
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        index_at(work / "idx", OLD)
        result = run(work, NEW, sidecars(work, NEW))
        assert result.returncode == 0, result.stderr
        for path, text in expected(OLD, NEW).items():
            got = (work / "idx" / path).read_text()
            assert got == text, f"{path}:\n{got}"


def test_a_second_run_changes_nothing():
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        index_at(work / "idx", OLD)
        sha = sidecars(work, NEW)
        assert run(work, NEW, sha).returncode == 0
        first = {p: (work / "idx" / p).read_text() for p in RECIPES}
        result = run(work, NEW, sha)
        assert result.returncode == 0, result.stderr
        assert "already there" in result.stdout, result.stdout
        assert first == {p: (work / "idx" / p).read_text() for p in RECIPES}


def test_a_missing_asset_writes_nothing():
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        index_at(work / "idx", OLD)
        before = {p: (work / "idx" / p).read_text() for p in RECIPES}
        missing = f"xdesk-{NEW}-linux-x86_64.tar.gz"
        result = run(work, NEW, sidecars(work, NEW, skip=missing))
        assert result.returncode != 0
        assert missing in result.stderr, result.stderr
        assert before == {p: (work / "idx" / p).read_text() for p in RECIPES}


def test_macos_arm64_assets_are_the_index_aarch64():
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        index_at(work / "idx", OLD)
        assert run(work, NEW, sidecars(work, NEW)).returncode == 0
        text = (work / "idx/pkgs/luban/luban-os.lua").read_text()
        mac = text[text.index("        macosx = {"):text.index("        windows = {")]
        assert f'aarch64 = "{digest("luban-os", "macosx", "aarch64", NEW)}"' in mac, mac


def test_an_older_version_is_added_without_moving_latest():
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        index_at(work / "idx", NEW)
        older = "2026.1.1.9"
        assert run(work, older, sidecars(work, older)).returncode == 0
        text = (work / "idx/pkgs/x/xdesk.lua").read_text()
        assert f'["latest"] = {{ ref = "{NEW}" }}' in text, text
        assert f'["{older}"] = {{' in text, text


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"ok   {name}")
            except AssertionError as error:
                print(f"FAIL {name}: {error}", file=sys.stderr)
                failures += 1
    if failures:
        raise SystemExit(f"{failures} bump_index contract(s) failed")
    print("bump_index: ok")
