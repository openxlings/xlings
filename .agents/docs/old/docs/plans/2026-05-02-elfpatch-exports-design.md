# 包元数据 `exports` + ElfPatch 重定位机制重设计

**Date**: 2026-05-02
**Status**: Design Spec（讨论中，未实施）
**Scope**: cross-repo — `mcpplibs/libxpkg` schema、`d2learn/xlings` installer/runtime、`d2learn/xim-pkgindex` provider/consumer 迁移
**Replaces**: 当前 4 个 consumer 各自 `elfpatch.auto({...})` 命令式调用 + 硬编码 loader 路径的实现

---

## 1. 背景与问题

### 1.1 当前在做的事

xlings 装一个二进制类 xpkg（gcc / binutils / openssl / d2x / glibc 等）后，需要修正其 ELF binary 的：

- **INTERP** （动态链接器路径，`PT_INTERP` 段）—— 默认 `/lib64/ld-linux-x86-64.so.2`，需重定位到 xlings 装好的 glibc 的 `lib64/ld-linux-x86-64.so.2` 绝对路径
- **RPATH/RUNPATH** （动态库搜索路径，`DT_RPATH` / `DT_RUNPATH` 段）—— 加上自身的 `lib64/lib`、deps 的 `lib64/lib`、subos sysroot 的 `lib`

这一步 install 后必须做，否则二进制运行时找不到 libc / 其他 .so。

### 1.2 当前实现的痛点

| 痛点 | 量化 | 来源 |
|---|---|---|
| Consumer 包描述里硬编码 loader 路径 | 4 个 consumer × 1 行 `path.join("lib64", "ld-linux-x86-64.so.2")` | binutils / openssl / gcc / d2x |
| Consumer 硬编码 dep 版本 | 4 个 consumer × 1 行 `dep_install_dir("glibc", "2.39")` | 同上 |
| Consumer 写 6 字段 elfpatch.auto 调用 | 4 个 consumer × ~5 行 boilerplate | 同上 |
| `_RUNTIME.deps_list` 是 runtime+build 并集 | 0.4.10 引入 #249 后的潜在 bug | build_deps 的 lib 会污染消费者 RPATH |
| libdirs 用约定 `{lib64, lib}` 写死 | 任何非常规布局的包不被覆盖 | `closure_lib_paths` 行 493 |
| 多 ABI（musl + glibc）共存无法表达 | 无 | `_resolve_loader` 没有 ABI 概念 |
| 跨架构防护缺失 | 一个 cross-build 包（同名同版本但 binary 是 arm64）会被错误 patch | `_is_elf` 仅 magic 检查，不读 e_machine |

### 1.3 现有实现的优点（保留）

| 已经做对的 80% | 不要重来 |
|---|---|
| `closure_lib_paths(deps_list)` 自动 RPATH 闭包思路 | 保留，扩展数据源 |
| `loader = "subos"` keyword 解析 | 保留 |
| bins / libs 目录区分 + ELF 类型自动检测 | 保留 |
| shrink / strict / recurse 等边角优化 | 保留 |
| macOS Mach-O `install_name_tool` 路径 | 保留 |
| xlings 端 `ctx.deps_list` 注入 + 装后调 `apply_elfpatch_auto` | 保留，扩展为 split deps |

本设计是**纯增量演进**，不重写 `_patch_elf` / `_patch_macho` 等核心 patch 逻辑。

---

## 2. 设计原则（锁定）

1. **声明式收口**：把"包的运行时属性"（loader / libdirs / abi）从 install hook 的命令式调用挪到 schema 元数据里，由 provider 单点声明
2. **subos 不做环境隔离**：subos 只是版本视图。INTERP / RPATH 用 install-time 计算的真实绝对路径，**不**走 subos 软链间接层
3. **Provider 只为非默认行为承担声明**：约定能覆盖的（`{lib64, lib}` libdirs）不强求声明
4. **控制流统一在 install hook**：用户控制 / opt-out / override 全部通过 `libxpkg.elfpatch` API，**元数据层不再有 elfpatch 控制字段**
5. **多 libc 共存合法**：通过 `abi` tag 区分，subos 一个工作环境里允许同时存在 glibc 和 musl 的 xpkg
6. **predicate-driven 触发**：xlings 根据 dep 的 `exports` 自动决定是否需要 patch，0 行 consumer 代码

