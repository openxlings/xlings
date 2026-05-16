# 目录重构计划：core/ → src/ 四层架构

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 `core/` 平铺结构重构为 `src/` 四层分层架构，模块命名与目录结构一一对应，用户无感（CLI、TUI、二进制行为零变化）。

**Architecture:** 四层六边形架构 — L0(platform, libs) → L1(runtime) → L2(core + capabilities) → L3(ui, cli, agent, mcp)。每层只依赖同层或下层，禁止向上依赖。

**Tech Stack:** C++23 modules, GCC 15, xmake, ftxui 6.1.9

---

## 约定

- **分区 (partition)**: `<200` 行、常用 → 父模块 `export import :xxx` 自动重导出
- **子模块 (submodule)**: `>200` 行、独立 → 需显式 `import xlings.xxx.yyy`
- **目录规则**: `a.cppm` 与 `a/` 目录同级（mcpp-style-ref 约定）
- **命名风格**: PascalCase 类型, camelCase 成员, snake_case 函数, `_` 后缀私有

## 目标目录结构

```
src/
├── main.cpp
├── platform.cppm                    # xlings.platform (重导出分区)
├── platform/
│   ├── linux.cppm                   # xlings.platform:linux
│   ├── macos.cppm                   # xlings.platform:macos
│   └── windows.cppm                 # xlings.platform:windows
├── libs.cppm                        # xlings.libs (重导出子模块)
├── libs/
│   ├── json.cppm                    # xlings.libs.json
│   ├── json/                        # json.hpp 头文件
│   │   ├── json.hpp
│   │   └── LICENSE
│   └── tinyhttps.cppm               # xlings.libs.tinyhttps
├── runtime.cppm                     # xlings.runtime (重导出子模块)
├── runtime/
│   ├── event.cppm                   # xlings.runtime.event
│   ├── event_stream.cppm            # xlings.runtime.event_stream
│   ├── capability.cppm              # xlings.runtime.capability
│   └── task.cppm                    # xlings.runtime.task
│   # NOTE: 使用子模块而非分区，因 GCC 15 ICE bug — 重导出含 std::atomic/mutex 的分区会触发编译器崩溃
├── core.cppm                        # xlings.core (重导出子模块)
├── core/
│   ├── log.cppm                     # xlings.core.log
│   ├── utils.cppm                   # xlings.core.utils
│   ├── i18n.cppm                    # xlings.core.i18n
│   ├── config.cppm                  # xlings.core.config
│   ├── common.cppm                  # xlings.core.common
│   ├── profile.cppm                 # xlings.core.profile
│   ├── subos.cppm                   # xlings.core.subos
│   ├── cmdprocessor.cppm            # xlings.core.cmdprocessor
│   ├── xself.cppm                   # xlings.core.xself
│   ├── xself/
│   │   ├── init.cppm                # xlings.core.xself:init
│   │   └── install.cppm             # xlings.core.xself:install
│   ├── xim.cppm                     # xlings.core.xim (重导出)
│   ├── xim/
│   │   ├── commands.cppm            # xlings.core.xim.commands
│   │   ├── catalog.cppm             # xlings.core.xim.catalog
│   │   ├── index.cppm               # xlings.core.xim.index
│   │   ├── repo.cppm                # xlings.core.xim.repo
│   │   ├── resolver.cppm            # xlings.core.xim.resolver
│   │   ├── downloader.cppm          # xlings.core.xim.downloader
│   │   ├── installer.cppm           # xlings.core.xim.installer
│   │   └── libxpkg/
│   │       └── types/
│   │           ├── type.cppm         # xlings.core.xim.libxpkg.types.type
│   │           └── script.cppm       # xlings.core.xim.libxpkg.types.script
│   ├── xvm.cppm                     # xlings.core.xvm (新建，重导出)
│   └── xvm/
│       ├── types.cppm               # xlings.core.xvm.types
│       ├── db.cppm                  # xlings.core.xvm.db
│       ├── shim.cppm                # xlings.core.xvm.shim
│       └── commands.cppm            # xlings.core.xvm.commands
├── ui.cppm                          # xlings.ui (重导出分区)
├── ui/
│   ├── theme.cppm                   # xlings.ui:theme
│   ├── banner.cppm                  # xlings.ui:banner
│   ├── info_panel.cppm              # xlings.ui:info_panel
│   ├── selector.cppm                # xlings.ui:selector
│   ├── table.cppm                   # xlings.ui:table
│   └── progress.cppm               # xlings.ui:progress
└── cli.cppm                         # xlings.cli
```

