# xlings 使用 mcpp 构建实测记录

> 日期: 2026-05-30 | 状态: Linux musl 静态二进制已跑通 | xlings 分支: `feat/mcpp-build-plan`

## 结论

已经在 xlings 仓库内新增 mcpp 构建入口，并用隔离的 `MCPP_HOME` /
`XLINGS_HOME` 验证 mcpp 可以完整构建出可运行的 xlings Linux musl 静态
二进制。

本次验证通过的产物:

```text
target/x86_64-linux-musl/416f32eb2054cfae/bin/xlings
```

验证命令:

```bash
KEEP_MCPP_RUNTIME=1 \
MCPP_BIN=/home/speak/workspace/github/mcpp-community/mcpp/target/x86_64-linux-gnu/afb76caa74c38c77/bin/mcpp \
bash tests/e2e/mcpp_build_xlings_test.sh
```

结果:

```text
Finished release [optimized] in 34.83s
[project-e2e] PASS: mcpp builds a runnable xlings binary
```

## xlings 侧改动

新增:

- `mcpp.toml`: xlings 的 mcpp manifest，默认工具链为 `gcc@16.1.0`，
  `x86_64-linux-musl` 目标使用 `gcc@15.1.0-musl` 和 static linkage。
- `mcpp/pkgs/*`: 仓库内本地 mcpp index，先承载 xlings 需要的 C 库包:
  `zlib`、`bzip2`、`lz4`、`zstd`、`xz`、`libarchive`。
- `mcpp/include/xlings_libarchive_config.h`: libarchive 的 xlings/musl
  专用配置头，避免依赖外部 configure 生成物。
- `tests/e2e/mcpp_build_xlings_test.sh`: 隔离验证脚本，运行时使用独立
  `tests/e2e/runtime/mcpp_build_xlings/{mcpp-home,xlings-home}`。

调整:

- `.gitignore`: 忽略 `.mcpp/`、`compile_commands.json` 等本地生成物。
- `src/platform/{linux,macos,unix,windows}.cppm`: 将 `import std` 等模块
  import 移到平台 `#if` 外层，满足 mcpp 当前的 module scanner 约束。

## 依赖构建策略

`libarchive` 继续保持 in-process 解压能力，不退回系统 `xz` / `tar` 工具。
本地 index 中的压缩库按源码包静态编译:

- `zlib`: 编译库源码，启用 `Z_HAVE_UNISTD_H`。
- `bzip2`: 编译库源码，排除 CLI。
- `lz4`: 编译 `lib/*.c`。
- `zstd`: 编译 common/compress/decompress/dictBuilder，启用
  `ZSTD_DISABLE_ASM=1`，避免未编译 `.S` 时出现 HUF asm 符号缺失。
- `xz`: 直接编译 `liblzma` 所需源码，并提供最小 config 宏。
- `libarchive`: 直接列出需要的 `libarchive/*.c`，包含 rar5 所需
  `archive_blake2s_ref.c` / `archive_blake2sp_ref.c`。

## mcpp 侧发现的问题

为了让 xlings 构建成立，mcpp 仓库需要配套修复。当前修复在:

```text
/home/speak/workspace/github/mcpp-community/mcpp
branch: feat/xlings-build-support
binary: target/x86_64-linux-gnu/afb76caa74c38c77/bin/mcpp
```

已验证的 mcpp 命令:

```bash
MCPP_HOME=/home/speak/.xlings/data/xpkgs/xim-x-mcpp/0.0.20 \
/home/speak/.xlings/data/xpkgs/xim-x-mcpp/0.0.30/bin/mcpp \
test -- --gtest_filter=Scanner.*:PmCompat.*:Toml.*

MCPP_HOME=/home/speak/.xlings/data/xpkgs/xim-x-mcpp/0.0.20 \
/home/speak/.xlings/data/xpkgs/xim-x-mcpp/0.0.30/bin/mcpp \
build --print-fingerprint
```

这些修复属于构建工具层:

- TOML parser 支持数组尾逗号。
- 项目 `.mcpp/.xlings.json` 初始化时保留本地 path index，并按项目目录解析相对路径。
- mcpp 通过 xlings project env 安装 custom/local index 包后，能从
  `.mcpp/.xlings/data/xpkgs` 读取 `xpkg.lua` 和安装路径。
- 兼容 nested namespace，例如 `mcpplibs.capi.lua` 与
  `mcpplibs.capi:lua` 的安装目录和去重。
- module scanner/plan/backend 增加 per-package local include dirs，并在
  编译/扫描规则中把本包 include 放在全局 include 前面。这个修复解决了
  `xz` 的 `"common.h"` 被错误解析到 `mbedtls/library/common.h` 的问题。

在这些 mcpp 修复合入并发布前，不应把 xlings CI 或 release 默认构建路径
切到已发布的 mcpp 包，否则 CI 会使用缺少上述修复的 mcpp 版本。

## 验证边界

已完成:

- Linux `x86_64-linux-musl` mcpp build。
- mcpp 构建产物为静态 ELF。
- 产物在隔离 `XLINGS_HOME` 下执行 `-h`，输出包含 `USAGE` 和
  `xlings [OPTIONS]`。

暂不声明完成:

- macOS / Windows mcpp 构建。
- release 脚本默认切换到 mcpp driver。
- 常规 CI job 默认启用 mcpp build。

原因是当前 xlings 成功依赖 mcpp 分支修复。下一步应先合入并发布 mcpp，
再在 xlings CI 中增加 non-blocking mcpp job，最后考虑
`XLINGS_BUILD_DRIVER=mcpp` 接入 release 脚本。

## 后续建议

1. 在 mcpp 仓库提交并合入 `feat/xlings-build-support`。
2. 发布包含上述修复的新 mcpp 版本。
3. 在 xlings Linux CI 中增加 `continue-on-error: true` 的 mcpp build e2e:
   `bash tests/e2e/mcpp_build_xlings_test.sh`。
4. Linux mcpp job 稳定后，再扩展 macOS / Windows。
5. 三平台稳定后，再让 `tools/*_release.*` 支持
   `XLINGS_BUILD_DRIVER=mcpp`。