---

## 3. Schema 设计

### 3.1 `xpm.<platform>.exports` 字段

新增的**唯一**元数据字段。位置：`xpm.<platform>` 下，与 `deps` 同级。

完整 schema（v1，所有子字段都是可选）：

```lua
xpm = {
    linux = {
        deps = {
            runtime = { ... },
            build   = { ... },
        },
        exports = {                                     -- ← 新增
            runtime = {
                loader  = "lib64/ld-linux-x86-64.so.2", -- 仅 libc 家族
                libdirs = { "lib64", "lib" },           -- 默认 {lib64, lib}，不一样才声明
                abi     = "linux-x86_64-glibc",         -- 多 libc 时必须
            },
        },
        ["latest"] = { ref = "..." },
        ["1.0.0"]  = { url = "...", sha256 = "..." },
    },
}
```

**v1 只设计 `runtime.*`**。`data.*` / `build.*` 等 capability 等真实需求出现再加，本次不做。

### 3.2 谁需要声明、声明什么

| 字段 | 谁必须声明 | 谁不需要声明 |
|---|---|---|
| `runtime.loader` | libc 家族（`glibc`、`musl-gcc` 内置 musl libc） | 其他**所有**包 |
| `runtime.abi` | 多 libc 共存场景下的 libc 家族 | v1 单 libc 时可不声明 |
| `runtime.libdirs` | 非 `{lib64, lib}` 布局的包（实测 0 个） | 99% 的包 |

整个 xim-pkgindex 改造后**真实声明 exports 的包数量**：≤ 5（glibc、musl-gcc、未来加 musl 单独包；其他都不写）。

### 3.3 路径语义

`exports.runtime.loader` 和 `exports.runtime.libdirs` 里的路径**都是相对路径**——相对于该包安装后的 `install_dir`。xlings 在 install-time join 上目标机器的绝对路径，得到真实 INTERP 值：

```
真实 INTERP = $XLINGS_HOME/data/xpkgs/xim-x-glibc/2.39/ + lib64/ld-linux-x86-64.so.2
            = /home/<user>/.xlings/data/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
```

每台机器、每个 user 的 `$XLINGS_HOME` 不同 → INTERP 在每台机器上都不一样 → 必须 install-time 在目标机器上算。**不能预 bake 进 release tarball**。

### 3.4 具体例子

```lua
-- glibc.lua（loader provider）
package = {
    spec = "1",
    name = "glibc",
    type = "package",
    -- ...
    xpm = {
        linux = {
            deps = { runtime = { ... } },
            exports = {
                runtime = {
                    loader = "lib64/ld-linux-x86-64.so.2",
                    abi    = "linux-x86_64-glibc",
                    -- libdirs 不写，默认 {lib64, lib}
                },
            },
            ["2.39"] = { url = "...", sha256 = "..." },
        },
    },
}
function install()
    -- ...解压拷贝...
    -- 不调用 elfpatch.auto / set / skip 任何 API
end
function config()
    xvm.add("glibc", { libdir = path.join(pkginfo.install_dir(), "lib64") })
end
```

```lua
-- binutils.lua（消费者）
package = {
    spec = "1",
    name = "binutils",
    type = "package",
    -- ...
    xpm = {
        linux = {
            deps = { runtime = { "glibc@2.39" } },
            -- 不需要 exports
            ["2.42"] = { url = "...", sha256 = "..." },
        },
    },
}
function install()
    -- ...解压拷贝...
    -- 不调用 elfpatch 任何 API
end
function config()
    xvm.add("binutils")
end
```

---

## 4. 触发机制（predicate-driven）

### 4.1 触发规则（按优先级）

xlings 在 install hook 跑完后执行 elfpatch 决策，按以下规则 fall through：

