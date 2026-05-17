# subos 目录统一设计方案

## 背景与目标

当前 `subos/<name>/` 存在两套库目录并行的冗余结构：
- `lib/` —— xvm 管理的 `.so` 符号链接
- `linux/lib/` —— xim 聚合的 `.so` **物理副本**（占用大量磁盘空间）
- `linux/usr/` —— 头文件**物理副本**（glibc、linux-headers、openssl）

目标：**将 `subos/<name>/` 本身作为 sysroot**，采用标准 FHS 目录布局，消除冗余，全平台统一。

---

## 目录结构对比

```
当前结构                                目标结构
subos/<name>/                          subos/<name>/        ← XLINGS_SUBOS = sysroot
├── bin/      ← xvm-shim 物理副本      ├── bin/             ← 硬链接（Unix）/ 副本（Windows）
├── lib/      ← xvm symlinks           ├── lib/             ← 所有库的符号链接（合并）
├── linux/                             ├── usr/
│   ├── lib/  ← 重复的物理副本         │   └── include/     ← 头文件（符号链接或副本）
│   └── usr/  ← 头文件物理副本         ├── xvm/             ← 不变
├── xvm/                               └── generations/     ← 不变
└── generations/
```

---

## 核心设计原则

`subos/<name>/` 本身即为 sysroot，等价于系统的 `/usr/local/`：

| 目录 | 用途 |
|---|---|
| `bin/` | 工具 shim（xvm-shim 硬链接）|
| `lib/` | 所有动态/静态库的符号链接 |
| `usr/include/` | 所有包头文件（符号链接或副本）|
| `xvm/` | xvm 版本配置（不参与 sysroot）|
| `generations/` | 回滚快照（未来功能）|

GCC 的 `--sysroot` 只读取已知子目录（`lib/`、`usr/include/` 等），`xvm/`、`generations/` 会被自动忽略，不产生干扰。

---

## 跨平台分析

| 平台 | `bin/` | `lib/` | `usr/include/` | 运行时库路径 |
|---|---|---|---|---|
| Linux | hardlink 到 xvm-shim | `.so` symlinks | glibc/openssl 头文件 | `LD_LIBRARY_PATH=$XLINGS_SUBOS/lib` |
| macOS | hardlink 到 xvm-shim | `.dylib` symlinks | macOS SDK 头文件 | `DYLD_LIBRARY_PATH=$XLINGS_SUBOS/lib` |
| Windows | DLL + shim 副本 | `.lib` 导入库 | 头文件副本 | PATH 包含 `bin/`（DLL 放 bin/）|

> Windows 平台：DLL 按 Windows 惯例放在 `bin/` 中随 PATH 解析；`aggregate_dep_libs_to` 函数已有 `if not is_host("linux")` 守卫，Windows 下不执行聚合，各包通过 xvm 的 per-package PATH env 配置自行处理。

---

## 关键分析：现有代码已正确

以下代码**无需修改**，已经指向正确路径：

| 文件 | 现状 |
|---|---|
| `core/config.cppm` | `libDir = subosDir / "lib"` ✓ |
| `core/xim/platform.lua` | `xlings_lib_dir = path.join(xlings_subos, "lib")` ✓ |
| `core/xvm/src/baseinfo.rs` | `libdir() = xvm_homedir().join("lib")` ✓ |
| `core/xvm/xvmlib/shims.rs` | `link_to()` 将 `.so` symlink 放入 `libdir()` ✓ |
| `pkgindex/gcc-specs-config.lua` | 使用 `sysrootdir/lib` 和 `sysrootdir/usr` ✓（改完 sysrootdir 自动正确）|
| `pkgindex/glibc.lua` | 复制到 `sysrootdir/usr` ✓ |
| `pkgindex/openssl.lua` | 复制到 `sysrootdir/usr/include` ✓ |
| `pkgindex/linux-headers.lua` | 复制到 `sysrootdir/usr` ✓ |
| `pkgindex/d2x.lua` | `LD_LIBRARY_PATH = sysrootdir/lib` ✓ |

---

## 需要修改的 5 个位置

### 1. `core/xim/libxpkg/system.lua` — `subos_sysrootdir()`

这是整个方案的**核心枢纽**，改完后所有 pkgindex 脚本无需修改即可自动正确。

```lua
-- 修改前
function subos_sysrootdir()
    local osname = os.host()
    if is_host("linux") then osname = "linux" end
    return path.join(platform.get_config_info().subosdir, osname)
end

-- 修改后
function subos_sysrootdir()
    return platform.get_config_info().subosdir
end
```

