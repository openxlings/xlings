# 从源码构建

[English](../90-build-from-source.md) | **简体中文**

xlings 全部使用 C++23 模块，由 [mcpp](https://github.com/mcpp-community/mcpp) 构建。构建依赖声明在仓库自己的
`.xlings.json` 里，由 xlings 安装。

## 构建

```bash
git clone https://github.com/openxlings/xlings.git && cd xlings
xlings install              # the toolchain the repository declares (mcpp, gcc)
mcpp build                  # xlings  -> target/<triple>/<fingerprint>/bin/xlings
mcpp build -p luban-tool    # luban   -> .../bin/luban-tool/luban
```

## 测试

```bash
mcpp test                   # unit and C++ end-to-end tests (tests/**/test_*.cpp)
mcpp build -p xdev
XDEV=$(find target -path '*/bin/xdev/xdev' | head -1)
$XDEV doctor                # which capabilities this machine can test
$XDEV test --suite lint     # the repository's rules (tools/lint_*.sh)
```

新测试用 testkit 写成 C++（`XTEST`，每个测试一个隔离的 home）。每个测试写明它覆盖的需求（`tests/requirements.toml`）。
在中国大陆运行 shell 测试套件时设置 `XLINGS_TEST_MIRROR=CN`，否则索引同步会一直等待连不上的主机。

## 发布构建

| 平台 | 脚本 |
|---|---|
| Linux（静态，musl） | `tools/linux_release.sh` |
| macOS（LLVM） | `tools/macos_release.sh` |
| Windows（MSVC） | `tools/windows_release.ps1` |

本地构建、CI 和发布用同一个 `.xlings.json`，所以用的是同一套工具。

## 参与贡献

issue、分支、带测试的 pull request；squash 合入，每项变更一个提交。提交信息格式为 `<type>(<scope>): <description>`。