```
规则 0：用户在 install hook 调过 elfpatch.skip()
        → 不 patch（用户显式跳过）

规则 1：用户在 install hook 调过 elfpatch.set({...})
        → 用 hook 给的参数 patch（用户显式覆盖）

规则 2：当前 consumer 自己声明了 exports.runtime.loader
        → 用 consumer 自己的 loader patch
        （处理 glibc / musl 自我 patch 的情况）

规则 3：当前 consumer 的 runtime deps 里有≥1 个声明了 exports.runtime.loader 的包
        → 按下面的子规则选 loader：
        a. 恰好 1 个 INTERP-provider → 用它的 loader
        b. ≥ 2 个 INTERP-provider 且 consumer 没声明 interp_from → fail-fast
        c. ≥ 2 个 INTERP-provider 且 consumer 声明了 interp_from → 选 abi 匹配的

规则 4：以上都不满足
        → 不 patch（consumer 是纯数据包 / 静态二进制 / 用 host 系统 libc）
```

**核心洞察**：`exports` 不只是元数据声明——它**决定了「这个包是否触发自动 patch」**。整个机制不再依赖 consumer 写 `elfpatch.auto({enable=true})`。

### 4.2 多 INTERP-provider 的 fail-fast

当 consumer 的 runtime deps 里同时有 glibc 和 musl 的 xpkg：

```
[xim:install] consumer 'foo' has multiple loader providers in runtime deps:
  - xim:glibc@2.39 (abi: linux-x86_64-glibc)
  - xim:musl-gcc@15.1.0-musl (abi: linux-x86_64-musl)
Please pick one explicitly in the install hook:

  function install()
      elfpatch.set({ interp_from = "linux-x86_64-glibc" })  -- or "linux-x86_64-musl"
  end
```

不允许 xlings 随便选第一个——因为选错了 binary 也能跑（被 musl loader 加载）但运行时找不到 .so（RPATH 闭包是 glibc 的）。**fail-fast** 强制 consumer 表态。

---

## 5. 用户控制：`libxpkg.elfpatch` API

### 5.1 设计原则

- **元数据层不引入 elfpatch 相关字段**（不要 `package.elfpatch = {...}`）
- 所有控制点统一在 install hook 通过 `libxpkg.elfpatch` 调用
- 默认行为（hook 不调任何 API）= predicate-driven auto

### 5.2 对外 API（v1）

```lua
-- 主要 API
elfpatch.set(opts)        -- 覆盖 predicate auto，使用 hook 给的参数
elfpatch.skip()           -- 跳过 auto-patch

-- 低层 escape hatch
elfpatch.patch_elf_loader_rpath(target, opts)   -- 直接 patch（用户完全手控）
elfpatch.closure_lib_paths(opts)                -- 计算 lib 闭包（debug / 高级用法）
```

**移除的 API**：

```lua
-- v0 旧 API（移除）：
elfpatch.auto(enable_or_opts)
elfpatch.is_auto()
elfpatch.is_shrink()
elfpatch.set_interpreter(...)        -- 用 patch_elf_loader_rpath 代替
elfpatch.set_rpath(...)              -- 同上
elfpatch.apply_auto(opts)            -- 内部 API，重命名 _apply()，只 xlings 调
```

### 5.3 `elfpatch.set` 参数 schema

所有字段可选：

```lua
elfpatch.set({
    enable      = true | false,                  -- 强制开/关
    interp_from = "linux-x86_64-glibc",          -- 多 INTERP-provider 时显式选
    interpreter = "/abs/path/to/loader",         -- 直接指定（最强，绕过解析）
    extra_rpath = { "/path/...", ... },          -- 默认 closure 之外补加
    scan        = "convention" | "deep" | { "bin", "share/foo" },
    skip        = { "share/firmware/" },         -- 子目录黑名单
    shrink      = true | false,
})
```

### 5.4 语义：覆盖式（不是合并式）

调用 `elfpatch.set` 后，**predicate auto 完全停用**——使用 hook 给的参数。即使 `set` 只指定了 `shrink = true`，loader / rpath 也走 set 给的（如果没给就不 patch loader / 不设 RPATH）。

**理由**：合并式语义复杂、调试困难。如果用户想"shrink + auto loader" 这种部分自定义，**罕见**——真有就显式写 `interpreter = ...` 或 `interp_from = ...`。

### 5.5 用法表

