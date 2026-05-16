# T19 — remove 时引用检查，防止误删共享 xpkgs

> **优先级**: P0
> **Wave**: 1（可与 T14 并行）
> **预估改动**: ~25 行 Lua（1 个文件）
> **设计文档**: [../xim-issues-design.md](../xim-issues-design.md) §三

---

## 1. 任务概述

修改 `XPkgManager:uninstall()` 使其在删除 xpkgs 物理文件前，扫描所有 subos 的 xvm 注册表，确认没有其他 subos 在使用该包。有引用时仅注销当前 subos 的 xvm 注册，保留 xpkgs 文件。

---

## 2. 问题背景

当前 `XPkgManager:uninstall()` 在执行 uninstall hook 后**无条件删除** xpkgs 目录：

```lua
-- core/xim/pm/XPkgManager.lua 现有逻辑
local installdir = runtime.get_pkginfo().install_dir
if not os.tryrm(installdir) and os.isdir(installdir) then
    sudo.exec("rm -rf " .. installdir)
end
```

在多 subos 架构下，这会导致 SubOS A remove 时删掉 SubOS B 还在使用的包文件。

---

## 3. 依赖前置

| 依赖 | 原因 |
|------|------|
| 无强依赖 | 引用检查逻辑独立于 T14/T15，可并行开发 |
| 建议在 T14 之后合并 | T14 完成后测试更完整 |

---

## 4. 实现方案

### 4.1 修改 XPkgManager:uninstall()

文件: `core/xim/pm/XPkgManager.lua`

```lua
function XPkgManager:uninstall(xpkg)
    -- 1. 执行 xpkg 的 uninstall hook（当前 subos 的 xvm 注销等）
    local ret = _try_execute_hook(xpkg.name, xpkg, "uninstall")

    -- 2. 检查是否还有其他 subos 在使用
    local installdir = runtime.get_pkginfo().install_dir
    if _any_other_subos_using(xpkg.name) then
        cprint("[xlings:xim]: ${dim}other subos still using, keep xpkgs files${clear}")
    else
        cprint("[xlings:xim]: no other references, removing - ${dim}%s${clear}", installdir)
        if not os.tryrm(installdir) and os.isdir(installdir) then
            sudo.exec("rm -rf " .. installdir)
        end
    end
    return ret
end
```

### 4.2 新增 _any_other_subos_using() 辅助函数

文件: `core/xim/pm/XPkgManager.lua`

```lua
function _any_other_subos_using(pkgname)
    local current_subos = path.filename(platform.get_config_info().subosdir)
    local subos_root = path.join(platform.get_config_info().homedir, "subos")
    local subos_dirs = os.dirs(path.join(subos_root, "*"))

    for _, subos_dir in ipairs(subos_dirs or {}) do
        local name = path.filename(subos_dir)
        if name ~= current_subos and name ~= "current" then
            local workspace_file = path.join(subos_dir, "xvm", ".workspace.xvm.yaml")
            if os.isfile(workspace_file) then
                local content = io.readfile(workspace_file)
                if content:find(pkgname, 1, true) then
                    return true
                end
            end
        end
    end
    return false
end
```

### 4.3 设计要点

**为什么不维护引用计数注册表**：
- xvm workspace 文件已经记录了每个 subos 注册的包
- 扫描这些文件直接获取真实状态，不存在一致性问题
- subos 数量通常 1-5 个，扫描开销可忽略（几个小文件的字符串匹配）
- 避免引入额外的注册表文件和同步逻辑

**兜底清理**：
- `xlings self clean` 已有 GC 机制（`profile::gc()`）
- 可扩展为扫描 xpkgs 中所有包 vs 所有 subos 的 xvm 注册，删除无引用的包

---

## 5. 验收标准

### 5.1 多 subos 共享包 — remove 不误删

```bash
# 准备: 确保 default 和 test-ref 两个 subos 都安装了 cmake
xlings install cmake -y
xlings subos new test-ref
xlings subos use test-ref
xlings install cmake -y

# 在 test-ref 中 remove cmake
xlings remove cmake -y
# 期望: 输出 "other subos still using, keep xpkgs files"
# 期望: xpkgs/cmake/ 目录仍然存在

# 验证 default 中 cmake 仍可用
xlings subos use default
cmake --version
# 期望: 正常输出
```

### 5.2 单 subos — remove 正常删除

```bash
# 确保只有 default subos 安装了某个包
xlings install mdbook -y
xlings remove mdbook -y
# 期望: 输出 "no other references, removing"
# 期望: xpkgs 对应目录已被删除

ls ~/.xlings/data/xpkgs/mdbook/
# 期望: 目录不存在
```

### 5.3 最后一个 subos remove — 删除文件

```bash
# 延续 5.1 的环境
xlings subos use default
xlings remove cmake -y
# 期望: 此时 test-ref 中已 remove，default 是最后引用
# 期望: 输出 "no other references, removing"
# 期望: xpkgs/cmake/ 目录被删除

xlings subos remove test-ref
```

### 5.4 验收检查表

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| 多 subos 共享时 remove 不删 xpkgs | 文件保留，输出提示 | [ ] |
| 单 subos remove 正常删除 xpkgs | 文件删除 | [ ] |
| 最后引用者 remove 时删除 xpkgs | 文件删除 | [ ] |
| uninstall hook 正常执行 | xvm 注销成功 | [ ] |
| subos 超过 3 个时性能可接受 | remove 耗时 < 2 秒 | [ ] |