## 模块名映射表

| # | 旧路径 | 新路径 | 旧模块名 | 新模块名 |
|---|--------|--------|----------|----------|
| 1 | core/main.cpp | src/main.cpp | — | — |
| 2 | core/platform.cppm | src/platform.cppm | xlings.platform | xlings.platform |
| 3 | core/platform/linux.cppm | src/platform/linux.cppm | xlings.platform:linux | xlings.platform:linux |
| 4 | core/platform/macos.cppm | src/platform/macos.cppm | xlings.platform:macos | xlings.platform:macos |
| 5 | core/platform/windows.cppm | src/platform/windows.cppm | xlings.platform:windows | xlings.platform:windows |
| 6 | core/json.cppm | src/libs/json.cppm | xlings.json | xlings.libs.json |
| 7 | core/json/ | src/libs/json/ | — | — |
| 8 | core/tinyhttps.cppm | src/libs/tinyhttps.cppm | xlings.tinyhttps | xlings.libs.tinyhttps |
| 9 | — | src/libs.cppm | — | xlings.libs (新建) |
| 10 | core/event.cppm | src/runtime/event.cppm | xlings.event | xlings.runtime:event |
| 11 | core/event_stream.cppm | src/runtime/event_stream.cppm | xlings.event_stream | xlings.runtime:event_stream |
| 12 | core/capability.cppm | src/runtime/capability.cppm | xlings.capability | xlings.runtime:capability |
| 13 | core/task.cppm | src/runtime/task.cppm | xlings.task | xlings.runtime:task |
| 14 | — | src/runtime.cppm | — | xlings.runtime (新建) |
| 15 | core/log.cppm | src/core/log.cppm | xlings.log | xlings.core.log |
| 16 | core/utils.cppm | src/core/utils.cppm | xlings.utils | xlings.core.utils |
| 17 | core/i18n.cppm | src/core/i18n.cppm | xlings.i18n | xlings.core.i18n |
| 18 | core/config.cppm | src/core/config.cppm | xlings.config | xlings.core.config |
| 19 | core/common.cppm | src/core/common.cppm | xlings.common | xlings.core.common |
| 20 | core/profile.cppm | src/core/profile.cppm | xlings.profile | xlings.core.profile |
| 21 | core/subos.cppm | src/core/subos.cppm | xlings.subos | xlings.core.subos |
| 22 | core/cmdprocessor.cppm | src/core/cmdprocessor.cppm | xlings.cmdprocessor | xlings.core.cmdprocessor |
| 23 | core/xself.cppm | src/core/xself.cppm | xlings.xself | xlings.core.xself |
| 24 | core/self/init.cppm | src/core/xself/init.cppm | xlings.xself:init | xlings.core.xself:init |
| 25 | core/self/install.cppm | src/core/xself/install.cppm | xlings.xself:install | xlings.core.xself:install |
| 26 | core/xim.cppm | src/core/xim.cppm | xlings.xim | xlings.core.xim |
| 27 | core/xim/commands.cppm | src/core/xim/commands.cppm | xlings.xim.commands | xlings.core.xim.commands |
| 28 | core/xim/catalog.cppm | src/core/xim/catalog.cppm | xlings.xim.catalog | xlings.core.xim.catalog |
| 29 | core/xim/index.cppm | src/core/xim/index.cppm | xlings.xim.index | xlings.core.xim.index |
| 30 | core/xim/repo.cppm | src/core/xim/repo.cppm | xlings.xim.repo | xlings.core.xim.repo |
| 31 | core/xim/resolver.cppm | src/core/xim/resolver.cppm | xlings.xim.resolver | xlings.core.xim.resolver |
| 32 | core/xim/downloader.cppm | src/core/xim/downloader.cppm | xlings.xim.downloader | xlings.core.xim.downloader |
| 33 | core/xim/installer.cppm | src/core/xim/installer.cppm | xlings.xim.installer | xlings.core.xim.installer |
| 34 | core/xim/libxpkg/types/type.cppm | src/core/xim/libxpkg/types/type.cppm | xlings.xim.libxpkg.types.type | xlings.core.xim.libxpkg.types.type |
| 35 | core/xim/libxpkg/types/script.cppm | src/core/xim/libxpkg/types/script.cppm | xlings.xim.libxpkg.types.script | xlings.core.xim.libxpkg.types.script |
| 36 | — | src/core/xvm.cppm | — | xlings.core.xvm (新建) |
| 37 | core/xvm/types.cppm | src/core/xvm/types.cppm | xlings.xvm.types | xlings.core.xvm.types |
| 38 | core/xvm/db.cppm | src/core/xvm/db.cppm | xlings.xvm.db | xlings.core.xvm.db |
| 39 | core/xvm/shim.cppm | src/core/xvm/shim.cppm | xlings.xvm.shim | xlings.core.xvm.shim |
| 40 | core/xvm/commands.cppm | src/core/xvm/commands.cppm | xlings.xvm.commands | xlings.core.xvm.commands |
| 41 | — | src/core.cppm | — | xlings.core (新建) |
| 42 | core/ui.cppm | src/ui.cppm | xlings.ui | xlings.ui |
| 43 | core/ui/theme.cppm | src/ui/theme.cppm | xlings.ui:theme | xlings.ui:theme |
| 44 | core/ui/banner.cppm | src/ui/banner.cppm | xlings.ui:banner | xlings.ui:banner |
| 45 | core/ui/info_panel.cppm | src/ui/info_panel.cppm | xlings.ui:info_panel | xlings.ui:info_panel |
| 46 | core/ui/selector.cppm | src/ui/selector.cppm | xlings.ui:selector | xlings.ui:selector |
| 47 | core/ui/table.cppm | src/ui/table.cppm | xlings.ui:table | xlings.ui:table |
| 48 | core/ui/progress.cppm | src/ui/progress.cppm | xlings.ui:progress | xlings.ui:progress |
| 49 | core/cli.cppm | src/cli.cppm | xlings.cli | xlings.cli |

