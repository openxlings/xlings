# T07 — xim Lua: 新增 `XLINGS_PKGDIR` 环境变量支持

> **Wave**: 1（依赖 T04 的设计约定，但代码上独立，可并行）
> **预估改动**: ~5 行 Lua，1 个文件

---

## 1. 任务概述

在 `core/xim/base/runtime.lua` 的 `get_xim_install_basedir()` 函数中，优先读取 `XLINGS_PKGDIR` 环境变量作为包安装目标目录。这使 xim 可以将包安装到全局共享的 `$XLINGS_HOME/xim/xpkgs/`，而不是受 `XLINGS_DATA` 限制的 per-env 目录。

**设计背景**: 详见 [../env-store-design.md §4.3](../env-store-design.md)（方案 A）

---

## 2. 依赖前置

无（Wave 1，代码层面独立。T04 完成后 C++ 侧会在启动时设置 `XLINGS_PKGDIR`）

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/xim/base/runtime.lua` | 修改 `get_xim_install_basedir()` |

---

## 4. 实施步骤

### 4.1 修改 `get_xim_install_basedir()`

当前实现（第 112-117 行）：

```lua
function get_xim_install_basedir()
    if __xim_install_basedir == nil then
        -- default xim install basedir
        __xim_install_basedir = path.join(get_xim_data_dir(), "xpkgs")
    end
    return __xim_install_basedir
end
```

修改为：

```lua
function get_xim_install_basedir()
    if __xim_install_basedir == nil then
        -- XLINGS_PKGDIR 优先（由 C++ 主程序在多环境模式下注入）
        -- 指向全局共享的 $XLINGS_HOME/xim/xpkgs/，所有 env 共用一份 store
        local pkgdir_env = os.getenv("XLINGS_PKGDIR")
        if pkgdir_env and os.isdir(pkgdir_env) then
            __xim_install_basedir = pkgdir_env
        else
            -- fallback: 兼容旧版，使用 $XLINGS_DATA/xim/xpkgs/
            __xim_install_basedir = path.join(get_xim_data_dir(), "xpkgs")
        end
    end
    return __xim_install_basedir
end
```

---

## 5. 注意事项

- `os.getenv()` 是 xmake Lua runtime 内置函数，无需额外 import
- `isdir()` 检查防止 `XLINGS_PKGDIR` 指向不存在的路径（避免首次运行时目录未创建）
- C++ 主程序（T02 完成后）负责在启动时设置 `XLINGS_PKGDIR`：
  ```cpp
  // config.cppm 中
  platform::set_env_variable("XLINGS_PKGDIR",
      (paths_.homeDir / "xim" / "xpkgs").string());
  ```
- 旧版数据路径（`$XLINGS_DATA/xim/xpkgs/`）作为 fallback，**无需迁移即可兼容旧版**

---

## 6. 验收标准

### 6.1 XLINGS_PKGDIR 生效验证

```bash
mkdir -p /tmp/test-pkgdir

# 设置 XLINGS_PKGDIR 指向自定义目录
export XLINGS_PKGDIR=/tmp/test-pkgdir

# 执行安装（需在 xlings 环境下运行 xmake xim）
xlings install dadk

# 验证包被安装到 XLINGS_PKGDIR 而非 XLINGS_DATA/xim/xpkgs/
ls /tmp/test-pkgdir/dadk/   # 期望: 存在对应版本目录
```

### 6.2 fallback 兼容验证

```bash
# 不设置 XLINGS_PKGDIR
unset XLINGS_PKGDIR

xlings install cmake
# 期望: 包安装到 $XLINGS_DATA/xim/xpkgs/cmake/（旧版行为不变）
```

### 6.3 路径不存在时的 fallback

```bash
export XLINGS_PKGDIR=/nonexistent/path
xlings install cmake
# 期望: fallback 到默认路径，无报错（isdir 检查保护）
```
