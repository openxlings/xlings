# 修复: Shim alias 自引用导致递归（防御性修复）

## 问题

在 subos 环境中安装带 alias 的包（如 gcc）后，shim 调用 `platform::exec()` 执行 alias 命令时，
命令中的裸程序名通过 PATH 解析，在特定异常条件下可能解析回 shim 自身，导致递归到 depth=8。

## 根因分析

### PATH 构建顺序

alias 分支（`shim.cppm:184-206`）构建的 PATH 顺序为：

```
expanded_path : bin_path(expanded_path/bin) : cfg_bin(shim_dir) : original_PATH
```

其中 `cfg_bin` 即 shim 目录。**package 目录（expanded_path、bin_path）排在 shim_dir 之前**，
因此在正常安装情况下，真实二进制文件会被优先找到，不会触发递归。

### 实际触发递归的条件

递归仅在以下异常情况下触发——即 package 目录中找不到真实二进制，PATH 继续搜索到 shim_dir：

1. **安装不完整** — 如 elfpatch 修复 `PT_INTERP` 失败，二进制未正确部署到 package 目录
2. **真实二进制被删除或损坏** — 用户手动操作或磁盘问题导致 package 目录中的文件缺失
3. **alias 命令名与 package 目录中的文件名不匹配** — 配置错误，alias 指向的程序名在 package 目录中不存在

### 修复定性

这是一个**防御性修复**，而非核心逻辑 bug。正常安装路径下不会触发递归，
但在上述异常条件下，旧代码会无限递归到 depth=8 才退出，且无明确错误信息。
修复后，alias 命令直接解析为完整路径，彻底消除 PATH 顺序依赖；
当找不到真实二进制时，输出清晰的错误信息而非静默递归。

## 修复方案

在 alias 分支执行命令前，提取 alias 的第一个词（命令名），通过 `resolve_executable()`
解析为完整路径，替换到命令字符串中：

1. 解析成功 → 使用完整路径替换命令名，避免 PATH 查找
2. 解析失败且命令名 == program_name → 明确报错（自引用但找不到真实二进制）
3. 解析失败且命令名 != program_name → 保持原样（可能是系统命令）

## 修改文件

| 文件 | 操作 |
|------|------|
| `src/core/xvm/shim.cppm` | alias 分支添加路径解析 |
| `tests/unit/test_main.cpp` | 新增 2 个 XvmShimTest 用例 |
| `tests/e2e/shim_alias_recursion_test.sh` | 新增 E2E 测试 |

## 验证

1. `rm -rf build && xmake build` — 编译通过
2. `xmake run xlings-test` — 单元测试通过
3. `bash tests/e2e/shim_alias_recursion_test.sh` — E2E 测试通过
