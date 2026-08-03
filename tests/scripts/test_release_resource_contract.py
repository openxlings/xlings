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
