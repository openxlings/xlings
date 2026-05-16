# T14 — 多 subos 安装复用 xpkgs（框架层拦截）

> **优先级**: P0
> **Wave**: 1（需 T15 先完成）
> **预估改动**: ~20 行 Lua
> **设计文档**: [../xim-issues-design.md](../xim-issues-design.md) §二

---

## 1. 任务概述

在 `PkgManagerExecutor:install()` 中检测 xpkgs 目录是否已有包文件。已有时跳过整个 install hook（download + build + install 全部跳过），只调 `config()` 完成当前 subos 的 xvm 注册和环境配置。

由于是框架层拦截，xpkg.lua 中的 `os.tryrm(install_dir)` 等代码根本不会执行，无需修改任何 xpkg.lua。

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T15 | template.lua 的 config() 必须存在，否则跳过 install 后无法完成 xvm 注册 |

---

## 3. 实现方案

### 3.1 改造 PkgManagerExecutor:install()

文件: `core/xim/pm/PkgManagerExecutor.lua`

```lua
function PkgManagerExecutor:install()
    os.cd(runtime.get_runtime_dir())

    if self.type == "xpm" then
        local pkginfo = runtime.get_pkginfo()

        if _xpkgs_has_files(pkginfo.install_dir) then
            cprint("[xlings:xim]: ${dim}reuse xpkgs, skip download${clear}")
        else
            if not self:_download() or not self:_build() then
                cprint("[xlings:xim]: hooks: ${red}download or build failed${clear}")
                return false
            end
            cprint("[xlings:xim]: start install ${green}%s${clear}...", self._pkg.name)
            if not _try_execute(self, "install") then
                cprint("[xlings:xim]: hooks.install: ${red}install failed${clear}")
                return false
            end
        end

        -- config 始终执行（无论是否跳过 install）
        if not self:_config() then
            cprint("[xlings:xim]: hooks.config: ${red}config failed${clear}")
            return false
        end
        return true
    end

    -- 非 xpm 类型，原有逻辑
    cprint("[xlings:xim]: start install ${green}%s${clear}...", self._pkg.name)
    if not _try_execute(self, "install") then
        cprint("[xlings:xim]: hooks.install: ${red}install failed${clear}")
        return false
    end
    return true
end

function _xpkgs_has_files(install_dir)
    if not os.isdir(install_dir) then return false end
    local entries = os.files(path.join(install_dir, "*"))
    return entries and #entries > 0
end
```

### 3.2 XPkgManager:config() 增加 template 兜底

文件: `core/xim/pm/XPkgManager.lua`

```lua
function XPkgManager:config(xpkg)
    if not xpkg.hooks["config"] then
        if xpkg.type == "template" then
            xpkg.hooks["config"] = function()
                return types.template.config(xpkg)
            end
        end
    end
    return _try_execute_hook(xpkg.name, xpkg, "config")
end
```

### 3.3 依赖链自动递归

`CmdProcessor:install()` 第 292-308 行处理依赖时递归调用 `new(dep_name, {install=true}):run()`，每个依赖也会进入 `PkgManagerExecutor:install()`，因此 xpkgs 复用逻辑对整个依赖链自动生效。

---

## 4. 验收标准

### 4.1 基础复用验证

```bash
# 确保 default subos 已安装 cmake
xlings install cmake -y

# 创建新 subos 并切换
xlings subos new test-reuse
xlings subos use test-reuse

# 在新 subos 中安装 cmake
xlings install cmake -y
# 期望: 输出包含 "reuse xpkgs, skip download"
# 期望: 不出现 download 进度条或 "start extract"

cmake --version
# 期望: 正常输出版本号

# 清理
xlings subos use default
xlings subos remove test-reuse
```

### 4.2 全新安装不受影响

```bash
xlings install mdbook -y
# 期望: 正常执行完整 download → install → config 流程
# 期望: 不出现 "reuse xpkgs" 提示
```

### 4.3 带依赖包的复用

```bash
xlings subos new test-deps
xlings subos use test-deps

# 安装带依赖的包（gcc 依赖 glibc、binutils、linux-headers）
xlings install gcc -y
# 期望: gcc 和所有依赖都输出 "reuse xpkgs, skip download"
# 期望: 每个包的 config 正常执行（xvm 注册、header 拷贝等）

gcc --version
# 期望: 正常输出

xlings subos use default
xlings subos remove test-deps
```

### 4.4 空目录不误判

```bash
# 模拟空的 xpkgs 目录
mkdir -p ~/.xlings/data/xpkgs/test-empty/0.0.1/
xlings install test-empty@0.0.1 -y
# 期望: 空目录不被视为已物化，走正常安装流程
```

### 4.5 验收检查表

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| 新 subos 安装已有包，跳过下载 | 输出 "reuse xpkgs" | [ ] |
| config 正常执行完成 xvm 注册 | 工具命令可用 | [ ] |
| 全新包安装流程不受影响 | 完整 download → install → config | [ ] |
| 依赖链全部复用 | 所有依赖都输出 "reuse xpkgs" | [ ] |
| 空目录不误判为已物化 | 走正常安装流程 | [ ] |
| 安装耗时对比 | 复用时 < 5 秒 vs 全新安装 > 30 秒 | [ ] |
