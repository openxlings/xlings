# 混合视图子方案：RPATH 唯一真相与库路径优先级

> 父文档: [xpkgs-subos-hybrid-design.md](xpkgs-subos-hybrid-design.md)
> 关联任务: [tasks/T23-hybrid-view-impl.md](tasks/T23-hybrid-view-impl.md)

## 1. 子方案目标

将父方案中的库解析优先级变成可执行约束，而不是文档约定：

1. 程序专属闭包路径（RPATH 内嵌于 ELF/Mach-O）
2. 依赖闭包路径（RPATH 内嵌于 ELF/Mach-O）
3. `subos/lib` 默认聚合路径（RPATH fallback）
4. 系统默认搜索路径（ld-linux / dyld 默认行为）

本子方案解决三个问题：

- 如何在运行时稳定保证该顺序
- 如何避免 xpkg 包定义绕过顺序
- 如何用 CI 和测试防回归

## 2. 设计原则

- **RPATH 是唯一真相**：库搜索路径写入二进制文件（Linux ELF 的 RUNPATH、macOS Mach-O 的 LC_RPATH），不依赖环境变量
- **shim 不注入库路径变量**：`xvm-shim` 不再组装或设置 `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH`，消除环境变量传染
- **elfpatch 是执行机制**：Linux 通过 `patchelf`、macOS 通过 `install_name_tool`，在安装时将闭包路径写入 RPATH
- **显式例外最小化**：仅对无法使用 RPATH 的特殊场景（如 musl-ldd alias wrapper）允许直接设置 `LD_LIBRARY_PATH`，且必须通过 `envs` 字段显式声明

## 3. 运行时保证机制（RPATH 实现）

### 3.1 elfpatch 自动 RPATH 写入

在包安装阶段，`elfpatch.apply_auto()` 自动扫描安装目录中的二进制文件，按平台分别处理：

**Linux（ELF）**：通过 `patchelf` 写入：
- **INTERP**（动态链接器）：指向 subos 或系统 loader
- **RUNPATH**：由 `closure_lib_paths()` 生成的闭包路径

**macOS（Mach-O）**：通过 `install_name_tool` 写入：
- **LC_RPATH**：由 `closure_lib_paths()` 生成的闭包路径（逐条 `-add_rpath`）
- **LC_LOAD_DYLIB 修正**：将绝对路径依赖改为 `@rpath/<basename>`（`-change`）
- 不需要修正 INTERP（macOS `dyld` 位置固定）

### 3.2 闭包路径生成规则

`elfpatch.closure_lib_paths()` 按以下固定顺序生成 RPATH：

1. **包自身的 lib 目录**（`install_dir/lib64` 或 `install_dir/lib`）
2. **依赖闭包路径**（按 `deps_list` 顺序，每个依赖的 `lib64` 或 `lib`）
3. **subos/lib 聚合路径**（作为 fallback，保证未声明闭包的库也能通过视图层解析）

去重保序（first-win），空路径过滤。

### 3.3 shim 层行为

`xvm-shim`（`core/xvm/xvmlib/shims.rs`）的 `get_ld_library_path_env()` 始终返回空值（`XVM_ENV_NULL`），**不再组装任何 LD_LIBRARY_PATH / DYLD_LIBRARY_PATH**。此行为在 Linux 和 macOS 上一致。

已删除的组件：
- 常量 `ENV_PROGRAM_LIBPATH`、`ENV_EXTRA_LIBPATH`
- `Program` 结构体字段 `program_libpath_env`、`extra_libpath_env`
- 函数 `compose_library_path()`
- 相关单元测试

### 3.4 LD_LIBRARY_PATH 的显式例外

对于无法使用 RPATH 的场景，包定义可通过 `envs` 字段直接设置 `LD_LIBRARY_PATH`。该值通过 `Program::envs` → `build_extended_envs` 传递给子进程，**不经过任何组装逻辑**。

当前唯一例外：`musl-gcc.lua` 中的 `musl-ldd` 和 `musl-loader` 命令。

原因：这两个命令是 alias wrapper（`alias = "libc.so --list"` / `alias = "libc.so"`），通过 shell 脚本调用 musl 动态链接器本身，无法使用 RPATH。musl 动态链接器会消费 `LD_LIBRARY_PATH` 而非传递给子进程，因此不会产生传染。