## import 重命名表

所有 `.cppm` 和 `.cpp` 文件中的 `import` 语句需按以下规则替换：

| 旧 import | 新 import |
|-----------|-----------|
| `import xlings.json` | `import xlings.libs.json` |
| `import xlings.tinyhttps` | `import xlings.libs.tinyhttps` |
| `import xlings.event` | `import xlings.runtime` |
| `import xlings.event_stream` | `import xlings.runtime` |
| `import xlings.capability` | `import xlings.runtime` |
| `import xlings.task` | `import xlings.runtime` |
| `import xlings.log` | `import xlings.core.log` |
| `import xlings.utils` | `import xlings.core.utils` |
| `import xlings.i18n` | `import xlings.core.i18n` |
| `import xlings.config` | `import xlings.core.config` |
| `import xlings.common` | `import xlings.core.common` |
| `import xlings.profile` | `import xlings.core.profile` |
| `import xlings.subos` | `import xlings.core.subos` |
| `import xlings.cmdprocessor` | `import xlings.core.cmdprocessor` |
| `import xlings.xself` | `import xlings.core.xself` |
| `import xlings.xim` | `import xlings.core.xim` |
| `import xlings.xim.*` | `import xlings.core.xim.*` |
| `import xlings.xvm.*` | `import xlings.core.xvm.*` |

**注意**: runtime 分区模块（event, event_stream, capability, task）在外部通过 `import xlings.runtime` 统一访问。runtime 内部分区之间通过 `import :event` 等分区导入互相访问。

## Phase enum 变更

`event.cppm` 中的 `Phase` 从 enum 改为 `std::string`：

```cpp
// 旧
export enum class Phase { resolving, downloading, extracting, installing, configuring, verifying };
export struct ProgressEvent { Phase phase; float percent; std::string message; };

// 新
export struct ProgressEvent { std::string phase; float percent; std::string message; };
```

同时需更新 `src/ui/progress.cppm` 中对 `Phase` 枚举值的引用（改为字符串比较）。

---

## 执行步骤

### Task 1: 创建目录结构并移动文件

**Files:**
- 创建: `src/`, `src/platform/`, `src/libs/`, `src/libs/json/`, `src/runtime/`, `src/core/`, `src/core/xself/`, `src/core/xim/`, `src/core/xim/libxpkg/types/`, `src/core/xvm/`, `src/ui/`

**Step 1: 创建目录**
```bash
mkdir -p src/platform src/libs/json src/runtime src/core/xself src/core/xim/libxpkg/types src/core/xvm src/ui
```

