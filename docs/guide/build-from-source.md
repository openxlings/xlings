# 从源码构建

> 更新日期: 2026-09-26 | 版本: 2026.9.26.3

xlings 使用 [mcpp](https://github.com/mcpp-community/mcpp) 作为构建工具,构建依赖通过 xlings 自身声明在 `.xlings.json` 中。

开发工具链由 `mcpp.toml` 固定为 gcc 16.1.0（macOS / Windows 为 LLVM 20.1.7）。静态发布包另见 `tools/linux_release.sh`、`tools/macos_release.sh` 和 `tools/windows_release.ps1`。

```bash
# 1. 先安装 xlings(见 README 的「快速开始」)
# 2. 在仓库根目录安装构建依赖:
xlings install           # 读取 .xlings.json → 安装 mcpp 构建工具链

# 3. 构建与测试:
mcpp build
mcpp test
```

首次构建会下载并编译 C++23 模块依赖，耗时通常超过一分钟；增量构建会复用 `target/` 下的产物。同一份 `.xlings.json` 同时驱动 CI 和 release 流水线，保证本地、CI 与发布环境一致。
