# RPATH 迁移方案总结与原理说明

> 关联文档：[hybrid-libpath-order-subplan.md](hybrid-libpath-order-subplan.md)、[elf-relocation-and-subos-design.md](elf-relocation-and-subos-design.md)、[elfpatch-shrink-rpath-mode.md](elfpatch-shrink-rpath-mode.md)

## 1. 问题回顾

### 1.1 LD_LIBRARY_PATH 传染问题

旧方案中 `xvm-shim` 在运行时组装 `LD_LIBRARY_PATH` 并注入子进程环境：

```
用户执行 xlings install d2x
  → xvm-shim 设置 LD_LIBRARY_PATH=subos/current/lib
    → xlings (C++ 二进制)
      → xmake (Lua 构建系统)
        → curl (下载工具) ← 继承了 LD_LIBRARY_PATH，execve() 失败！
```

`LD_LIBRARY_PATH` 是进程级环境变量，通过 `fork+exec` 传播给整棵进程树。当 `subos/current/lib` 为空或包含不兼容的库时，系统工具（如 `curl`）无法正常执行。

### 1.2 根本原因

```
# 环境变量天然具有"传染性"
export LD_LIBRARY_PATH=/my/libs     # 设置一次
/bin/curl https://example.com       # curl 继承了，可能加载错误的 libssl
gcc -o hello hello.c                # gcc 也继承了，cc1plus 可能加载错误的 libc
```

没有任何机制可以让 `LD_LIBRARY_PATH`（或 macOS 的 `DYLD_LIBRARY_PATH`）只对特定的一个程序生效而不影响它的子进程——这是 Unix 进程模型的固有限制。

## 2. 解决方案：RPATH 唯一真相

### 2.1 核心思想

将库搜索路径从"运行时环境变量"改为"安装时写入二进制"：

```
迁移前                                 迁移后
─────                                 ─────
shim 运行时组装 LD_LIBRARY_PATH   →   elfpatch 安装时写入 RPATH
环境变量传播给所有子进程           →   RPATH 只对当前二进制生效
无法观测（需调试开关）            →   readelf -d / otool -l 直接查看
仅支持 Linux                     →   Linux (ELF RUNPATH) + macOS (Mach-O LC_RPATH)
```

### 2.2 RPATH 为什么不传染

RPATH 是二进制文件头部的一个字段（Linux ELF 的 RUNPATH、macOS Mach-O 的 LC_RPATH），由动态链接器（`ld-linux.so` / `dyld`）在加载**当前程序**时读取，**不会传递给子进程**：

```
# 模拟示例：RPATH 不传染

# Linux: d2x 二进制的 RUNPATH 指向自己的依赖
$ readelf -d /data/xpkgs/d2x/0.1.1/bin/d2x | grep RUNPATH
  RUNPATH  /data/xpkgs/d2x/0.1.1/lib:/data/xpkgs/openssl/3.1.5/lib64:/subos/current/lib

# macOS: d2x 二进制的 LC_RPATH 指向自己的依赖
$ otool -l /data/xpkgs/d2x/0.1.3/bin/d2x | grep -A2 LC_RPATH
  path /data/xpkgs/d2x/0.1.3/lib
  path /data/xpkgs/openssl/3.1.5/lib
  path /subos/current/lib

# 两个平台上，d2x 启动子进程时，子进程看不到 d2x 的 RPATH
$ d2x --exec /usr/bin/curl https://example.com
# curl 不受 d2x 的 RPATH 影响，使用自己的系统库 → 正常工作
```

## 3. 实现架构

### 3.1 整体流程

