# T15 — template.lua config 拆分 + 补齐 2 个 xpkg 的 config

> **优先级**: P0
> **Wave**: 1（T14 的前置依赖）
> **预估改动**: ~10 行 Lua（3 个文件）
> **设计文档**: [../xim-issues-design.md](../xim-issues-design.md) §二

---

## 1. 任务概述

将 `template.lua` 中 `install()` 里的 `xvm.add()` 移到新增的 `config()` 中，并为 `rustup.lua` 和 `musl-cross-make.lua` 补上 `config()` 函数。

这是 T14（xpkgs 复用）生效的前提：跳过 `install()` 直接调 `config()` 时，xvm 注册必须在 `config()` 中完成。

---

## 2. 依赖前置

无。本任务是其他 P0 任务的前置。

---

## 3. 实现方案

### 3.1 修改 template.lua

文件: `core/xim/pm/types/template.lua`

```lua
function install(xpkg)
    local pkginfo = runtime.get_pkginfo()
    local install_file = pkginfo.install_file

    pkgdir = install_file
        :replace(".zip", "")
        :replace(".tar.gz", "")
        :replace(".git", "")

    os.tryrm(pkginfo.install_dir)

    if not os.trymv(pkgdir, pkginfo.install_dir) then
        cprint("[xlings:xim:template]: ${red bright}failed to install to %s${clear}", pkginfo.install_dir)
        return false
    end

    -- xvm.add 移到 config() 中
    return true
end

function config(xpkg)
    xvm.add(__get_xvm_pkgname(xpkg))
    return true
end
```

### 3.2 补齐 rustup.lua 的 config()

文件: `xim-pkgindex/pkgs/r/rustup.lua`

当前 `install()` 中有 `xvm.add("rustup-init")`，需移到新增的 `config()` 中：

```lua
function config()
    xvm.add("rustup-init")
    return true
end
```

对应地从 `install()` 中删除 `xvm.add("rustup-init")` 行。

### 3.3 补齐 musl-cross-make.lua 的 config()

文件: `xim-pkgindex/pkgs/m/musl-cross-make.lua`

当前 `install()` 中有 `xvm.add("musl-cross-make", {...})`，需移到新增的 `config()` 中：

```lua
function config()
    local target_script_file = path.join(pkginfo.install_dir(), "musl-cross-make.lua")
    xvm.add("musl-cross-make", {
        alias = "xlings script " .. target_script_file,
        bindir = "TODO-FIX-SPATH-ISSUES",
    })
    return true
end
```

对应地从 `install()` 中删除 `xvm.add(...)` 调用。

---

## 4. 不需要修改的包（已确认）

审查 44 个有 `install()` 的包：

- **30 个已有自定义 config()** — 无需改动（cmake, gcc, glibc, d2x, openssl, binutils, 等）
- **11 个无 config() 且不涉及 subos 切换** — 无需改动（nvm, wsl-ubuntu, gitcode-hosts, xvm, vs-buildtools, 等）
- **1 个** template 类型无 config() — 由 T14 的 `XPkgManager:config()` 框架兜底处理，无需手动补
- **2 个需补 config()** — rustup.lua, musl-cross-make.lua（本任务处理）

---

## 5. 验收标准

### 5.1 template 类型包验证

```bash
xlings remove mdbook -y
xlings install mdbook -y
mdbook --version
# 期望: install 成功，xvm 注册在 config 阶段完成
```

### 5.2 代码审查

```bash
# template.lua 的 install() 中不含 xvm.add
grep "xvm.add" core/xim/pm/types/template.lua
# 期望: 只在 config() 函数中出现

# rustup.lua 的 install() 中不含 xvm.add
grep "xvm.add" ../xim-pkgindex/pkgs/r/rustup.lua
# 期望: 只在 config() 函数中出现

# musl-cross-make.lua 的 install() 中不含 xvm.add
grep "xvm.add" ../xim-pkgindex/pkgs/m/musl-cross-make.lua
# 期望: 只在 config() 函数中出现（install 中的 xpkg_main 内的 xvm.add 不算）
```

### 5.3 验收检查表

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| template.lua install() 不含 xvm.add | 代码审查确认 | [ ] |
| template.lua 新增 config() 含 xvm.add | 代码审查确认 | [ ] |
| rustup.lua 补齐 config() | 代码审查确认 | [ ] |
| musl-cross-make.lua 补齐 config() | 代码审查确认 | [ ] |
| template 类型包安装后可正常使用 | mdbook --version 成功 | [ ] |
