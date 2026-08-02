from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_release_publishes_sha256_sidecars():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    assert "Generate source-bound sidecars" in workflow
    assert "artifacts/xlings-release-sidecars/*.sha256" in workflow


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
