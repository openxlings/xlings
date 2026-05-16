# 优化 elfpatch 机制：包声明式接口 + 分类处理

## Context

预构建二进制包的 PT_INTERP/RUNPATH 指向构建服务器路径。CI 显示 `43 0 43`（全部失败）。
根因：`--set-interpreter` 对共享库失败后级联跳过 `--set-rpath`，且无分类处理。

## 系统级设计：包声明目标，elfpatch 按类型处理

### 核心理念

**包最了解自己的结构**。gcc.lua 知道 `bin/` 是可执行文件，`lib64/` 是库。
让包通过接口声明，而不是让 elfpatch 扫描文件头再猜测。

### 新接口

```lua
-- 在包的 install() 中：
elfpatch.auto({
    enable = true,
    shrink = true,
    bins = { "bin" },         -- 可执行文件目录（interpreter + rpath）
    libs = { "lib64" },       -- 共享库目录（rpath only）
})
```

**速度对比**：

| 方案 | I/O 次数 | patchelf 调用 |
|------|---------|--------------|
| 当前（全扫描） | 43 次 `_is_elf` + 43 次 patchelf | 43 次（含必失败的） |
| ELF header 分类 | 43 次 `_is_elf` + 43 次读头 + 24 次 `--print-interpreter` | 43 次 |
| **包声明式** | 0 次分类 | 只对正确类型调用（~43 次，0 失败） |

### 处理流程

```
elfpatch.auto({ bins=["bin"], libs=["lib64"] })
                     ↓                ↓
              扫描 bin/ 目录     扫描 lib64/ 目录
              找到 24 个 ELF     找到 19 个 ELF
                     ↓                ↓
            set-interpreter     skip interpreter
            + set-rpath         + set-rpath
            + shrink            + shrink
```

### 向后兼容

未指定 `bins`/`libs` 时，回退到当前行为（全目录扫描 + 统一处理），
但改为**不级联失败**：interpreter 失败不影响 rpath 设置。

### 跨平台

| 平台 | bins 目录 | libs 目录 |
|------|----------|----------|
| Linux | interpreter + rpath | rpath only |
| macOS | rpath + dylib refs | rpath + dylib refs |

macOS 没有 interpreter 概念，bins/libs 都做 rpath 修正。

## 修改方案

### 文件: `elfpatch.lua` (libxpkg)

路径: `/home/speak/workspace/github/mcpplibs/libxpkg/src/lua-stdlib/xim/libxpkg/elfpatch.lua`

#### 1. `M.auto()` — 接收 bins/libs 参数

```lua
function M.auto(enable_or_opts)
    _RUNTIME = _RUNTIME or {}
    if type(enable_or_opts) == "table" then
        if enable_or_opts.enable ~= nil then
            _RUNTIME.elfpatch_auto = (enable_or_opts.enable == true)
        end
        if enable_or_opts.shrink ~= nil then
            _RUNTIME.elfpatch_shrink = (enable_or_opts.shrink == true)
        end
        -- 新增：包声明式目标
        if enable_or_opts.bins then
            _RUNTIME.elfpatch_bins = enable_or_opts.bins
        end
        if enable_or_opts.libs then
            _RUNTIME.elfpatch_libs = enable_or_opts.libs
        end
    else
        _RUNTIME.elfpatch_auto = (enable_or_opts == true)
    end
    return _RUNTIME.elfpatch_auto
end
```

#### 2. 新增 `_patch_elf_targets` — 按类型处理

```lua
-- Patch a list of directories/files as executables (interpreter + rpath)
local function _patch_elf_executables(patch_tool, dirs, install_dir, loader, rpath, shrink, result)
    for _, dir in ipairs(dirs) do
        local full = path.is_absolute(dir) and dir or path.join(install_dir, dir)
        local targets = _collect_targets(full, { include_shared_libs = true })
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            local ok = true
            if loader then
                ok = _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-interpreter " .. _shell_quote(loader)
                    .. " " .. _shell_quote(filepath))
            end
            if ok and rpath and rpath ~= "" then
                ok = _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-rpath " .. _shell_quote(rpath)
                    .. " " .. _shell_quote(filepath))
            end
            if ok then
                result.patched = result.patched + 1
                _apply_shrink(patch_tool, filepath, shrink, result)
            else
                result.failed = result.failed + 1
            end
        end
    end
end

-- Patch a list of directories/files as libraries (rpath only, no interpreter)
local function _patch_elf_libraries(patch_tool, dirs, install_dir, rpath, shrink, result)
    for _, dir in ipairs(dirs) do
        local full = path.is_absolute(dir) and dir or path.join(install_dir, dir)
        local targets = _collect_targets(full, { include_shared_libs = true })
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            local ok = true
            if rpath and rpath ~= "" then
                ok = _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-rpath " .. _shell_quote(rpath)
                    .. " " .. _shell_quote(filepath))
            end
            if ok then
                result.patched = result.patched + 1
                _apply_shrink(patch_tool, filepath, shrink, result)
            else
                result.failed = result.failed + 1
            end
        end
    end
end

-- Shared shrink helper
local function _apply_shrink(patch_tool, filepath, shrink, result)
    if shrink == true then
        if _exec_ok(_shell_quote(patch_tool.program) .. " --shrink-rpath " .. _shell_quote(filepath)) then
            result.shrinked = result.shrinked + 1
        else
            result.shrink_failed = result.shrink_failed + 1
        end
    end
end
```

