"""The version lives in two files and only one of them is ever read.

`src/core/config.cppm` is what the release workflow reads and what the binary
reports; `mcpp.toml` is the build manifest. Nothing forces them to agree, so a
bump touches the first and quietly leaves the second behind -- which is how
2026.8.4.2 shipped with a manifest still saying 2026.8.4.1.

The drift is harmless the moment it happens and confusing forever after: the
manifest is what a contributor reads to answer "what version is this tree".
"""
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _config_version() -> str:
    text = (ROOT / "src/core/config.cppm").read_text(encoding="utf-8")
    match = re.search(r'VERSION\s*=\s*"([^"]+)"', text)
    assert match, "no VERSION in src/core/config.cppm"
    return match.group(1)


def _manifest_version() -> str:
    text = (ROOT / "mcpp.toml").read_text(encoding="utf-8")
    match = re.search(r'^version\s*=\s*"([^"]+)"', text, re.MULTILINE)
    assert match, "no version in mcpp.toml"
    return match.group(1)


def test_manifest_version_tracks_the_released_version():
    config, manifest = _config_version(), _manifest_version()
    assert config == manifest, (
        f"mcpp.toml says {manifest} but src/core/config.cppm says {config}. "
        f"The release reads config.cppm, so this drift does not break a build "
        f"-- it just makes the manifest lie about which tree this is. Bump both."
    )


def test_version_has_four_components_starting_at_one():
    """YYYY.M.D.N, N starting at 1 -- .0 is reserved for milestone releases."""
    version = _config_version()
    parts = version.split(".")
    assert len(parts) == 4, f"expected YYYY.M.D.N, got {version!r}"
    assert all(p.isdigit() for p in parts), f"non-numeric component in {version!r}"
    assert int(parts[3]) >= 1, f"N starts at 1; .0 is reserved ({version!r})"


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
        raise SystemExit(f"{failures} version-consistency contract(s) failed")
    print("version consistency: ok")
