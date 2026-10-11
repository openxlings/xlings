# Releasing

**English** | [简体中文](zh/92-release.md)

How a version of xlings reaches users. A release is not finished when the
release workflow is green: two steps run outside it.

## The version

`YYYY.M.D.N`, `N` from 1; `.0` is reserved for a formal release. The version is
in three places that must agree — `mcpp.toml`, `src/core/config.cppm` and
`luban/src/cli/cli.cppm` (`test_luban_cli` checks it): luban is released with the
xlings beside it.

## Steps

1. The change is merged (squash) to `main`.
2. The release workflow builds Linux (x86_64, aarch64), macOS and Windows,
   and publishes the GitHub release with a `.sha256` per asset.
3. **The China mirror**: the assets are pushed to GitCode from a machine in
   China (`tools/mirror-latest.sh xlings`), and each is verified by downloading
   it (GET, not HEAD) and comparing its sha256.
4. **The index**: `pkgs/x/xlings.lua` in xim-pkgindex gets the new version for
   every platform with its sha256, and `latest` points at it.
5. CI runs on `main` again, and the published index artifact is rebuilt.
6. The release is verified for real: a fresh install, the host matrix, the
   index's acceptance.

## What CI never does

CI never pins a released xlings version for the fresh-install suite: it installs
what `latest` resolves to, which is what a first-time user gets
(`tests/fresh-install/no_xlings_version_pin_check.sh` enforces it).