| 用户意图 | 写法 |
|---|---|
| 默认（99% 包） | 不调 elfpatch 任何 API |
| 我的包结构非常规，要深扫 | `elfpatch.set({ scan = "deep" })` |
| 我有 firmware blob 不要 patch | `elfpatch.set({ skip = { "share/fw/" } })` |
| 多 libc 时显式选 glibc | `elfpatch.set({ interp_from = "linux-x86_64-glibc" })` |
| 关闭 shrink | `elfpatch.set({ shrink = false })`（注意：scan 默认会变 nil → 不 patch loader/rpath） |
| 完全跳过 | `elfpatch.skip()` |
| 我自己用 patchelf 处理 | `elfpatch.skip()` 后自己 `os.exec("patchelf ...")` |
| 完全手控 | `elfpatch.skip()` + `elfpatch.patch_elf_loader_rpath(install_dir, {...})` |

---

## 6. 自动扫描策略

### 6.1 三种扫描模式

| 模式 | 范围 | 速度 | 默认？ |
|---|---|---|---|
| `"convention"` | `bin/`, `sbin/`, `libexec/`, `lib/`, `lib64/` | 快 | ✅ |
| `"deep"` | install_dir 整棵树递归 | 慢 | opt-in |
| `{ "dir1", "dir2" }` | 用户列出的具体目录 | 最快 | opt-in |

### 6.2 默认 `"convention"` 的依据

实测 xim-pkgindex 现有所有包的 ELF 分布：

| 目录 | 命中率 |
|---|---|
| `bin/` | 100% |
| `lib64/` | 60% |
| `lib/` | 30% |
| `libexec/` | 10% |
| 其他 | 0%（暂无） |

`"convention"` 已 100% 覆盖现实包。`"deep"` 是为未来非常规包准备的 escape hatch。

### 6.3 文件类型识别

现有 `_is_elf` / `_is_macho` 用 magic byte 判断，不会误判 PNG / JSON 之类。

**新增防护：`e_machine` 检查**（用 ELF header 的 18-19 字节字段）：

| host arch | 期望 e_machine | 不匹配则 |
|---|---|---|
| x86_64 | `EM_X86_64 = 62` | skip（不 patch） |
| aarch64 | `EM_AARCH64 = 183` | skip |

防止跨平台分发包（同时含 x86_64 和 aarch64 binary）被错 patch。

### 6.4 静态二进制处理

读 ELF program headers，没有 `PT_INTERP` 段的二进制（静态链接）→ skip。patchelf 自己也会跳过，但提前检测节省 fork-exec。

---

## 7. 后端机制实现

### 7.1 _RUNTIME 注入扩展（C++ 侧）

`xpkg-executor.cppm` 的 `inject_context` 增加 4 个字段：

```cpp
// 新增字段（在 ExecutionContext 里）
std::vector<std::string> runtime_deps_list;   // node.runtime_deps
std::vector<std::string> build_deps_list;     // node.build_deps
struct DepExportSlim {
    std::string loader;        // exports.runtime.loader（绝对路径，xlings 已 join install_dir）
    std::vector<std::string> libdirs;
    std::string abi;
};
std::map<std::string, DepExportSlim> deps_exports;  // "name@ver" → 简化 exports
DepExportSlim self_exports;                         // 当前包自己的 exports
```

xlings installer.cppm 在调 `inject_context` 前，**预先 join 好绝对路径**——elfpatch.lua 拿到的 loader 就是真实可用的绝对路径，不用再 join。

注入后 `_RUNTIME` 长这样：

```lua
_RUNTIME = {
    -- 原有字段
    pkg_name, version, platform, install_dir, install_file,
    deps_list,                                    -- 保留 (union)，老代码兼容
    -- 新增字段
    runtime_deps_list = { "xim:glibc@2.39", ... },
    build_deps_list   = { "xim:gcc@15.1.0", ... },
    deps_exports = {
        ["xim:glibc@2.39"] = {
            loader  = "/home/.../xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2",
            libdirs = { "/home/.../xpkgs/xim-x-glibc/2.39/lib64",
                        "/home/.../xpkgs/xim-x-glibc/2.39/lib" },
            abi     = "linux-x86_64-glibc",
        },
        -- linux-headers 等无 exports 的包不出现在这里
    },
    self_exports = {
        loader = "/home/.../xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2",
        libdirs = { ... },
        abi = "linux-x86_64-glibc",
    },  -- 仅 glibc 自己装时非空；其他包是空表
}
```

### 7.2 elfpatch.lua 的决策流程