```
┌─────────────────────────────────────────────────────────────────┐
│                        安装阶段 (一次性)                          │
│                                                                 │
│  xlings install d2x@0.1.1                                      │
│  │                                                              │
│  ├─ 1. 下载 & 解压到 data/xpkgs/d2x/0.1.1/                      │
│  │                                                              │
│  ├─ 2. elfpatch.auto() 扫描安装目录中的二进制文件                    │
│  │   │                                                          │
│  │   ├─ closure_lib_paths() 计算 RPATH（跨平台共用）：              │
│  │   │   ├─ ① data/xpkgs/d2x/0.1.1/lib          (自身)          │
│  │   │   ├─ ② data/xpkgs/glibc/2.38/lib64        (dep: glibc)   │
│  │   │   ├─ ③ data/xpkgs/openssl/3.1.5/lib64     (dep: openssl) │
│  │   │   └─ ④ subos/current/lib                   (fallback)    │
│  │   │                                                          │
│  │   ├─ Linux: patchelf --set-rpath "①:②:③:④" bin/d2x           │
│  │   │         patchelf --set-interpreter /subos/.../ld-linux.so │
│  │   └─ macOS: install_name_tool -add_rpath ①②③④ bin/d2x        │
│  │             install_name_tool -change 绝对路径 → @rpath/name  │
│  │                                                              │
│  └─ 3. xvm.add("d2x") 注册到 shim 系统                           │
│                                                                 │
├─────────────────────────────────────────────────────────────────┤
│                        运行阶段 (每次执行)                         │
│                                                                 │
│  用户执行: d2x --version                                         │
│  │                                                              │
│  ├─ xvm-shim 查找 d2x 的版本和路径                                │
│  ├─ 设置 PATH（含 d2x 的 bindir）                                │
│  ├─ 不设置 LD_LIBRARY_PATH / DYLD_LIBRARY_PATH（XVM_ENV_NULL）    │
│  └─ exec(data/xpkgs/d2x/0.1.1/bin/d2x)                         │
│      │                                                          │
│      └─ ld-linux.so / dyld 读取 RPATH，加载正确的库               │
│         子进程不受影响                                             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 关键代码路径

**安装时 — elfpatch 写入 RPATH（跨平台）：**

```lua
-- d2x.lua install()
function install()
    -- ... 解压到 install_dir ...
    elfpatch.auto({enable = true, shrink = true})
    -- elfpatch 内部流程（跨平台共用 closure_lib_paths）：
    -- 1. closure_lib_paths() → [自身lib, dep_glibc_lib, dep_openssl_lib, subos/lib]
    -- 2. Linux: _patch_elf()  → patchelf --set-rpath + --set-interpreter
    --    macOS: _patch_macho() → install_name_tool -add_rpath + -change
    -- 3. Linux 可选 shrink: patchelf --shrink-rpath
    return true
end
```

**运行时 — shim 不注入 LD_LIBRARY_PATH：**

```rust
// shims.rs — get_ld_library_path_env()
pub fn get_ld_library_path_env(&self) -> (String, String) {
    // RPATH 是唯一真相，shim 永远不设置 LD_LIBRARY_PATH
    ("XVM_ENV_NULL".to_string(), String::new())
}
```

**运行时 — 旧字段静默忽略：**

```rust
// shims.rs — add_env()
pub fn add_env(&mut self, key: &str, value: &str) {
    if key == "PATH" {
        // ... 正常处理 PATH ...
    } else if key == "XLINGS_PROGRAM_LIBPATH" || key == "XLINGS_EXTRA_LIBPATH" {
        // 静默忽略已废弃字段，保证旧 xpkg 不报错
    } else {
        // 其他环境变量（含 LD_LIBRARY_PATH）直接传递
        self.envs.push((key.to_string(), value.to_string()));
    }
}
```

## 4. 对比示例：d2x 包的完整生命周期

### 4.1 d2x 的依赖关系

```
d2x@0.1.1
├── glibc (系统级 C 运行时)
└── openssl@3.1.5 (TLS 库)
```

### 4.2 安装后的二进制状态（模拟）

**Linux (ELF)：**

```bash
$ patchelf --print-rpath data/xpkgs/d2x/0.1.1/bin/d2x
/home/user/.xlings/data/xpkgs/d2x/0.1.1/lib:/home/user/.xlings/data/xpkgs/glibc/2.38/lib64:/home/user/.xlings/data/xpkgs/openssl/3.1.5/lib64:/home/user/.xlings/subos/current/lib

$ readelf -l data/xpkgs/d2x/0.1.1/bin/d2x | grep interpreter
  [Requesting program interpreter: /home/user/.xlings/subos/current/lib/ld-linux-x86-64.so.2]
