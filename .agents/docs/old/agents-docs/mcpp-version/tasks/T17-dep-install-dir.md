# T17 — dep_install_dir API

> **优先级**: P1
> **Wave**: 2（依赖 T16 完成）
> **预估改动**: ~30 行 Lua
> **设计文档**: [../xim-issues-design.md](../xim-issues-design.md) §四

---

## 1. 任务概述

在 `libxpkg/pkginfo.lua` 中新增 `dep_install_dir(dep_name, dep_version)` API，让 xpkg.lua 能可靠地获取已安装依赖包的实际路径，替代硬编码命名空间猜测。

---

## 2. 问题分析

### 2.1 当前痛点

xpkg.lua 中经常需要引用其他已安装包的路径。例如 `glibc.lua` 需要知道自己的 relocate 前缀，`gcc.lua` 的依赖 glibc 路径等。当前只能通过硬编码猜测：

```lua
-- glibc.lua 中硬编码
local fromsource_glibc = "fromsource-x-" .. package.name
local glibc_path_prefix = path.join(xim_xpkgs_dir, fromsource_glibc, ...)
```

### 2.2 现有的 `pkginfo.install_dir(pkgname)` 已有部分实现

`libxpkg/pkginfo.lua` 已有一个接受参数的 `install_dir(pkgname, pkgversion)` 重载，但实现依赖 xvm info 的字符串解析，不够健壮：

```lua
function install_dir(pkgname, pkgversion)
    if pkgname then
        local pinfo = xvm.info(pkgname, pkgversion or "")
        local spath = pinfo["SPath"]
        -- 通过 split version 字符串推导 install_dir
        -- 问题: 当 version 字符串出现在路径多处时可能误切
    end
end
```

---

## 3. 依赖前置

| 依赖 | 原因 |
|------|------|
| T16 | namespace 统一后，xvm 中的注册信息更可靠 |

---

## 4. 实现方案

### 4.1 新增 `dep_install_dir()` API

文件: `core/xim/libxpkg/pkginfo.lua`

```lua
function dep_install_dir(dep_name, dep_version)
    -- 1. 尝试通过 xvm info 获取
    local pinfo = xvm.info(dep_name, dep_version or "")
    if pinfo and pinfo["SPath"] then
        local ver = pinfo["Version"]
        if ver then
            local strs = string.split(pinfo["SPath"], ver)
            if #strs >= 2 then
                return path.join(strs[1], ver)
            end
        end
    end

    -- 2. fallback: 扫描 xpkgs 目录
    local xpkgs_base = runtime.get_xim_install_basedir()
    local candidates = _find_dep_in_xpkgs(xpkgs_base, dep_name, dep_version)
    if candidates then
        return candidates
    end

    cprint("[xlings:xim]: ${yellow}dep_install_dir: cannot find %s@%s${clear}",
        tostring(dep_name), tostring(dep_version))
    return nil
end

function _find_dep_in_xpkgs(xpkgs_base, name, version)
    -- 按优先级尝试: name, *-x-name
    local try_patterns = {name}
    local dirs = os.dirs(path.join(xpkgs_base, "*"))
    if dirs then
        for _, d in ipairs(dirs) do
            local basename = path.basename(d)
            if basename:find(name, 1, true) then
                table.insert(try_patterns, basename)
            end
        end
    end

    for _, try_name in ipairs(try_patterns) do
        local dep_root = path.join(xpkgs_base, try_name)
        if os.isdir(dep_root) then
            if version then
                local ver_dir = path.join(dep_root, version)
                if os.isdir(ver_dir) then return ver_dir end
            else
                -- 取最新版本
                local vers = os.dirs(dep_root .. "/*")
                if vers and #vers > 0 then
                    table.sort(vers)
                    return vers[#vers]
                end
            end
        end
    end
    return nil
end
```

### 4.2 增强现有 `install_dir()` 的参数重载

保持向后兼容，当 `pkgname` 参数不为 nil 时代理到 `dep_install_dir()`：

```lua
function install_dir(pkgname, pkgversion)
    if pkgname then
        return dep_install_dir(pkgname, pkgversion)
    else
        return runtime.get_pkginfo().install_dir
    end
end
```

### 4.3 更新 xpkg.lua 使用新 API

例如 `glibc.lua:__relocate()`:

```lua
-- 改前
local fromsource_glibc = "fromsource-x-" .. package.name
local glibc_path_prefix = path.join(xim_xpkgs_dir, fromsource_glibc, pkginfo.version(), "lib")

-- 改后
local glibc_path_prefix = path.join(pkginfo.install_dir(), "lib")
```

---

## 5. 验收标准

### 5.1 API 功能验证

```bash
# 安装 gcc（依赖 glibc）
xlings install gcc -y

# 在 xpkg 环境中测试 API（通过临时脚本或调试）
# dep_install_dir("glibc") 应返回 xpkgs 中 glibc 的实际路径
# dep_install_dir("glibc", "2.39") 应返回 xpkgs/glibc/2.39/ 或 xpkgs/*-x-glibc/2.39/
```

### 5.2 xpkg.lua 中消除硬编码

```bash
# 代码审查: xpkg.lua 中不再有 xpkgs 路径的硬编码拼接
grep -rn "fromsource-x-\|scode-x-" ../xim-pkgindex/pkgs/
# 期望: 无输出（或已改为使用 pkginfo.dep_install_dir）
```

### 5.3 向后兼容性

```bash
# 现有 pkginfo.install_dir() 无参数调用行为不变
xlings install cmake -y
cmake --version
# 期望: 正常工作
```

### 5.4 验收检查表

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| `dep_install_dir(name)` 返回正确路径 | 单元测试 | [ ] |
| `dep_install_dir(name, version)` 返回正确路径 | 单元测试 | [ ] |
| 依赖不存在时返回 nil + 警告 | 功能测试 | [ ] |
| `install_dir()` 无参调用行为不变 | 回归测试 | [ ] |
| xpkg.lua 中无硬编码 namespace | 代码审查 | [ ] |
| gcc/glibc 安装后功能正常 | `gcc --version` | [ ] |