```
function elfpatch._apply()    -- 由 xlings.apply_elfpatch_auto() 调
  if _RUNTIME.elfpatch_user_skip then
    log.debug("elfpatch: user skip"); return
  end

  if _RUNTIME.elfpatch_user_override then
    -- 规则 1：使用 hook 给的 opts
    local opts = _RUNTIME.elfpatch_user_opts
    if opts.enable == false then return end
    return _patch_with_user_opts(opts)
  end

  -- predicate-driven
  local loader, abi = _resolve_loader_from_exports()
  if not loader then
    log.debug("elfpatch: no loader provider in deps; skipping"); return
  end

  local rpath = _build_rpath_from_exports()
  return _patch_install_dir({ loader = loader, rpath = rpath })
end

function _resolve_loader_from_exports()
  -- 规则 2：自己声明
  if _RUNTIME.self_exports and _RUNTIME.self_exports.loader then
    return _RUNTIME.self_exports.loader, _RUNTIME.self_exports.abi
  end
  -- 规则 3：runtime deps 里找
  local candidates = {}
  for dep_spec in pairs(_RUNTIME.deps_exports) do
    local e = _RUNTIME.deps_exports[dep_spec]
    if e.loader then candidates[#candidates+1] = e end
  end
  if #candidates == 0 then return nil end
  if #candidates == 1 then return candidates[1].loader, candidates[1].abi end
  -- ≥ 2，要求 hook 通过 elfpatch.set({interp_from = ...}) 选
  fail("multiple loader providers; require interp_from")
end

function _build_rpath_from_exports()
  local rpath = {}
  -- 自身 libdirs（exports 优先，否则约定）
  table.insert(rpath, _RUNTIME.install_dir .. "/lib64")  -- 简化版
  -- 每个 runtime dep 的 libdirs（exports 优先，否则约定）
  for _, dep_spec in ipairs(_RUNTIME.runtime_deps_list) do
    local e = _RUNTIME.deps_exports[dep_spec]
    if e and e.libdirs then
      for _, dir in ipairs(e.libdirs) do table.insert(rpath, dir) end
    else
      -- fallback 约定
      local dep_dir = pkginfo.dep_install_dir(dep_spec)
      for _, sub in ipairs({"lib64", "lib"}) do
        if os.isdir(path.join(dep_dir, sub)) then
          table.insert(rpath, path.join(dep_dir, sub)); break
        end
      end
    end
  end
  -- subos sysroot lib（保留现状）
  if _RUNTIME.subos_sysrootdir then
    table.insert(rpath, path.join(_RUNTIME.subos_sysrootdir, "lib"))
  end
  return rpath
end
```

### 7.3 完整 install-time pipeline

```
xlings installer.cppm
  ↓
1. node = resolved plan node（含 runtime_deps, build_deps, exports）
2. 收集 deps_exports：
   for dep in node.runtime_deps:
     dep_pkg = catalog.load_package(dep)
     if dep_pkg.exports.runtime.loader exists:
       deps_exports[dep] = {
         loader  = dep_install_dir + "/" + dep_pkg.exports.runtime.loader,
         libdirs = [dep_install_dir + "/" + d for d in dep_pkg.exports.runtime.libdirs],
         abi     = dep_pkg.exports.runtime.abi,
       }
3. self_exports = node.exports（同样 join 路径）
4. 构造 ExecutionContext，注入到 _RUNTIME
5. run install hook
   ├─ default: hook 不调 elfpatch
   ├─ override: hook 调 elfpatch.set({...})
   └─ skip: hook 调 elfpatch.skip()
6. apply_elfpatch_auto() → ep._apply() 按 5 条规则决策
7. patch_elf_loader_rpath(install_dir, {loader, rpath, scan, skip, shrink, ...})
   ├─ 扫描（默认 convention，e_machine 过滤）
   ├─ 找 ELF binary
   ├─ patchelf --set-interpreter <loader>
   ├─ patchelf --set-rpath <rpath>
   └─ optional --shrink-rpath
```

---

## 8. 实施方案（分 3 个独立 PR）

每个 PR 独立可上、独立可回滚、独立 CI 三平台跑通才进下一个。

### Phase 1 — Schema 基础设施（不改运行行为）

**Repos**: `mcpplibs/libxpkg`、`d2learn/xlings`

**libxpkg 改动**：