```

**macOS (Mach-O)：**

```bash
$ otool -l data/xpkgs/d2x/0.1.3/bin/d2x | grep -A2 LC_RPATH
  cmd LC_RPATH
  path /home/user/.xlings/data/xpkgs/d2x/0.1.3/lib
  cmd LC_RPATH
  path /home/user/.xlings/data/xpkgs/openssl/3.1.5/lib
  cmd LC_RPATH
  path /home/user/.xlings/subos/current/lib

$ otool -L data/xpkgs/d2x/0.1.3/bin/d2x
  @rpath/libssl.3.dylib       (patched from absolute path)
  @rpath/libcrypto.3.dylib    (patched from absolute path)
  /usr/lib/libSystem.B.dylib  (system lib, unchanged)
```

### 4.3 运行时行为对比

```bash
# ===== 迁移前（LD_LIBRARY_PATH 方案）=====
$ strace -e trace=execve d2x --download https://example.com/file.tar.gz
# xvm-shim 设置:
#   LD_LIBRARY_PATH=/.../openssl/3.1.5/lib64:/.../glibc/2.38/lib64:/.../subos/lib
# d2x 内部调用 curl:
execve("/usr/bin/curl", ["curl", "-L", "-o", "file.tar.gz", "https://..."])
# curl 继承了 LD_LIBRARY_PATH → 可能加载错误版本的 libssl → execve failed(-1)

# ===== 迁移后（RPATH 方案）=====
$ strace -e trace=execve d2x --download https://example.com/file.tar.gz
# xvm-shim 不设置 LD_LIBRARY_PATH
# d2x 通过 RUNPATH 加载自己的 openssl → 正常
# d2x 内部调用 curl:
execve("/usr/bin/curl", ["curl", "-L", "-o", "file.tar.gz", "https://..."])
# curl 没有 LD_LIBRARY_PATH → 使用系统 libssl → 正常
```

## 5. 库路径 4 级优先级

RPATH 写入二进制后，动态链接器按以下顺序搜索共享库（Linux 和 macOS 一致）：

```
优先级    来源                    示例路径                              何时命中
──────   ──────                  ──────                               ──────
  1      包自身 lib              data/xpkgs/d2x/0.1.1/lib            包自带的私有库
  2      依赖闭包                data/xpkgs/openssl/3.1.5/lib64      deps 声明的依赖库
  3      subos/lib (fallback)    subos/current/lib                    视图层聚合的库
  4      系统默认                /lib/x86_64-linux-gnu 或 /usr/lib    系统库
```

`closure_lib_paths()` 生成 1-3 级路径（跨平台共用），第 4 级由 `ld-linux` / `dyld` 默认处理。

### 5.1 多版本并存示例

```
同一 subos 下，A 和 C 依赖不同版本的 libB：

程序 A (RUNPATH 写入 ELF):
  data/xpkgs/A/1.0/lib
  data/xpkgs/B/0.0.1/lib64    ← A 的 RUNPATH 指向 B@0.0.1
  subos/current/lib

程序 C (RUNPATH 写入 ELF):
  data/xpkgs/C/2.0/lib
  data/xpkgs/B/0.0.2/lib64    ← C 的 RUNPATH 指向 B@0.0.2
  subos/current/lib

A 和 C 各自加载正确版本的 libB，互不干扰。
（LD_LIBRARY_PATH 方案下两者共享同一个环境变量，无法隔离）
```

## 6. 唯一例外：musl-ldd / musl-loader

`musl-gcc.lua` 中的 `musl-ldd` 和 `musl-loader` 是唯一需要 `LD_LIBRARY_PATH` 的场景：

```lua
-- musl-ldd 是 alias 命令，通过 shell 脚本调用 libc.so（musl 动态链接器）
xvm.add("musl-ldd", {
    bindir = musl_lib_dir,
    alias = "libc.so --list",  -- 不是直接执行 ELF，而是通过 xvm-alias 脚本
    envs = {
        LD_LIBRARY_PATH = musl_lib_dir,  -- 直接传递，不经过组装
    },
})
```

为什么不能用 RPATH：
- `musl-ldd` 通过 `xvm-alias`（shell 脚本）调用 `libc.so`
- Shell 脚本不是 ELF 文件，无法写入 RPATH
- `libc.so` 本身就是动态链接器，有自己的加载语义

为什么不传染：
- musl 动态链接器消费 `LD_LIBRARY_PATH` 后不传递给被分析的目标程序
- 这两个命令是诊断工具，不会启动需要 curl/xmake 等的复杂子进程树

数据流：

```
用户执行: musl-ldd /path/to/binary
  → xvm-shim
    → 设置 LD_LIBRARY_PATH=/.../x86_64-linux-musl/lib （仅此一项）
    → exec xvm-alias 脚本
      → libc.so --list /path/to/binary
        → musl 链接器读取 LD_LIBRARY_PATH，列出依赖
        → 结束（无子进程）