**Step 2: git mv 所有文件**
```bash
# main
git mv core/main.cpp src/main.cpp

# platform (模块名不变)
git mv core/platform.cppm src/platform.cppm
git mv core/platform/linux.cppm src/platform/linux.cppm
git mv core/platform/macos.cppm src/platform/macos.cppm
git mv core/platform/windows.cppm src/platform/windows.cppm

# libs
git mv core/json.cppm src/libs/json.cppm
git mv core/json/json.hpp src/libs/json/json.hpp
git mv core/json/LICENSE src/libs/json/LICENSE
git mv core/tinyhttps.cppm src/libs/tinyhttps.cppm

# runtime
git mv core/event.cppm src/runtime/event.cppm
git mv core/event_stream.cppm src/runtime/event_stream.cppm
git mv core/capability.cppm src/runtime/capability.cppm
git mv core/task.cppm src/runtime/task.cppm

# core
git mv core/log.cppm src/core/log.cppm
git mv core/utils.cppm src/core/utils.cppm
git mv core/i18n.cppm src/core/i18n.cppm
git mv core/config.cppm src/core/config.cppm
git mv core/common.cppm src/core/common.cppm
git mv core/profile.cppm src/core/profile.cppm
git mv core/subos.cppm src/core/subos.cppm
git mv core/cmdprocessor.cppm src/core/cmdprocessor.cppm
git mv core/xself.cppm src/core/xself.cppm
git mv core/self/init.cppm src/core/xself/init.cppm
git mv core/self/install.cppm src/core/xself/install.cppm

# core/xim
git mv core/xim.cppm src/core/xim.cppm
git mv core/xim/commands.cppm src/core/xim/commands.cppm
git mv core/xim/catalog.cppm src/core/xim/catalog.cppm
git mv core/xim/index.cppm src/core/xim/index.cppm
git mv core/xim/repo.cppm src/core/xim/repo.cppm
git mv core/xim/resolver.cppm src/core/xim/resolver.cppm
git mv core/xim/downloader.cppm src/core/xim/downloader.cppm
git mv core/xim/installer.cppm src/core/xim/installer.cppm
git mv core/xim/libxpkg/types/type.cppm src/core/xim/libxpkg/types/type.cppm
git mv core/xim/libxpkg/types/script.cppm src/core/xim/libxpkg/types/script.cppm

# core/xvm
git mv core/xvm/types.cppm src/core/xvm/types.cppm
git mv core/xvm/db.cppm src/core/xvm/db.cppm
git mv core/xvm/shim.cppm src/core/xvm/shim.cppm
git mv core/xvm/commands.cppm src/core/xvm/commands.cppm

# ui (模块名不变)
git mv core/ui.cppm src/ui.cppm
git mv core/ui/theme.cppm src/ui/theme.cppm
git mv core/ui/banner.cppm src/ui/banner.cppm
git mv core/ui/info_panel.cppm src/ui/info_panel.cppm
git mv core/ui/selector.cppm src/ui/selector.cppm
git mv core/ui/table.cppm src/ui/table.cppm
git mv core/ui/progress.cppm src/ui/progress.cppm

# cli (模块名不变)
git mv core/cli.cppm src/cli.cppm
```

**Step 3: 删除空 core/ 目录**
```bash
rm -rf core/
```

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: move core/ to src/ directory structure"
```

---

### Task 2: 创建新的重导出模块

**Files:**
- 创建: `src/libs.cppm`, `src/runtime.cppm`, `src/core.cppm`, `src/core/xvm.cppm`

**Step 1: 创建 src/libs.cppm**
```cpp
export module xlings.libs;

export import xlings.libs.json;
export import xlings.libs.tinyhttps;
```

**Step 2: 创建 src/runtime.cppm**
```cpp
export module xlings.runtime;

export import :event;
export import :event_stream;
export import :capability;
export import :task;
```

**Step 3: 创建 src/core.cppm**
```cpp
export module xlings.core;

export import xlings.core.log;
export import xlings.core.utils;
export import xlings.core.i18n;
export import xlings.core.config;
export import xlings.core.common;
export import xlings.core.profile;
export import xlings.core.subos;
export import xlings.core.cmdprocessor;
export import xlings.core.xself;
export import xlings.core.xim;
export import xlings.core.xvm;
```

**Step 4: 创建 src/core/xvm.cppm**
```cpp
export module xlings.core.xvm;