1. `src/xpkg.cppm`：`PlatformMatrix` 新增字段
   ```cpp
   struct ExportsRuntime {
       std::string loader;
       std::vector<std::string> libdirs;
       std::string abi;
   };
   struct ExportsBlock {
       ExportsRuntime runtime;
       // future: data, build
   };
   std::unordered_map<std::string, ExportsBlock> exports;  // platform → exports
   ```
2. `src/xpkg-loader.cppm`：parse `xpm.<platform>.exports.runtime.{loader, libdirs, abi}` → 写入 `PlatformMatrix.exports`
3. tests/test_loader.cppm：加 fixture `tests/fixtures/pkgindex/pkgs/e/exports-fixture.lua`，断言加载后字段正确

**xlings 改动**：

1. `src/core/xim/libxpkg/types/type.cppm`：`PlanNode` 加：
   ```cpp
   ExportsBlock exports;        // 当前包自己的 exports
   ```
2. `src/core/xim/resolver.cppm`：从 catalog 读 `pkg->xpm.exports[platform]` → 写入 `node.exports`
3. `src/core/xim/installer.cppm`：构造 `ctx.runtime_deps_list`、`ctx.build_deps_list`、`ctx.deps_exports`、`ctx.self_exports`
4. `xpkg-executor.cppm` (在 libxpkg 仓)：`inject_context` 注入这 4 个新字段到 `_RUNTIME`
5. **保留** `_RUNTIME.deps_list = union`（兼容老 `closure_lib_paths`）

**这一阶段完成后**：

- 元数据流通完全打通
- 现有 elfpatch.auto 行为完全不变
- 现有 4 个 consumer 一行不动还能跑

测试：libxpkg 8 → 10 个 LoaderTest（加 2 个 exports fixture）；xlings 自带 e2e 测试不退化。

**libxpkg 新版本号**：v0.0.33。`xim-pkgindex` 不动；xlings 升 add_requires 到 0.0.33。

### Phase 2 — elfpatch.lua 重写 + xlings 调度逻辑

**Repos**: `mcpplibs/libxpkg`（elfpatch.lua + xpkg-executor）

**libxpkg 改动**：

1. `src/lua-stdlib/xim/libxpkg/elfpatch.lua`：
   - 移除 `M.auto` / `M.is_auto` / `M.is_shrink` / `M.set_interpreter` / `M.set_rpath` / `M.apply_auto`
   - 加 `M.set(opts)` / `M.skip()` / 内部 `M._apply()`
   - `_RUNTIME.elfpatch_user_override` / `_RUNTIME.elfpatch_user_skip` / `_RUNTIME.elfpatch_user_opts` 状态
   - `M._apply()` 实现规则 0-4 决策
   - `closure_lib_paths` 改读 `_RUNTIME.runtime_deps_list` + `_RUNTIME.deps_exports`
   - 加 `e_machine` 检查到 `_collect_targets`
2. `src/xpkg-executor.cppm`：`apply_elfpatch_auto` 改调 `ep._apply()`

**xlings 改动**：无（Phase 1 已经把数据准备好）

**这一阶段完成后**：

- 现有 4 个 consumer 调 `elfpatch.auto({...})` 会**报错**（API 已删）
- 必须**先**在 Phase 3 改完 xim-pkgindex，**才能** ship Phase 2

→ Phase 2 + Phase 3 必须**联动 ship**，单个发不行。

**libxpkg 新版本号**：v0.1.0（break change）。

### Phase 3 — xim-pkgindex 迁移

**Repo**: `d2learn/xim-pkgindex`

**Provider 改动（5 个包）**：

| 包 | 改动 |
|---|---|
| `glibc.lua` | 加 `xpm.linux.exports.runtime.{loader, abi}`，删 install hook 里 `elfpatch.auto` |
| `musl-gcc.lua` | 同上（注意 musl 的 loader 路径不一样） |

**Consumer 改动（4 个包）**：

| 包 | 改动 |
|---|---|
| `binutils.lua` | 删 install hook 里 `glibc_dir = ...` + `loader = ...` + `elfpatch.auto({...})` 共 ~6 行 |
| `openssl.lua` | 同上 |
| `gcc.lua` | 同上（注意保留 `bins = { "bin", "libexec" }` → 改成 `elfpatch.set({ scan = { "bin", "libexec" } })` 因为 gcc 的 cc1 在 libexec） |
| `d2x.lua` | 同上 |

