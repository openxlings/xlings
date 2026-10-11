# Building from Source

**English** | [简体中文](zh/90-build-from-source.md)

xlings is C++23 with modules throughout, built by
[mcpp](https://github.com/mcpp-community/mcpp). Its build dependencies are
declared in the repository's own `.xlings.json`, so xlings installs them.

## Build

```bash
git clone https://github.com/openxlings/xlings.git && cd xlings
xlings install              # the toolchain the repository declares (mcpp, gcc)
mcpp build                  # xlings  -> target/<triple>/<fingerprint>/bin/xlings
mcpp build -p luban-tool    # luban   -> .../bin/luban-tool/luban
```

## Test

```bash
mcpp test                   # unit and C++ end-to-end tests (tests/**/test_*.cpp)
mcpp build -p xdev
XDEV=$(find target -path '*/bin/xdev/xdev' | head -1)
$XDEV doctor                # which capabilities this machine can test
$XDEV test --suite lint     # the repository's rules (tools/lint_*.sh)
```

New tests are C++ with the testkit (`XTEST`, an isolated home per test). Every
test names the requirements it covers (`tests/requirements.toml`). From China,
set `XLINGS_TEST_MIRROR=CN` for the shell suites, or index syncs wait on
unreachable hosts.

## Release builds

| Platform | Script |
|---|---|
| Linux (static, musl) | `tools/linux_release.sh` |
| macOS (LLVM) | `tools/macos_release.sh` |
| Windows (MSVC) | `tools/windows_release.ps1` |

The same `.xlings.json` drives local builds, CI and releases, so they build with
the same tools.

## Contributing

Issue, branch, a pull request with its tests; squash-merged, one commit per
change. Commit messages are `<type>(<scope>): <description>`.
