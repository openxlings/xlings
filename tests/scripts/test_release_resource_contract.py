import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_release_publishes_sha256_sidecars():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    assert "gen_release_sidecars.sh" in workflow
    assert "artifacts/xlings-release-sidecars/*.sha256" in workflow


def test_published_sidecars_verify_where_the_user_checks_them():
    """A sidecar records a digest AND a filename, and `sha256sum -c` resolves
    that filename relative to wherever the user is standing. Generating it as
    `sha256sum artifacts/<job>/<file>` bakes in a build-runner path, so every
    published sidecar carries the right digest and fails verification anyway.

    Run the generator and check the sidecar the way a user would: next to the
    archive, in a directory with no relation to where it was produced.
    """
    import shutil
    import subprocess
    import tempfile

    checker = shutil.which("sha256sum") or shutil.which("shasum")
    if checker is None:
        return  # no checker on this host; the generator picks the same one

    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        artifacts = temp / "artifacts" / "xlings-linux-x86_64"
        artifacts.mkdir(parents=True)
        asset = artifacts / "xlings-9999.0.0.1-linux-x86_64.tar.gz"
        asset.write_bytes(b"not really a tarball, but it hashes the same way")

        sidecars = temp / "sidecars"
        subprocess.run(
            ["bash", str(ROOT / "tools/gen_release_sidecars.sh"),
             str(temp / "artifacts"), str(sidecars)],
            check=True, capture_output=True, text=True)

        # What a user downloads: the archive and its sidecar, side by side,
        # somewhere else entirely.
        elsewhere = temp / "download"
        elsewhere.mkdir()
        shutil.copy(asset, elsewhere / asset.name)
        shutil.copy(sidecars / f"{asset.name}.sha256",
                    elsewhere / f"{asset.name}.sha256")
        # The build tree is gone by the time anyone downloads. Without this the
        # check passes against a sidecar that names the producing path, because
        # that path is still sitting there in the fixture.
        shutil.rmtree(temp / "artifacts")

        args = [checker, "-c", f"{asset.name}.sha256"]
        if checker.endswith("shasum"):
            args = [checker, "-a", "256", "-c", f"{asset.name}.sha256"]
        result = subprocess.run(args, cwd=elsewhere, capture_output=True,
                                text=True)
        assert result.returncode == 0, (
            "published sidecar does not verify next to its archive:\n"
            f"{(sidecars / (asset.name + '.sha256')).read_text()}"
            f"{result.stdout}{result.stderr}")


def test_xlings_resource_mirror_includes_sha256_sidecars():
    script = (ROOT / "tools/mirror_res.sh").read_text()
    xlings_case = script.split("  xlings)", 1)[1].split("    ;;", 1)[0]
    assert "linux-x86_64.tar.gz.sha256" in xlings_case
    assert "linux-aarch64.tar.gz.sha256" in xlings_case
    assert "macosx-arm64.tar.gz.sha256" in xlings_case
    assert "windows-x86_64.zip.sha256" in xlings_case


def test_index_bump_ignores_outer_github_workspace():
    script = (ROOT / "tools/bump_index.sh").read_text()
    assert '--workspace "$PWD" --apply --only "$PROJ"' in script


def test_luban_is_released_as_its_own_payload():
    """xim:luban installs luban-<ver>-<platform>: made from the xlings archives,
    uploaded with the release and mirrored to xlings-res/luban with sidecars."""
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    assert "tools/luban_release.sh artifacts artifacts/luban" in workflow
    for plat in ("linux-x86_64.tar.gz", "linux-aarch64.tar.gz",
                 "macosx-arm64.tar.gz", "windows-x86_64.zip"):
        assert f"artifacts/luban/luban-*-{plat}" in workflow
    assert 'mirror_res.sh luban "' in workflow
    script = (ROOT / "tools/mirror_res.sh").read_text()
    luban_case = script.split("  luban)", 1)[1].split("    ;;", 1)[0]
    for sidecar in ("linux-x86_64.tar.gz.sha256", "linux-aarch64.tar.gz.sha256",
                    "macosx-arm64.tar.gz.sha256", "windows-x86_64.zip.sha256"):
        assert sidecar in luban_case


def _xlings_archive(directory, plat, files):
    import io
    import tarfile
    import zipfile

    top = f"xlings-9999.0.0.1-{plat}"
    if plat.startswith("windows"):
        path = directory / f"{top}.zip"
        with zipfile.ZipFile(path, "w") as z:
            for name, data in files.items():
                # Windows PowerShell 5.1 writes backslashes.
                z.writestr(f"{top}\\{name}".replace("/", "\\"), data)
        return path
    path = directory / f"{top}.tar.gz"
    with tarfile.open(path, "w:gz") as t:
        for name, data in files.items():
            info = tarfile.TarInfo(f"{top}/{name}")
            info.size, info.mode = len(data), 0o755
            t.addfile(info, io.BytesIO(data))
    return path


def test_luban_payload_is_the_luban_of_the_xlings_archive():
    import subprocess
    import tarfile
    import tempfile
    import zipfile

    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        artifacts = temp / "artifacts"
        (artifacts / "a").mkdir(parents=True)
        _xlings_archive(artifacts / "a", "linux-x86_64",
                        {"bin/xlings": b"xlings", "bin/luban": b"luban-linux"})
        _xlings_archive(artifacts / "a", "windows-x86_64",
                        {"bin/xlings.exe": b"xlings", "bin/luban.exe": b"luban-win"})
        out = temp / "out"
        subprocess.run(["bash", str(ROOT / "tools/luban_release.sh"), str(artifacts), str(out)],
                       check=True, capture_output=True, text=True,
                       env={**os.environ,
                            "LUBAN_RELEASE_PLATFORMS": "linux-x86_64 windows-x86_64"})

        with tarfile.open(out / "luban-9999.0.0.1-linux-x86_64.tar.gz") as t:
            member = t.getmember("luban-9999.0.0.1-linux-x86_64/bin/luban")
            assert member.mode & 0o111
            assert t.extractfile(member).read() == b"luban-linux"
            assert not any(m.name.endswith("/xlings") for m in t.getmembers())
        with zipfile.ZipFile(out / "luban-9999.0.0.1-windows-x86_64.zip") as z:
            assert z.read("luban-9999.0.0.1-windows-x86_64/bin/luban.exe") == b"luban-win"


def test_luban_payload_refuses_an_archive_without_luban():
    import subprocess
    import tempfile

    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        (temp / "artifacts").mkdir()
        _xlings_archive(temp / "artifacts", "linux-x86_64", {"bin/xlings": b"xlings"})
        result = subprocess.run(["bash", str(ROOT / "tools/luban_release.sh"),
                                 str(temp / "artifacts"), str(temp / "out")],
                                capture_output=True, text=True,
                                env={**os.environ,
                                     "LUBAN_RELEASE_PLATFORMS": "linux-x86_64 macosx-arm64"})
        assert result.returncode != 0
        assert "has no xlings-9999.0.0.1-linux-x86_64/bin/luban" in result.stderr