```lua
xvm.add("musl-ldd", {
    version = "musl-gcc-" .. pkginfo.version(),
    bindir = musl_lib_dir,
    alias = "libc.so --list",
    envs = {
        LD_LIBRARY_PATH = musl_lib_dir,
    },
    binding = binding_tree_root,
})
```

## 4. xpkg 规范（输入约束）

### 4.1 已废弃字段

以下字段已被移除，不再被 shim 识别：

- ~~`XLINGS_PROGRAM_LIBPATH`~~：已删除，shim 会静默忽略
- ~~`XLINGS_EXTRA_LIBPATH`~~：已删除，shim 会静默忽略

### 4.2 当前规范（红线）

- xpkg 包定义层**禁止**直接设置 `LD_LIBRARY_PATH`（除已记录的例外）
- 库路径通过 `elfpatch.auto()` 在安装时写入 RUNPATH
- 包作者只需声明 `deps`（依赖），`elfpatch` 自动计算闭包路径
- 如需显式 RPATH，在 `install()` 中调用 `elfpatch.patch_elf_loader_rpath()`

### 4.3 最简单示例

#### 标准包（使用 elfpatch 自动 RPATH）

```lua
function install()
    -- ... 解压/安装逻辑 ...
    elfpatch.auto({enable = true, shrink = true})
    return true
end
```

elfpatch 会根据 `deps` 自动生成 RPATH，无需手动指定路径。

#### 特殊情况（需要 LD_LIBRARY_PATH 的 alias 命令）

```lua
xvm.add("musl-ldd", {
    bindir = musl_lib_dir,
    alias = "libc.so --list",
    envs = {
        LD_LIBRARY_PATH = musl_lib_dir,  -- 已记录的例外
    },
})
```

### 4.4 项目内真实示例

- `xim-pkgindex/pkgs/d/d2x.lua`
  - 使用 `elfpatch.auto({enable=true, shrink=true})`
  - 依赖 `glibc` + `openssl@3.1.5`，RPATH 由 elfpatch 自动生成
- `xim-pkgindex/pkgs/g/glibc.lua`
  - 基础运行时库，通过视图层暴露
- `xim-pkgindex/pkgs/m/musl-gcc.lua`
  - `musl-ldd` / `musl-loader` 使用直接 `LD_LIBRARY_PATH`（唯一例外）

## 5. 校验与防回归

### 5.1 静态检查

CI 中的 `check-no-direct-ld-libpath.sh` 脚本：

- 扫描 xpkg 文件，若发现直接写 `LD_LIBRARY_PATH`，仅允许已记录的例外（`musl-ldd`、`musl-loader`）
- 拒绝 `XLINGS_PROGRAM_LIBPATH` 和 `XLINGS_EXTRA_LIBPATH`（已废弃字段）
- 扫描关键可执行文件，检查 `RUNPATH/INTERP` 不含构建机私有路径

### 5.2 运行时集成测试

最小回归用例：

- A 依赖 `b@0.0.1`，C 依赖 `b@0.0.2`
- 同一 subos 下启动 A/C，验证各自 RUNPATH 指向不同版本的库
- 不启用闭包的旧程序仍可通过 `subos/lib`（RPATH fallback）正常运行

### 5.3 诊断

**Linux**：使用 `readelf -d <binary>` 或 `patchelf --print-rpath <binary>` 可直接查看写入 ELF 的 RPATH。

**macOS**：使用 `otool -l <binary> | grep -A2 LC_RPATH` 查看 LC_RPATH 条目，`otool -L <binary>` 查看依赖引用是否已改为 `@rpath/` 前缀。

## 6. 与 T23 的映射

- T23-A：elfpatch 闭包路径生成 + subos/lib fallback
- T23-B：shim 层清除 LD_LIBRARY_PATH 组装逻辑
- T23-C：CI 静态检查更新 + 多版本并存集成测试

## 7. 验收标准

- 文档中的 4 级优先级可由 RUNPATH/LC_RPATH 字段唯一推导
- 任意程序的 RPATH 可通过 `readelf -d`（Linux）或 `otool -l`（macOS）直接观测
- xpkg 直写 `LD_LIBRARY_PATH` 在 CI 被拦截（已记录例外除外）
- `XLINGS_PROGRAM_LIBPATH` 和 `XLINGS_EXTRA_LIBPATH` 在 CI 被拦截
- 多版本并存测试稳定通过
- shim 执行基础设施工具（xmake、curl 等）时不传染 LD_LIBRARY_PATH / DYLD_LIBRARY_PATH