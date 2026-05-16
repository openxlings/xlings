# T16 — 命名空间解析统一

> **优先级**: P1
> **Wave**: 2（依赖 T14 完成）
> **预估改动**: ~40 行 Lua
> **设计文档**: [../xim-issues-design.md](../xim-issues-design.md) §四

---

## 1. 任务概述

统一 xpkg 包命名空间到安装路径的映射逻辑，消除 `aggregate_dep_libs_to()` 和各 xpkg.lua 中硬编码的 namespace 前缀猜测。

---

## 2. 问题分析

### 2.1 当前 namespace → 目录映射

`XPkgManager.lua:_set_runtime_info()`:

```lua
local pkgname = xpkg.name
if xpkg.namespace then
    pkgname = xpkg.namespace .. "-x-" .. pkgname
end
install_dir = xpkgs_base / pkgname / version
```

结果：同一个逻辑包在不同 namespace 下有不同目录名：

| 场景 | 目录名 |
|------|--------|
| 无 namespace | `xpkgs/glibc/2.39/` |
| namespace = "scode" | `xpkgs/scode-x-glibc/2.39/` |
| namespace = "fromsource" | `xpkgs/fromsource-x-glibc/2.39/` |

### 2.2 消费者硬编码猜测

`CmdProcessor.lua:aggregate_dep_libs_to()`:

```lua
for _, try_name in ipairs({name, "scode-x-" .. name, "fromsource-x-" .. name}) do
    local dep_root = path.join(xpkgs, try_name)
    -- ...
end
```

`glibc.lua:__relocate()`:

```lua
local fromsource_glibc = "fromsource-x-" .. package.name
```

新增 namespace 时，所有消费者必须同步修改，维护成本高。

---

## 3. 依赖前置

| 依赖 | 原因 |
|------|------|
| T14 | 需要 xpkgs 复用机制到位后再优化路径查找 |

---

## 4. 实现方案

### 4.1 利用 xvm 注册信息查找依赖路径

xvm 注册时已经记录了包的安装路径。通过 `xvm info <name>` 可以获取 `SPath`（源路径），从中推导 install_dir。

改造 `aggregate_dep_libs_to()` 使用 xvm 查询而非硬编码猜测：

```lua
local function aggregate_dep_libs_to(deps_list, target_libdir)
    if not deps_list or not is_host("linux") or not target_libdir then return end

    for _, dep_spec in ipairs(deps_list) do
        local name = dep_spec:gsub("@.*", "")
        local ver_opt = dep_spec:find("@", 1, true) and dep_spec:match("@(.+)") or nil

        -- 优先通过 xvm 查询实际路径
        local install_dir = _resolve_dep_dir_via_xvm(name, ver_opt)

        -- fallback: 扫描 xpkgs 目录
        if not install_dir then
            install_dir = _resolve_dep_dir_via_scan(name, ver_opt)
        end

        if install_dir then
            _link_libs_from(install_dir, target_libdir)
        end
    end
end

local function _resolve_dep_dir_via_xvm(name, version)
    local pinfo = xvm.info(name, version or "")
    if pinfo and pinfo["SPath"] then
        local spath = pinfo["SPath"]
        local ver = pinfo["Version"]
        if ver then
            local strs = string.split(spath, ver)
            if #strs == 2 then
                return path.join(strs[1], ver)
            end
        end
    end
    return nil
end
```

### 4.2 xpkg.lua 中消除硬编码

对于 `glibc.lua:__relocate()` 中的硬编码：

```lua
-- 改前
local fromsource_glibc = "fromsource-x-" .. package.name
local glibc_path_prefix = path.join(xim_xpkgs_dir, fromsource_glibc, pkginfo.version(), "lib")

-- 改后: 使用当前包自身的 install_dir
local glibc_path_prefix = path.join(pkginfo.install_dir(), "lib")
```

relocate 的本质是替换编译时硬编码的路径前缀为当前安装路径。之前硬编码 `fromsource-x-glibc` 是因为 glibc 从源码编译时的安装路径使用了这个 namespace，但既然 relocate 的目标就是当前 install_dir，直接用 `pkginfo.install_dir()` 即可。

如果 relocate 需要替换的是"旧路径"到"新路径"，应该让构建过程记录旧路径，而非硬编码猜测。

---

## 5. 验收标准

### 5.1 依赖库聚合验证

```bash
# 安装带依赖的包（如 d2x 依赖 glibc、openssl）
xlings install d2x -y

# 验证 subos/lib 中正确聚合了依赖库
ls ~/.xlings/subos/default/lib/ | grep -E "libc|libssl"
# 期望: 存在 libc.so.6、libssl.so 等软链

# 验证 d2x 可正常运行
d2x --version
```

### 5.2 消除硬编码验证

```bash
# 代码审查: CmdProcessor.lua 中不再有 "scode-x-" 或 "fromsource-x-" 硬编码
grep -n "scode-x-\|fromsource-x-" core/xim/CmdProcessor.lua
# 期望: 无输出

# 代码审查: xpkg.lua 中不再硬编码其他包的 namespace
grep -rn "fromsource-x-" ../xim-pkgindex/pkgs/
# 期望: 无输出（或已改为使用 pkginfo API）
```

### 5.3 新 namespace 兼容性

```bash
# 添加一个自定义 namespace 的 index repo
xim --add-indexrepo myns:https://example.com/my-pkgindex.git

# 安装其中的包
xlings install myns:some-tool -y

# 验证依赖聚合正确
# 期望: 无需修改任何 aggregate 代码即可正常工作
```

### 5.4 验收检查表

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| `aggregate_dep_libs_to` 不含硬编码 namespace | 代码审查确认 | [ ] |
| 依赖库聚合通过 xvm 查询路径 | 功能测试通过 | [ ] |
| glibc relocate 不含硬编码 namespace | 代码审查确认 | [ ] |
| 带依赖包安装后可正常运行 | d2x --version 成功 | [ ] |
| 新 namespace 包无需改代码即可聚合 | 测试通过 | [ ] |