export import xlings.core.xvm.types;
export import xlings.core.xvm.db;
export import xlings.core.xvm.shim;
export import xlings.core.xvm.commands;
```

**Step 5: Commit**
```bash
git add src/libs.cppm src/runtime.cppm src/core.cppm src/core/xvm.cppm
git commit -m "feat: add re-export modules for libs, runtime, core, xvm"
```

---

### Task 3: 更新所有 module 声明

按映射表更新每个 `.cppm` 文件的 `export module` 行。

**变更清单（仅列模块名有变化的文件）:**

| 文件 | 旧声明 | 新声明 |
|------|--------|--------|
| src/libs/json.cppm | `export module xlings.json` | `export module xlings.libs.json` |
| src/libs/tinyhttps.cppm | `export module xlings.tinyhttps` | `export module xlings.libs.tinyhttps` |
| src/runtime/event.cppm | `export module xlings.event` | `export module xlings.runtime:event` |
| src/runtime/event_stream.cppm | `export module xlings.event_stream` | `export module xlings.runtime:event_stream` |
| src/runtime/capability.cppm | `export module xlings.capability` | `export module xlings.runtime:capability` |
| src/runtime/task.cppm | `export module xlings.task` | `export module xlings.runtime:task` |
| src/core/log.cppm | `export module xlings.log` | `export module xlings.core.log` |
| src/core/utils.cppm | `export module xlings.utils` | `export module xlings.core.utils` |
| src/core/i18n.cppm | `export module xlings.i18n` | `export module xlings.core.i18n` |
| src/core/config.cppm | `export module xlings.config` | `export module xlings.core.config` |
| src/core/common.cppm | `export module xlings.common` | `export module xlings.core.common` |
| src/core/profile.cppm | `export module xlings.profile` | `export module xlings.core.profile` |
| src/core/subos.cppm | `export module xlings.subos` | `export module xlings.core.subos` |
| src/core/cmdprocessor.cppm | `export module xlings.cmdprocessor` | `export module xlings.core.cmdprocessor` |
| src/core/xself.cppm | `export module xlings.xself` | `export module xlings.core.xself` |
| src/core/xself/init.cppm | `export module xlings.xself:init` | `export module xlings.core.xself:init` |
| src/core/xself/install.cppm | `export module xlings.xself:install` | `export module xlings.core.xself:install` |
| src/core/xim.cppm | `export module xlings.xim` | `export module xlings.core.xim` |
| src/core/xim/commands.cppm | `export module xlings.xim.commands` | `export module xlings.core.xim.commands` |
| src/core/xim/catalog.cppm | `export module xlings.xim.catalog` | `export module xlings.core.xim.catalog` |
| src/core/xim/index.cppm | `export module xlings.xim.index` | `export module xlings.core.xim.index` |
| src/core/xim/repo.cppm | `export module xlings.xim.repo` | `export module xlings.core.xim.repo` |
| src/core/xim/resolver.cppm | `export module xlings.xim.resolver` | `export module xlings.core.xim.resolver` |
| src/core/xim/downloader.cppm | `export module xlings.xim.downloader` | `export module xlings.core.xim.downloader` |
| src/core/xim/installer.cppm | `export module xlings.xim.installer` | `export module xlings.core.xim.installer` |
| src/core/xim/libxpkg/types/type.cppm | `export module xlings.xim.libxpkg.types.type` | `export module xlings.core.xim.libxpkg.types.type` |
| src/core/xim/libxpkg/types/script.cppm | `export module xlings.xim.libxpkg.types.script` | `export module xlings.core.xim.libxpkg.types.script` |
| src/core/xvm/types.cppm | `export module xlings.xvm.types` | `export module xlings.core.xvm.types` |
| src/core/xvm/db.cppm | `export module xlings.xvm.db` | `export module xlings.core.xvm.db` |
| src/core/xvm/shim.cppm | `export module xlings.xvm.shim` | `export module xlings.core.xvm.shim` |
| src/core/xvm/commands.cppm | `export module xlings.xvm.commands` | `export module xlings.core.xvm.commands` |

**不变的模块（模块名未改变，仅路径变化）:**
- `src/platform.cppm` → `xlings.platform` (不变)
- `src/platform/*.cppm` → `xlings.platform:*` (不变)
- `src/ui.cppm` → `xlings.ui` (不变)
- `src/ui/*.cppm` → `xlings.ui:*` (不变)
- `src/cli.cppm` → `xlings.cli` (不变)

**Step: Commit**
```bash
git add -A && git commit -m "refactor: rename module declarations to match new directory structure"
```

---

### Task 4: 更新所有 import 语句

按 import 重命名表更新所有 `.cppm` 和 `.cpp` 文件中的 `import` 语句。

**特殊处理 — runtime 内部分区导入:**

runtime 分区文件（event.cppm, event_stream.cppm, capability.cppm, task.cppm）之间用分区导入：
- `src/runtime/event_stream.cppm`: `import xlings.event` → `import :event`
- `src/runtime/capability.cppm`: `import xlings.event` + `import xlings.event_stream` → `import :event` + `import :event_stream`
- `src/runtime/task.cppm`: `import xlings.event` + `import xlings.event_stream` + `import xlings.capability` → `import :event` + `import :event_stream` + `import :capability`

**外部文件引用 runtime:**
- 所有 `import xlings.event` / `import xlings.event_stream` / `import xlings.capability` / `import xlings.task` → `import xlings.runtime`

**main.cpp 更新:**
- `import xlings.cli` → 不变
- `import xlings.config` → `import xlings.core.config`
- `import xlings.platform` → 不变
- `import xlings.xvm.shim` → `import xlings.core.xvm.shim`

**测试文件 (tests/unit/test_main.cpp) 更新:**
- 所有 `import xlings.*` 按映射表更新

**Step: Commit**
```bash
git add -A && git commit -m "refactor: update all import statements to new module paths"
```

---

### Task 5: Phase enum → string 变更

**Files:**
- 修改: `src/runtime/event.cppm`
- 修改: `src/ui/progress.cppm`
- 修改: `tests/unit/test_main.cpp`

**Step 1: 修改 event.cppm**
- 删除 `export enum class Phase { ... };`
- 修改 `ProgressEvent` 的 `phase` 字段类型: `Phase phase` → `std::string phase`

**Step 2: 修改 progress.cppm**
- 将所有 `Phase::resolving` 等枚举比较改为字符串比较 `"resolving"` 等

**Step 3: 修改 test_main.cpp**
- 将所有测试中的 `Phase::xxx` 改为 `"xxx"` 字符串

**Step 4: Commit**
```bash
git add -A && git commit -m "refactor: change Phase from enum to string to avoid domain leakage into runtime"
```

---

### Task 6: 更新 xmake.lua

**Files:**
- 修改: `xmake.lua`

**Step 1: 更新路径**
```lua
-- 旧
target("xlings")
    add_files("core/main.cpp")
    add_files("core/**.cppm")
    add_includedirs("core/json")

-- 新
target("xlings")
    add_files("src/main.cpp")
    add_files("src/**.cppm")
    add_includedirs("src/libs/json")
```

同理更新 `xlings_tests` target:
```lua
-- 旧
target("xlings_tests")
    add_files("core/**.cppm")
    add_includedirs("core/json")

-- 新
target("xlings_tests")
    add_files("src/**.cppm")
    add_includedirs("src/libs/json")
```

**Step 2: Commit**
```bash
git add xmake.lua && git commit -m "build: update xmake.lua paths from core/ to src/"
```

---

### Task 7: 全量构建验证

**Step 1: 清理并构建**
```bash
rm -rf build && xmake build
```

**Step 2: 运行测试**
```bash
xmake build xlings_tests && xmake run xlings_tests
```
预期: 139 tests 全部通过。

**Step 3: 验证 CLI 行为**
```bash
./build/linux/x86_64/release/xlings --help
./build/linux/x86_64/release/xlings xim search lua
```

**Step 4: Commit (如有修复)**
```bash
git add -A && git commit -m "fix: resolve build issues from directory restructuring"
```

---

## 风险与回退

- **回退**: `git revert` 或 `git reset` 到 Task 1 之前的 commit
- **构建缓存**: 必须 `rm -rf build` 完整清理，模块接口变更后增量构建不可靠
- **CI/CD**: 确认 GitHub Actions 中无 `core/` 硬编码路径
- **xmake 缓存**: 如遇缓存问题 `xmake clean -a`

## 后续阶段（不在本次范围）

- **Phase 2**: 重构 core 模块使用 EventStream（消除 core→ui 直接依赖）
- **Phase 3**: 在 capabilities/ 目录实现真实 Capability
- **Phase 4**: Agent runtime（`xlings agent` 子命令）
- **Phase 5**: MCP Server
