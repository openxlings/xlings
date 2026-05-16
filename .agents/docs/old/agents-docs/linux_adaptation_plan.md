# Linux 适配方案（基于 quick_install + xlings install gcc@15）

## 背景

当前项目已具备 macOS 构建路径。为补齐 Linux 适配，本方案目标是：

- 在 Linux 环境下可稳定构建 `xlings` 主程序。
- 可运行单测与 e2e 测试流程。
- 保持最小改动，不影响 macOS 既有构建链路。

本方案以用户指定流程为起点：

```bash
curl -fsSL https://raw.githubusercontent.com/d2learn/xlings/refs/heads/main/tools/other/quick_install.sh | bash
xlings install gcc@15 -y
```

## 现状结论

在仅安装 `gcc@15` 时，`xmake` 依赖包编译阶段会出现 Linux C 运行时/头文件缺失问题（如 `crt1.o`、`stdio.h`）。因此 Linux 构建需要工具链兜底策略：

1. **首选** `musl-gcc@15.1.0`（与现有 CI / release 一致）。
2. **Linux release 强制要求** `musl-gcc@15.1.0`（确保产物全静态，避免运行时依赖问题）。

## 任务拆分

### T1 - 文档化 Linux 适配路径
- [x] 新增 Linux 适配文档，明确目标、限制与任务。

### T2 - release 脚本强制 musl 全静态（最小改动）
- [x] `tools/linux_release.sh` 仅接受 `musl-gcc@15.1.0` 构建链路。
- [x] 明确要求全静态链接输出，避免 release 运行时依赖问题。
- [x] 增加静态产物校验（`file`/`ldd`）。

### T3 - Linux 构建与测试验证
- [x] 验证 C++ 构建成功（musl-gcc 路径 + release 脚本路径均已通过）。
- [x] 跑通单测（`xmake build xlings_tests && xmake run xlings_tests`，其中依赖外部 pkgindex 的用例按测试设计跳过）。
- [x] 跑通可执行的 release e2e（`release_self_install_test.sh`、`release_quick_install_test.sh`）。
- [ ] `bootstrap_home_test.sh` / `release_subos_smoke_test.sh` 依赖 `tests/fixtures/xim-pkgindex`，当前仓库快照缺失该目录。

## 实施策略（不影响 macOS）

- 仅修改 Linux release 脚本中的 Linux 工具链探测分支。
- 不更改 `xmake.lua` 的 macOS 分支。
- 不改动 macOS release/CI 文件。

## 建议执行顺序（Linux CI / 本地）

1. quick install 安装 xlings。
2. `xlings install gcc@15 -y`。
3. 若可用，补充安装 `musl-gcc@15.1.0` 作为主构建工具链。
4. 运行 `tools/linux_release.sh`；脚本强制使用 musl 全静态构建。
5. 执行单测与 e2e。


## 本轮执行结果

- `xlings install musl-gcc@15.1.0 -y` 已完成，并用于主构建链路。
- `tools/linux_release.sh`（`SKIP_NETWORK_VERIFY=1`）完成打包与自检。
- 单测可编译并运行通过；部分依赖外部 pkgindex 的测试按设计跳过。
- e2e 中 release 安装/quick-install 场景通过；依赖 fixtures 的场景待补齐 fixtures 后再跑。

## 关键说明：musl loader / runtime 链接动作是做什么的

在 Linux CI/release 中看到的以下动作：

- `/home/xlings/.xlings_data/lib/ld-musl-x86_64.so.1` 软链接
- `/lib/ld-musl-x86_64.so.1` 软链接
- `/etc/ld-musl-x86_64.path` 写入 musl 库路径
- `/lib/libstdc++.so.6`、`/lib/libgcc_s.so.1` 软链接

其目的不是改变发布包结构，而是为了让 **musl-gcc 工具链本身及其产物在构建机上可运行**：

1. 工具链里的 `cc1/as/collect2` 可能内置固定解释器路径；
2. musl 产物默认依赖 musl loader 与 runtime 搜索路径；
3. 若不补齐，构建阶段会出现“解释器不存在 / 运行时库找不到”导致编译或链接失败。

这些动作已统一收敛到 `tools/setup_musl_runtime.sh`，供 Linux CI、release workflow、`tools/linux_release.sh` 复用。

## Linux release 包结构与 macOS 对齐

> 说明：构建阶段强制 `musl-gcc` 全静态；交付阶段仍与 macOS 一样是 bootstrap 极简包。

Linux release 打包策略保持与 macOS 一致：

- 最小交付：`bin/xlings` + `.xlings.json`；
- 可直接在目录内使用（`self init` + PATH）；
- 也可执行安装（`self install`）后作为用户安装形态使用。

即：发布包仍是“单个主二进制 + 配置文件”的 bootstrap 形态，运行时目录按需生成，不在打包阶段预展开。