### 2. `core/xim/CmdProcessor.lua` — `aggregate_dep_libs_to`

将库聚合目标从 `linux/lib/` 改为 `lib/`，将物理复制改为符号链接：

```lua
-- 修改前（约第 316 行）
local target_lib = path.join(cfg.subosdir, "linux", "lib")
-- aggregate_dep_libs_to 内部：
os.cp(f, path.join(target_libdir, path.filename(f)), {force = true})

-- 修改后
local target_lib = path.join(cfg.subosdir, "lib")
-- aggregate_dep_libs_to 内部：
os.ln(f, path.join(target_libdir, path.filename(f)), {force = true})
```

效果：消除 `linux/lib/` 下的物理副本，合并进已有的 `lib/` 符号链接目录，节省大量磁盘空间。

### 3. `core/subos.cppm` — `create()` 函数

新建 subos 时创建 `usr/` 目录：

```cpp
// 在 create() 中添加
fs::create_directories(dir / "bin");
fs::create_directories(dir / "xvm");
fs::create_directories(dir / "usr");        // 新增
fs::create_directories(dir / "generations");
```

### 4. `core/xself.cppm` — `cmd_init()` 和 `cmd_migrate()`

- `cmd_init()`：在 `dirs` 列表（约第 21 行）中加入 `subosDir / "usr"`
- `cmd_migrate()`：在 `defaultDir` 目录创建（约第 126 行）后加入 `defaultDir / "usr"`

### 5. `tools/linux_release.sh` — 目录创建

```bash
# 修改前
mkdir -p "$OUT_DIR/subos/default/linux/lib"
mkdir -p "$OUT_DIR/subos/default/linux/usr"

# 修改后
mkdir -p "$OUT_DIR/subos/default/usr"
```

删除所有对 `linux/` 子目录的引用和验证步骤。

---

## GCC/Clang 集成

改造完成后，编译器可以直接以 `$XLINGS_SUBOS` 为 sysroot：

```bash
# 方式 A：--sysroot（gcc 自动在 $XLINGS_SUBOS/usr/include 和 $XLINGS_SUBOS/lib 下查找）
gcc --sysroot=$XLINGS_SUBOS ...

# 方式 B：显式指定（更直接，不依赖 --sysroot 行为）
export CPATH="$XLINGS_SUBOS/usr/include"
export LIBRARY_PATH="$XLINGS_SUBOS/lib"
export LD_LIBRARY_PATH="$XLINGS_SUBOS/lib"
```

`gcc-specs-config.lua` 包已使用 `sysrootdir/lib` 和 `sysrootdir/usr`，改完 `subos_sysrootdir()` 后自动生效。

---

## 可选优化：bin/ 硬链接

`core/xvm/xvmlib/shims.rs` 的 `try_create()` 目前是物理复制。可改为优先尝试硬链接：

```rust
// 先尝试硬链接，失败则回退到复制
if fs::hard_link(&src, &dst).is_err() {
    fs::copy(&src, &dst)?;
}
```

效果：`bin/` 下所有工具 shim 共享同一 inode（xvm-shim 只存一份），节省 `N × sizeof(xvm-shim)` 空间。此优化独立于主方案，可单独执行。

---

## 实施顺序

```
1. system.lua（subos_sysrootdir 改为返回 subosdir）
        ↓
2. CmdProcessor.lua（目标目录 + os.ln）
        ↓
3. subos.cppm + xself.cppm（目录创建）
        ↓
4. linux_release.sh（mkdir + 验证步骤）
        ↓
5. [可选] shims.rs（硬链接优化）
```

步骤 3 和 4 可并行执行；步骤 1 必须先于步骤 2。

---

## 验收标准

1. `xlings self init` 后 `subos/default/` 下存在 `bin/`、`lib/`、`usr/`、`xvm/`、`generations/`，**不存在 `linux/`**
2. `xlings install glibc` 后头文件出现在 `subos/default/usr/include/` 下（符号链接或副本）
3. `xlings install openssl` 后 `subos/default/usr/include/openssl/` 存在
4. `xlings install d2x` 后 `d2x` 可正常运行，`LD_LIBRARY_PATH` 指向 `subos/default/lib`
5. `xlings subos new test` 创建的新 subos 包含 `usr/` 目录
6. `linux_release.sh` 打包验证通过，无 `linux/` 相关错误
7. `du -sh subos/default/` 对比改前应显著减小（消除物理副本）