**migration 验证步骤**：

每改一个 consumer 包，本地：
1. 在隔离 XLINGS_HOME 装该包
2. `nm` / `readelf -l` 验证 INTERP / RPATH 是否正确
3. 实际跑一下二进制，确认能 link 加载

### Phase 4（可选 / 未来）— `data` / `build` capability 扩展

等出现真实需求（如 openssl ssl_certs、icu locale 数据）才做。

```lua
-- 未来 openssl.lua
exports = {
    runtime = { ... },
    data = {
        ssl_certs = "share/cert.pem",  -- xlings 自动注入 SSL_CERT_FILE env
    },
}
```

**v1 不做**，保持 schema 演进空间。

---

## 9. 测试方案

### 9.1 libxpkg 单元测试

| Test | 验证内容 |
|---|---|
| `LoaderTest.LoadPackage_ExportsRuntime` | parse `exports.runtime.{loader, libdirs, abi}` 字段正确 |
| `LoaderTest.LoadPackage_NoExports` | 没有 exports 字段时 `PlatformMatrix.exports` 为空（不崩） |
| `LoaderTest.LoadPackage_ExportsRuntime_OnlyLoader` | 只声明 loader 不声明 abi/libdirs 也能 parse |

### 9.2 xlings 单元 / e2e

| Test | 验证内容 |
|---|---|
| `e2e/elfpatch_predicate_test.sh` | consumer 不调 elfpatch；install 后 `readelf -l` 验证 INTERP / RPATH 正确 |
| `e2e/elfpatch_self_loader_test.sh` | glibc 自己装；其 ELF binary INTERP 指向自己 install_dir 的 loader（规则 2） |
| `e2e/elfpatch_skip_test.sh` | consumer 调 `elfpatch.skip()`；install 后 ELF 头**未变**（INTERP 仍是默认 `/lib64/...`） |
| `e2e/elfpatch_override_test.sh` | consumer 调 `elfpatch.set({interpreter = "/custom"})`；INTERP 是 `/custom` |
| `e2e/elfpatch_no_provider_test.sh` | consumer runtime deps 里没人声明 loader；ELF 头未变 |
| `e2e/elfpatch_multi_libc_fail_test.sh` | runtime deps 里同时有 glibc + musl；安装失败，错误信息要求 `interp_from` |
| `e2e/elfpatch_runtime_only_rpath_test.sh` | 包同时有 runtime_deps 和 build_deps；RPATH 只含 runtime_deps 的 lib（不含 build_deps）|
| `e2e/elfpatch_e_machine_test.sh` | 包里同时有 x86_64 和 aarch64 的 ELF；只 patch 当前 host arch 的 |

### 9.3 三平台 CI

- Linux：上述全部
- macOS：跳过纯 Linux 的 INTERP 测试，跑 install_name_tool 路径
- Windows：跳过（无 ELF 概念）

---

## 10. 风险与未决问题

### 10.1 跨仓库联动 ship 复杂度

Phase 2 + Phase 3 必须联动发版才能保持 xlings 工作。流程：

1. xim-pkgindex 改完，**新分支**等着
2. libxpkg 0.1.0 发版（同时 break elfpatch API + 加 exports）
3. xlings 升 add_requires 到 libxpkg 0.1.0，跑 CI 三平台
4. xim-pkgindex 新分支 merge 到 main

整个窗口里 xim-pkgindex main 分支必须**和 xlings main 兼容**。

**减缓措施**：xlings 加一个 transition 期支持 —— 老 `elfpatch.auto({...})` 调用在 0.1.0 内部映射到 `elfpatch.set({...})`，发 deprecation log。等所有 xpkg 迁完移除（半年期）。

### 10.2 dep 升级触发 re-patch（不在本次范围）

binutils 装时 INTERP 指 glibc 2.39 的绝对路径。用户后来 `xlings update glibc` 升到 2.42，binutils 的 INTERP 还指 2.39 路径，2.39 payload 删了的话 binutils 跑不起来。

**本次设计不解决**。需要 phase 5：reverse-dep index + dep 升级时反向 re-patch。规模大，单独立项。

**临时缓解**：xlings 不自动删除老 glibc payload（保留多版本，binutils 还能用）。这是当前已有行为。

