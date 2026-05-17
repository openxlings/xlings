# T11 — xmake.lua: musl 全静态链接配置

> **Wave**: 1（无前置依赖，可与 T12 并行）
> **预估改动**: ~10 行
> **设计文档**: [../release-static-build.md](../release-static-build.md)

---

## 1. 任务概述

修改 `xmake.lua` 中 Linux 平台的链接配置，将 glibc 专用的动态链接 flags 替换为 musl 全静态链接 `-static`，并支持从 gcc SDK 兜底查找静态库。

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| 无 | xmake.lua 修改独立于 CI 配置 |

构建时需要 `musl-gcc@15.1.0` SDK 已安装（通过 `xlings install musl-gcc@15.1.0 -y`），但安装步骤属于 T12 的 CI 配置范畴。本地开发时需手动安装。

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `xmake.lua` | 修改 Linux 平台 ldflags |

---

## 4. 实施步骤

### 4.1 修改 Linux 链接配置

将 `xmake.lua` 第 21-27 行替换：

```lua
-- 当前代码:
elseif is_plat("linux") then
    -- Use system dynamic linker (glibc) so binary is not tied to SDK path (e.g. /home/xlings/.xlings_data/...)
    add_ldflags("-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2", {force = true})
    -- Static link stdc++/gcc for release so binary does not depend on SDK libs
    if not os.getenv("XLINGS_NOLINKSTATIC") then
        add_ldflags("-static-libstdc++", "-static-libgcc", {force = true})
    end
```

```lua
-- 改为:
elseif is_plat("linux") then
    if not os.getenv("XLINGS_NOLINKSTATIC") then
        add_ldflags("-static", {force = true})
    end
    local gcc_sdk = os.getenv("GCC_SDK")
    if gcc_sdk then
        add_linkdirs(gcc_sdk .. "/lib64", {force = true})
    end
```

### 4.2 变更说明

| 原 flag | 新 flag | 原因 |
|---------|---------|------|
| `-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2` | 删除 | 静态二进制无需动态链接器 |
| `-static-libstdc++` | `-static` | `-static` 已包含全部静态链接 |
| `-static-libgcc` | `-static` | 同上 |
| (无) | `GCC_SDK` linkdirs | 兜底：如 musl SDK 中缺少某个 `.a`，可从 gcc SDK 搜索 |

### 4.3 静态库查找逻辑

`-static` flag 告诉链接器优先使用 `.a` 静态库。链接器搜索顺序：

1. musl-gcc SDK 内置路径（由 `--sdk` 和 `--cross` 参数自动设置）：
   - `$MUSL_SDK/x86_64-linux-musl/lib/` — 含 `libc.a`, `libstdc++.a`, `libm.a` 等
   - `$MUSL_SDK/lib/gcc/x86_64-linux-musl/15.1.0/` — 含 `libgcc.a`, `libgcc_eh.a`
2. `GCC_SDK/lib64/`（可选兜底，通过环境变量 `GCC_SDK` 指定）：
   - 含 `libstdc++.a`, `libstdc++fs.a`, `libsupc++.a` 等

在实际验证中，musl-gcc@15.1.0 SDK 已包含所有必需的静态库，`GCC_SDK` 兜底通常不需要启用。

### 4.4 `XLINGS_NOLINKSTATIC` 环境变量

保留 `XLINGS_NOLINKSTATIC` 环境变量作为逃逸开关：设置后跳过 `-static`，用于本地调试需要动态链接的场景。

---

## 5. 验收标准

### 5.1 使用 musl-gcc SDK 编译成功

```bash
MUSL_SDK=/home/xlings/.xlings_data/xim/xpkgs/musl-gcc/15.1.0
xmake f -p linux -m release --sdk=$MUSL_SDK --cross=x86_64-linux-musl-
xmake build xlings
# 期望: [100%]: build ok
```

### 5.2 二进制为全静态

```bash
file build/linux/x86_64/release/xlings
# 期望包含: statically linked

ldd build/linux/x86_64/release/xlings
# 期望: not a dynamic executable
```

### 5.3 零 GLIBC 符号、无 RUNPATH

```bash
readelf --dyn-syms build/linux/x86_64/release/xlings | grep GLIBC
# 期望: 无输出

readelf -d build/linux/x86_64/release/xlings | grep RUNPATH
# 期望: 无输出
```

### 5.4 功能正常

```bash
./build/linux/x86_64/release/xlings -h
# 期望: 正常输出帮助信息
```

### 5.5 NOLINKSTATIC 逃逸开关仍可用

```bash
XLINGS_NOLINKSTATIC=1 xmake f -p linux -m release --sdk=$MUSL_SDK --cross=x86_64-linux-musl-
xmake build xlings
ldd build/linux/x86_64/release/xlings
# 期望: 动态链接 (显示 libc.so 依赖)
```