#### 3. 重写 `_patch_elf` — 声明式优先，全扫描回退

```lua
local function _patch_elf(target, opts, result)
    local patch_tool = _get_tool("patchelf", "patchelf")
    if not patch_tool then
        _warn("patchelf not found, skip patching")
        return result
    end

    local loader = _resolve_loader(opts.loader)
    local rpath = _normalize_rpath(opts.rpath)
    if opts.loader and not loader then
        _warn("cannot resolve loader '" .. tostring(opts.loader) .. "'")
    end

    local install_dir = _RUNTIME and _RUNTIME.install_dir or target
    local bins = opts.bins or (_RUNTIME and _RUNTIME.elfpatch_bins)
    local libs = opts.libs or (_RUNTIME and _RUNTIME.elfpatch_libs)

    if bins or libs then
        -- 声明式模式：包已分类
        _info(string.format("declared: bins=%s libs=%s loader=%s",
            bins and table.concat(bins,",") or "nil",
            libs and table.concat(libs,",") or "nil",
            tostring(loader)))
        _patch_elf_executables(patch_tool, bins or {}, install_dir, loader, rpath, opts.shrink, result)
        _patch_elf_libraries(patch_tool, libs or {}, install_dir, rpath, opts.shrink, result)
    else
        -- 回退模式：全扫描，interpreter 和 rpath 独立处理（不级联）
        _info("fallback scan mode, loader=" .. tostring(loader))
        local targets = _collect_targets(target, opts)
        for _, filepath in ipairs(targets) do
            result.scanned = result.scanned + 1
            local any_ok = false

            if loader then
                if _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-interpreter " .. _shell_quote(loader)
                    .. " " .. _shell_quote(filepath)) then
                    any_ok = true
                end
            end
            if rpath and rpath ~= "" then
                if _exec_ok(_shell_quote(patch_tool.program)
                    .. " --set-rpath " .. _shell_quote(rpath)
                    .. " " .. _shell_quote(filepath)) then
                    any_ok = true
                end
            end

            if any_ok then
                result.patched = result.patched + 1
                _apply_shrink(patch_tool, filepath, opts.shrink, result)
            else
                result.failed = result.failed + 1
            end
        end
    end

    return result
end
```

#### 4. `_get_tool` — 增加 `_RUNTIME.bin_dir` 回退

```lua
-- 在 _tool_exists 和 _try_probe_tool 都失败后，增加：
if not tool then
    local bin_dir = _RUNTIME and _RUNTIME.bin_dir
    if bin_dir then
        local full_path = path.join(bin_dir, toolname)
        if os.isfile(full_path) then
            tool = _try_probe_tool(full_path)
        end
    end
end
```

### 文件: `gcc.lua` (xim-pkgindex)

路径: `tests/fixtures/xim-pkgindex/pkgs/g/gcc.lua`

```lua
-- 修改 install() 中的 elfpatch.auto 调用
elfpatch.auto({
    enable = true,
    shrink = true,
    bins = { "bin", "libexec" },  -- 可执行文件
    libs = { "lib64" },           -- 共享库
})
```

### 文件: `glibc.lua`、`binutils.lua` 等其他包

同样添加 `bins`/`libs` 声明（如适用），或保持不变使用回退模式。

### 文件: `installer.cppm` (xlings repo)

elfpatch 前把 `binDir` 加入 PATH：

```cpp
if (!payloadInstalled) {
    auto binDir = Config::paths().binDir.string();
    auto curPath = std::string(std::getenv("PATH") ? std::getenv("PATH") : "");
    if (!binDir.empty() && curPath.find(binDir) == std::string::npos) {
        platform::set_env_variable("PATH",
            binDir + std::string(1, platform::PATH_SEPARATOR) + curPath);
    }
    auto epResult = executor.apply_elfpatch_auto();
    ...
}
```

## 关键文件

| 仓库 | 文件 | 修改 |
|------|------|------|
| libxpkg | `src/lua-stdlib/xim/libxpkg/elfpatch.lua` | 声明式接口 + 分类处理 + bin_dir 回退 + 回退模式不级联 |
| xlings | `src/core/xim/installer.cppm` | elfpatch 前 PATH 设置 |
| xlings (pkgindex) | `tests/fixtures/xim-pkgindex/pkgs/g/gcc.lua` | 使用 bins/libs 声明 |

## 预期效果

```
# 修复前
gcc: elfpatch auto: 43 0 43

# 修复后（声明式）
elfpatch: declared: bins=bin,libexec libs=lib64 loader=/home/runner/.xlings/subos/default/lib/ld-linux-x86-64.so.2
gcc: elfpatch auto: 43 43 0
```

## 验证

1. `cd /home/speak/workspace/github/mcpplibs/libxpkg && xmake build && xmake run mcpplibs-xpkg-tests`
2. `cd /home/speak/workspace/github/d2learn/xlings && rm -rf build && xmake build && xmake run xlings_tests`
3. `xlings install xim:gcc@15.1.0 -y --force --verbose 2>&1 | grep -i elfpatch`
4. `readelf -l ~/.xlings/data/xpkgs/xim-x-gcc/15.1.0/bin/gcc | grep interpreter` → 用户本地路径
5. `gcc --version` → 正常运行