```

## 7. CI 防线

### 7.1 静态检查（check-no-direct-ld-libpath.sh）

```bash
# Check 1: 拒绝已废弃的字段
rg "XLINGS_(PROGRAM|EXTRA)_LIBPATH" pkgs/ --glob "*.lua"
# → 发现即报错

# Check 2: LD_LIBRARY_PATH 仅允许白名单
rg "LD_LIBRARY_PATH\s*=" pkgs/ --glob "*.lua"
# → 只有 pkgs/m/musl-gcc.lua 允许通过
```

### 7.2 E2E 测试（libpath_order_test.sh）

```bash
# Test 1: 废弃字段被静默忽略
xvm add test 0.0.1 --alias env \
  --env "XLINGS_PROGRAM_LIBPATH=/tmp/a" \
  --env "XLINGS_EXTRA_LIBPATH=/tmp/b"
xvm info test 0.0.1
# 输出中不应包含 XLINGS_PROGRAM_LIBPATH 或 XLINGS_EXTRA_LIBPATH

# Test 2: shim 不注入 LD_LIBRARY_PATH
# 输出中不应包含 LD_LIBRARY_PATH=

# Test 3: 直接 LD_LIBRARY_PATH 正常传递（例外路径）
xvm add test2 0.0.1 --alias env \
  --env "LD_LIBRARY_PATH=/exception/path"
xvm info test2 0.0.1
# 输出中应包含 LD_LIBRARY_PATH=/exception/path
```

## 8. 平台支持矩阵

| 能力 | Linux | macOS | Windows |
|------|-------|-------|---------|
| 二进制格式 | ELF | Mach-O | PE/COFF |
| elfpatch RPATH 写入 | patchelf | install_name_tool | 不适用 |
| per-program 闭包隔离 | RUNPATH | LC_RPATH + @rpath | 不可能（靠 PATH） |
| 依赖引用修正 | 不需要（DT_NEEDED 是纯文件名） | -change 绝对路径为 @rpath/ | 不需要 |
| INTERP/loader 修正 | patchelf --set-interpreter | 不需要（dyld 固定） | 不适用 |
| shim 不注入库路径变量 | XVM_ENV_NULL | XVM_ENV_NULL | N/A（用 PATH） |
| 诊断工具 | readelf -d / patchelf --print-rpath | otool -L / otool -l | N/A |

## 9. 与业界方案对比

| 工具 | 库路径策略 | xlings 对标 |
|------|-----------|------------|
| **Nix** | 所有二进制 patchelf 写入闭包 RPATH | 相同：elfpatch + closure_lib_paths() |
| **Guix** | 同 Nix，基于 RPATH | 相同 |
| **Homebrew (macOS)** | install_name_tool 修正 dylib 引用 | 相同：macOS 分支使用同一工具 |
| **Flatpak** | namespace 隔离，挂载独立 /usr | 不同：xlings 不使用 namespace |
| **AppImage** | 内嵌 ld.so + 相对 RPATH | 部分相似：xlings 可设置 subos loader |
| **Spack** | RPATH + 环境 module 混合 | 部分相似：xlings 已放弃环境变量部分 |
| **Conda** | LD_LIBRARY_PATH（已知问题） | xlings 避免了此问题 |

## 10. 变更记录

| 提交 | 内容 |
|------|------|
| `86764cc` | 初始实现：hybrid libpath composition + T23 docs |
| `0616955` | 临时修复：仅在显式声明时组装 LD_LIBRARY_PATH |
| `9ce622e` | 完整迁移：移除 compose_library_path，elfpatch subos/lib fallback |
| `08f85ec` | 测试更新：重写 e2e 测试验证 RPATH-only 策略 |
| (pending) | macOS 支持：elfpatch 添加 Mach-O 分支（install_name_tool） |
