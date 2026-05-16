# 默认静默 `pkgindex-build.lua` 的进度输出（备选方案，暂缓）

## Status

**搁置**。本文档作为后续实现的设计备份。优化 1 / 优化 2 / `--use` 已经在 PR #234
合并，本议题独立于那两项。

## Context

跑 `xlings update`（或任意触发 catalog rebuild 的路径）时，sub-index repo 自带的
`pkgindex-build.lua` 会逐个文件打印一行：

```
[1/15] scode::/.../pkgs/l/libffi.lua
[2/15] scode::/.../pkgs/l/linux-headers.lua
...
```

实测：默认、`-v / --verbose`、`-q / --quiet` **三种模式下都打印**，xlings 的 log 等级
对它**完全无效**。

## Root Cause

```
xlings update
  └─ IndexManager::rebuild()                  [xlings: src/core/xim/index.cppm:138]
       └─ xpkg::build_index(repo_dir)         [libxpkg: src/xpkg-loader.cppm:482]
            └─ run_pkgindex_build(repo_dir)   [libxpkg: src/xpkg-loader.cppm:385]
                 └─ 执行 repo 自带 pkgindex-build.lua
                      └─ cprint("[${green}%d/%d${clear}] ns::%s", ...)   ★
```

打印实际由 **包索引仓库自己写的 `pkgindex-build.lua`** 发出，例如
`xim-pkgindex-awesome/pkgindex-build.lua:44`：

```lua
cprint("[${green}%d/%d${clear}] awesome::%s", built_index_cnt, all_index_cnt, file)
```

`cprint` 在 libxpkg 的 **build sandbox**（`xpkg-loader.cppm:360-373`）注入：

```cpp
"cprint = function(...)\n"
"  local args = {...}\n"
"  local fmt = args[1] or ''\n"
"  fmt = fmt:gsub('%${.-}', '')\n"
"  if #args > 1 then\n"
"    print(string.format(fmt, table.unpack(args, 2)))\n"
"  else\n"
"    print(fmt)\n"
"  end\n"
"end\n"
```

实质：**Lua `print` 的薄包装，无 level 检查**。xlings 的 log 系统碰不到这条通路。

### libxpkg 有三套 Lua sandbox，仅 build sandbox 缺 log

| Sandbox | 用途 | 当前 `cprint` | 有 log level？ |
| --- | --- | --- | --- |
| loader sandbox (`xpkg-loader.cppm:85`) | 解析单个 `*.lua` 包定义 | `cprint or print` | ❌ |
| **build sandbox** (`xpkg-loader.cppm:250`) | **跑 `pkgindex-build.lua`** | **薄包装 print** | ❌ |
| executor sandbox (`xpkg-executor.cppm`) | 跑 install/config/uninstall hook | 用 `xim.libxpkg.log` 模块 | ✅，xlings 已透传 |

## 受影响仓库

主仓 `d2learn/xim-pkgindex` 没有 `pkgindex-build.lua`，不受影响。
**3 个 sub-index repo** 受影响（脚本结构同源，仅 namespace 字符串不同）：

| 仓库 | 文件 | 行 |
| --- | --- | --- |
| `d2learn/xim-pkgindex-awesome` | `pkgindex-build.lua` | 44, 47 |
| `d2learn/xim-pkgindex-d2x` | `pkgindex-build.lua` | 40, 43 |
| `d2learn/xim-pkgindex-scode` | `pkgindex-build.lua` | 47, 50 |

## 候选方案对比

| 方案 | 改 libxpkg | 改 xlings | 改 sub-index 仓 build.lua | 备注 |
| --- | --- | --- | --- | --- |
| **C：libxpkg 加 `set_build_verbose()` C++ API** | ✅ ~10 行 | ✅ 1 行 | ❌ 不动 | 协议是 C++，未来新增 sub-index repo 默认就静默 |
| **E：约定 env var，`pkgindex-build.lua` 自己读** | ❌ 不动 | ✅ 1 行 setenv | ✅ 每个仓改头部 + 替换 cprint | 协议散落在 N 个仓，新增 repo 必须遵守，否则又踩坑 |
| **F：libxpkg 注入 lua 全局 `_BUILD_VERBOSE`** | ✅ 1 行 | ✅ 1 行 | ✅ 改脚本 | 三处都得动，最碎 |

## 推荐：方案 C

### 实现

**libxpkg 侧**（`src/xpkg-loader.cppm`）：

```cpp
// 在 namespace mcpplibs::xpkg::loader_detail 里加
inline bool g_build_verbose_ = false;

// 导出 API
export namespace mcpplibs::xpkg {
    void set_build_verbose(bool v) {
        loader_detail::g_build_verbose_ = v;
    }
}

// 在 register_build_sandbox() 注入 cprint 之前透 flag
lua::pushboolean(L, loader_detail::g_build_verbose_ ? 1 : 0);
lua::setglobal(L, "_BUILD_VERBOSE");

// cprint 注入加一行 level 检查
lua::L_dostring(L,
    "cprint = function(...)\n"
    "  if not _BUILD_VERBOSE then return end\n"   // ← 新增
    "  local args = {...}\n"
    "  local fmt = args[1] or ''\n"
    "  fmt = fmt:gsub('%${.-}', '')\n"
    "  if #args > 1 then\n"
    "    print(string.format(fmt, table.unpack(args, 2)))\n"
    "  else\n"
    "    print(fmt)\n"
    "  end\n"
    "end\n"
);
```

**xlings 侧**（`src/core/xim/index.cppm`，`IndexManager::rebuild()` 中）：

```cpp
xpkg::set_build_verbose(log::level() <= log::Level::Debug);
auto result = xpkg::build_index(repoDir_);
```

**最后**：bump `add_requires("mcpplibs-xpkg X.Y.Z")` 到含此改动的 libxpkg 版本。

### 行为表

| 操作 | 默认 / `-q` | `-v / --verbose` |
| --- | --- | --- |
| `xlings update` 触发 sub-index `pkgindex-build.lua` | **静默** | 像今天一样逐文件 `[N/M] ns::path.lua` |
| `xlings update` 主仓（无 build 脚本） | 不变 | 不变 |
| `xlings install` 装包/config hook (executor sandbox) | 不变 | 不变 |
| 包定义解析 (loader sandbox) | 不变 | 不变 |
| xlings/libxpkg 的 C++ 输出（`std::println` / `log::*`） | 不变 | 不变 |

## 实施顺序（之后做时按此走）

1. libxpkg 提 PR：加 `set_build_verbose` + 改 build sandbox 的 `cprint`，发 release tag
2. xlings：bump `mcpplibs-xpkg` 版本，加一行 `set_build_verbose(...)`
3. e2e 验证：`xlings update`、`xlings update -v`、`xlings update -q` 三种模式输出符合上表

3 个 sub-index 仓的 `pkgindex-build.lua` **不需要任何改动**。