### 10.3 `elfpatch.set` 未给 `interpreter` 也未给 `interp_from` 时

预期：用 predicate 推（ scope 内合并？还是 set 完全覆盖、不再推？）

按 5.4 决定：**set 完全覆盖**——predicate 不再跑。所以 `set({shrink=false})` 这种调用相当于关闭 auto patch（loader/rpath 都没设）。这可能不直观。

**优化方案**：让 `elfpatch.set` 在没指定 `interpreter` / `interp_from` 时**仍**走 predicate 解析（混合式）——但其他参数（shrink / scan / skip）走 hook 给的。

**待拍板**：覆盖式（清晰）vs 局部混合式（更符合人直觉）。

### 10.4 `data` / `build` capability 的未来 schema

`exports.data.*` 直接驱动 `vdata.envs` 注入 → 重写 xvm.add 调用约定。要不要在本次 schema 里就把 `data` 子树骨架放进去？

**建议不放**——YAGNI。现在加只会冻结一个未验证的 schema。等真需求出现再设计。

### 10.5 macOS 适配

本次主要针对 Linux ELF。macOS Mach-O 当前 `_patch_macho` 已在 elfpatch.lua 里，但 `exports.runtime.loader` 在 macOS 上没意义（macOS 用 `/usr/lib/dyld`，从不 patch）。

**v1 macOS 行为**：
- `exports.runtime` 字段可以不声明（macOS 上没人提供 INTERP）
- predicate 在 macOS 上永远走规则 4（不 patch INTERP），但 RPATH 闭包还是按 deps 算
- `_patch_macho` 走老路径（dylib 引用修正）

未来扩展：`exports.runtime.macos.dylibs` 这种 per-platform 子字段。**v1 不做**。

---

## 11. 决议清单（实施前要拍板）

| # | 决议项 | 当前倾向 |
|---|---|---|
| 1 | 字段名 `exports` vs `provides` | **`exports`** |
| 2 | schema 位置 `xpm.<platform>.exports`（与 deps 同级）vs top-level | **per-platform** |
| 3 | loader 字段名 `loader` vs `interp` | **`loader`** |
| 4 | 控制点：metadata 字段 vs hook API | **hook API only** |
| 5 | `elfpatch.set` 语义：覆盖式 vs 合并式 | **覆盖式**（待你确认） |
| 6 | 默认扫描：convention vs deep | **convention** |
| 7 | 老 `elfpatch.auto` 是否保留过渡兼容 | **保留半年** + deprecation log |
| 8 | `data` / `build` capability v1 是否预留 | **不预留** |
| 9 | dep 升级触发 re-patch | **本次不做**，未来单独项目 |

未拍板的项：5, 7。

---

## 12. 时间线估计（粗）

| Phase | 工时 | 阻塞依赖 |
|---|---|---|
| Phase 1（libxpkg + xlings 基础设施） | ~2 天 | 无 |
| Phase 2（elfpatch.lua 重写） | ~1.5 天 | 依赖 Phase 1 |
| Phase 3（xim-pkgindex 迁移 5+4 个包） | ~1 天 | 依赖 Phase 2 |
| 测试 + e2e 编写 | ~1.5 天 | 与 Phase 2/3 并行 |
| **总计** | **~6 天** | |

---

## 13. 落档 / Drop-this-design 标准

本设计**取代**当前 `elfpatch.auto({...})` 的命令式调用模式。完成后：

- `xim-pkgindex` 4 个 consumer install hook **0 行 elfpatch 调用**
- `glibc.lua` / `musl-gcc.lua` 加 ~7 行 `exports` schema，install hook **0 行 elfpatch**
- `libxpkg/src/lua-stdlib/xim/libxpkg/elfpatch.lua` 从 ~670 行收敛到 ~250 行（移除半 imperative auto API + 内部约定 hardcode 后净减少）
- consumer 不再硬编码 loader 路径 / dep 版本
- 多 ABI、跨架构、build_dep RPATH 污染、wrapper 包 RPATH 闭包等 5 个 latent 问题一次性收口

何时 drop（不再维护本设计、走下一个迭代）：
- 出现 multi-arch 同 host 共存需求时（需要 schema 加 arch 子树）
- 出现 dep-upgrade-triggered re-patch 真实痛点时（需要 reverse-dep machinery）
