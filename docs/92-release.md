# Releasing

**English** | [简体中文](zh/92-release.md)

How a version of xlings reaches users. A release is not finished when the
release workflow is green: two steps run outside it.

## The version

`YYYY.M.D.N`, `N` from 1; `.0` is reserved for a formal release. The version is
in three places that must agree — `mcpp.toml`, `src/core/config.cppm` and
`luban/os/service/src/service.cppm` (the Luban OS app's): luban and luban-os
are released with the xlings beside them.

## Steps

1. The change is merged (squash) to `main`.
2. The release workflow builds Linux (x86_64, aarch64), macOS and Windows,
   and publishes the GitHub release with a `.sha256` per asset; luban is also
   its own asset (`luban-<version>-<platform>`, `bin/luban` of each xlings archive).
3. **The China mirror**: the assets are pushed to GitCode from a machine in
   China (`tools/mirror-latest.sh xlings`, then `luban`, `luban-os`, `xdesk`), and each is verified by downloading
   it (GET, not HEAD) and comparing its sha256.
4. **The index**: `pkgs/x/xlings.lua` in xim-pkgindex gets the new version for
   every platform with its sha256, and `latest` points at it; so do
   `pkgs/luban/luban.lua`, `luban-os.lua` and `xdesk.lua`.
5. CI runs on `main` again, and the published index artifact is rebuilt.
   What tests the published experience waits for it: `xlings-ci-luban-fresh`,
   started by the release, first waits until the published index's `xlings`
   and `luban-os` `latest` are this version.
6. The release is verified for real: a fresh install, the host matrix, the
   index's acceptance.

## What CI never does

CI never pins a released xlings version for the fresh-install suite: it installs
what `latest` resolves to, which is what a first-time user gets
(`tests/fresh-install/no_xlings_version_pin_check.sh` enforces it).
