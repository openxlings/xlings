# xlings C++23 重构版 - 顶层设计方案

> **状态**: 设计中 | **目标版本**: 0.1.1 | **语言标准**: C++23 (Modules)

---

## 一、项目定位与迁移目标

补充设计文档:

- `XLINGS_RES` 资源服务器配置与匹配规则: `docs/mcpp-version/xlings-res-config-design.md`
- `xlings_tests` 在 macOS 上的运行时崩溃分析: `docs/mcpp-version/gtest-macos-runtime-analysis.md`
- `XLINGS_HOME` bootstrap 包与统一目录模型: `docs/mcpp-version/unified-xlings-home-bootstrap-design.md`
- `xim` 已安装复用、subos attach/detach 与 payload 引用回收: `docs/mcpp-version/xim-subos-attach-detach-design.md`
- Shim 项目上下文在 CWD 离开项目时的恢复方案: `docs/mcpp-version/shim-project-context-design.md`

### 1.1 背景

xlings 原主体入口逻辑由 Lua (xim) 编写，借助 xmake 的 Lua 运行时驱动。C++23 重构版的目标是：

- 用现代 C++ 重写**主体入口与核心编排层**，替代原 Lua 主体
- xim（包管理器，Lua）和 xvm（版本管理器，Rust）**暂不迁移**，保持现有实现不变
- 在重构过程中同步进行**简化、优化和功能增强**

### 1.2 迁移范围

| 组件 | 原实现 | 新实现 | 迁移状态 |
|------|--------|--------|----------|
| 主入口 / CLI 分发 | Lua (xim.lua) | C++23 | ✅ 迁移 |
| 配置管理 | Lua (xconfig.lua) | C++23 | ✅ 迁移 |
| 平台抽象 | Lua (platform.lua) | C++23 | ✅ 迁移 |
| 日志 / i18n | Lua (log.lua, i18n.lua) | C++23 | ✅ 迁移 |
| 自管理 (self) | Lua (xself.lua) | C++23 | ✅ 迁移 |
| 环境隔离管理 | 无 | C++23 | 🆕 新增 |
| xim 包管理器 | Lua | Lua (保留) | ⏸ 暂不迁移 |
| xvm 版本管理器 | Rust | Rust (保留) | ⏸ 暂不迁移 |

### 1.3 核心目标

1. **自包含（Self-contained）**: 压缩包解压后无需额外安装，直接可用
2. **系统环境隔离**: 不依赖、不污染系统环境变量和全局路径
3. **多隔离环境**: 支持多个命名环境（类似 Python venv），可自由切换
4. **自定义数据目录**: `XLINGS_HOME` 和 `XLINGS_DATA` 完全可配置
5. **多平台**: Linux / macOS / Windows 统一支持
6. **代码风格**: 严格遵守 [mcpp-style-ref](https://github.com/mcpp-community/mcpp-style-ref)

---

## 二、整体架构

### 2.1 组件协作关系

```
┌─────────────────────────────────────────────────────────┐
│                    xlings (C++23)                        │
│                                                         │
│  main.cpp                                               │
│    └─► CmdProcessor ──┬─► install/remove/update/search  │
│                       │     └─► xim_exec()              │
│                       ├─► use                           │
│                       │     └─► xvm_exec()              │
│                       ├─► env (new/use/list/remove)     │
│                       │     └─► EnvManager              │
│                       └─► self (init/update/config/clean)│
│                             └─► XSelf                   │
│                                                         │
│  模块层: Config ◄── Platform ◄── Utils/Log/I18n/Json    │
└──────────┬─────────────────────────┬────────────────────┘
           │ xmake xim -P $HOME      │ xvm <subcommand>
           ▼                         ▼
┌──────────────────┐    ┌────────────────────────────────┐
│   xim (Lua)      │    │        xvm (Rust)               │
│  包管理器         │    │  版本管理器 + shim 机制          │
│  - install       │    │  - use / list / add / remove    │
│  - remove        │    │  - workspace 隔离               │
│  - update        │    │  - shim 透明版本切换             │
│  - search        │    └────────────────────────────────┘
└──────────────────┘
```

### 2.2 运行时目录结构

```
XLINGS_HOME/                        # 默认 ~/.xlings，可自定义
├── .xlings.json                    # 唯一配置文件
├── bin/
│   ├── xlings                      # 入口脚本（自包含模式设 XLINGS_HOME）
│   └── .xlings.real                # C++23 编译产物（主程序）
├── xim/                            # Lua 包管理器（自包含，随 HOME 移动）
├── config/
│   └── i18n/                       # 国际化资源
├── envs/                           # 多隔离环境根目录
│   ├── default/                    # 默认环境
│   │   └── data/                   # 该环境的 XLINGS_DATA
│   │       ├── bin/                # 安装的可执行文件
│   │       ├── lib/                # 库文件
│   │       └── xvm/                # xvm 数据
│   ├── work/                       # 自定义环境 "work"
│   │   └── data/
│   └── test/                       # 自定义环境 "test"
│       └── data/
└── cache/                          # 临时缓存（可安全清理）
```

---

## 三、C++23 模块设计

### 3.1 模块列表与职责

| 模块 | 文件 | 职责 |
|------|------|------|
| `xlings.cmdprocessor` | `core/cmdprocessor.cppm` | CLI 参数解析、命令注册与分发 |
| `xlings.config` | `core/config.cppm` | 路径配置、`.xlings.json` 读取、单例管理 |
| `xlings.env` | `core/env.cppm` | 多隔离环境管理、环境切换 (**新增**) |
| `xlings.platform` | `core/platform.cppm` | 平台抽象（环境变量、命令执行、路径分隔符） |
| `xlings.platform:linux` | `core/platform/linux.cppm` | Linux 平台实现 |
| `xlings.platform:macos` | `core/platform/macos.cppm` | macOS 平台实现 |
| `xlings.platform:windows` | `core/platform/windows.cppm` | Windows 平台实现 |
| `xlings.xself` | `core/xself.cppm` | 自管理（init / update / config / clean） |
| `xlings.log` | `core/log.cppm` | 日志输出（带前缀、颜色、级别） |
| `xlings.i18n` | `core/i18n.cppm` | 国际化字符串查找 |
| `xlings.utils` | `core/utils.cppm` | 文件读写、环境变量获取等通用工具 |
| `xlings.json` | `core/json.cppm` | nlohmann/json 封装（自包含头文件） |

### 3.2 模块依赖关系

```
main.cpp
  └── xlings.cmdprocessor
        ├── xlings.config
        │     ├── xlings.json
        │     ├── xlings.platform
        │     └── xlings.utils
        ├── xlings.env          (新增)
        │     ├── xlings.config
        │     ├── xlings.platform
        │     └── xlings.json
        ├── xlings.platform
        │     ├── xlings.platform:linux
        │     ├── xlings.platform:macos
        │     └── xlings.platform:windows
        ├── xlings.xself
        │     ├── xlings.config
        │     └── xlings.platform
        └── xlings.log
              └── xlings.platform
```

### 3.3 模块文件结构规范（mcpp-style-ref 2.1）

每个 `.cppm` 文件遵循以下结构：

```cpp
// 0. 全局模块片段（仅当需要传统头文件时）
module;
#include <cstdio>  // 仅在此区域 include，其余全用 import

// 1. 模块声明
export module xlings.xxx;

// 2. 模块导入
import std;
import xlings.config;
// import :partition;  // 分区导入（如有）

// 3. 接口导出与实现
export namespace xlings::xxx {
    // 公开接口
}

namespace xlings::xxx {
    // 内部实现
}
```

---

## 四、自包含与环境隔离设计

### 4.1 自包含检测机制

xlings 主程序启动时，通过以下逻辑自动判断运行模式：

```
启动时检测（在 Config 初始化之前）:
  若 argv[0] 同级目录 (或上级目录) 存在 xim/ 目录
    → 自包含模式: XLINGS_HOME = argv[0] 所在目录的父目录
  否则
    → 安装模式:   XLINGS_HOME 按优先级规则解析
```

实现位于 `core/config.cppm` 的 `Config` 构造函数中，通过 `platform::get_executable_dir()` 获取可执行文件路径。

### 4.2 路径解析优先级

`XLINGS_HOME` 解析优先级（从高到低）：

```
1. XLINGS_HOME 环境变量（用户手动设置）
2. 可执行文件同级 ../  存在 xim/ → 自包含模式
3. 默认值: $HOME/.xlings  (Linux/macOS)
            %USERPROFILE%\.xlings  (Windows)
```

`XLINGS_DATA`（当前活跃环境的数据目录）解析优先级：

```
1. XLINGS_DATA 环境变量（最高优先级，跳过环境系统）
2. .xlings.json 中的 "activeEnv" 字段
     → $XLINGS_HOME/envs/<activeEnv>/data
3. .xlings.json 中的 "data" 字段（兼容旧版直接指定）
4. 默认: $XLINGS_HOME/envs/default/data
```

### 4.3 `.xlings.json` 配置文件规范

```json
{
  "version": "0.2.0",
  "activeEnv": "default",
  "mirror": "",
  "lang": "auto",
  "data": "",
  "envs": {
    "default": {
      "data": ""
    },
    "work": {
      "data": "/custom/path/to/work-data"
    }
  },
  "xim": {
    "repos": []
  }
}
```

- `activeEnv`: 当前激活的环境名称，默认 `"default"`
- `envs.<name>.data`: 为空则使用 `$XLINGS_HOME/envs/<name>/data`，非空则使用自定义路径（支持绝对路径）
- `data`: 兼容旧版，若 `activeEnv` 为空时使用此字段

### 4.4 系统隔离原则

- xlings **不修改系统** `PATH`、`LD_LIBRARY_PATH` 等全局变量（由 shell profile 按需 source）
- xim 调用始终携带 `-P $XLINGS_HOME`，保证 xim 读写的是 xlings 自有目录
- xvm 数据存于 `$XLINGS_DATA/xvm/`，不使用系统级配置
- `bin/xlings` 入口脚本负责在启动前设置 `XLINGS_HOME`（自包含模式），主程序再设置 `XLINGS_DATA`

---

## 五、多环境管理设计

### 5.1 设计思路

多环境（Multi-Environment）类似 Python venv 或 Rust toolchain 的概念：

- 每个环境拥有独立的 `data/` 目录（bin、lib、xvm 数据等完全隔离）
- 切换环境仅修改 `.xlings.json` 中的 `activeEnv` 字段
- 不同环境可安装不同版本的工具，互不干扰

### 5.2 `xlings env` 子命令

| 命令 | 说明 |
|------|------|
| `xlings env list` | 列出所有环境，标记当前激活环境 |
| `xlings env new <name>` | 创建新环境（创建目录结构，写入配置） |
| `xlings env use <name>` | 切换当前激活环境 |
| `xlings env remove <name>` | 删除环境（需非 default，且非当前激活） |
| `xlings env info [name]` | 显示环境详情（路径、已安装工具数等） |

### 5.3 `xlings.env` 模块接口设计

```cpp
// core/env.cppm
export module xlings.env;

import std;
import xlings.config;

export namespace xlings::env {

struct EnvInfo {
    std::string name;
    std::filesystem::path dataDir;
    bool isActive;
};

// 列出所有环境
std::vector<EnvInfo> list_envs();

// 创建新环境
int create_env(const std::string& name, const std::filesystem::path& customData = {});

// 切换活跃环境（修改 .xlings.json）
int use_env(const std::string& name);

// 删除环境
int remove_env(const std::string& name);

// 获取环境详情
std::optional<EnvInfo> get_env(const std::string& name);

// 命令入口（由 cmdprocessor 调用）
int run(int argc, char* argv[]);

} // namespace xlings::env
```

### 5.4 环境切换流程

```
xlings env use work
  │
  ├─ 读取 .xlings.json
  ├─ 检查 "work" 是否存在于 envs 配置中
  ├─ 若不存在 → 报错提示用 "xlings env new work" 创建
  ├─ 更新 "activeEnv" = "work"
  ├─ 写回 .xlings.json
  └─ 打印: [xlings:env] switched to 'work' (data: /path/to/work/data)
```

---

## 六、多平台支持

### 6.1 平台差异处理策略

| 特性 | Linux | macOS | Windows |
|------|-------|-------|---------|
| HOME 目录 | `$HOME` | `$HOME` | `%USERPROFILE%` |
| 路径分隔符 | `/` | `/` | `\` (兼容 `/`) |
| 可执行文件扩展名 | 无 | 无 | `.exe` |
| 环境变量设置 | `setenv()` | `setenv()` | `_putenv_s()` |
| 命令执行 | `std::system()` | `std::system()` | `std::system()` |
| 静态链接 | `-static-libstdc++ -static-libgcc` | 不支持 | MSVC 静态 CRT |
| xmake 调用方式 | `xmake xim -P ...` | `xmake xim -P ...` | `xmake xim -P ...` |

### 6.2 平台模块分区规范

```cpp
// core/platform.cppm - 主模块（聚合导出）
export module xlings.platform;

export import :linux;    // Linux 实现
export import :macos;    // macOS 实现
export import :windows;  // Windows 实现

// 各分区使用条件编译区分平台实现
// 非当前平台的分区提供空实现或存根
```

平台分区内的函数通过 `platform_impl` 命名空间提供，主模块统一 `using` 导出：

```cpp
export namespace xlings::platform {
    using platform_impl::get_home_dir;
    using platform_impl::set_env_variable;
    using platform_impl::get_executable_path;  // 新增：用于自包含检测
    using platform_impl::run_command_capture;
    // ...
}
```

### 6.3 新增平台函数

| 函数 | 用途 |
|------|------|
| `get_executable_path()` | 获取当前进程可执行文件的绝对路径（用于自包含检测） |
| `get_home_dir()` | 获取用户 HOME 目录（跨平台） |
| `set_env_variable(k, v)` | 设置进程环境变量 |
| `run_command_capture(cmd)` | 执行命令并捕获输出（返回 exit code + stdout） |
| `exec(cmd)` | 执行命令，继承 stdio（交互式子进程） |
| `open_in_shell(path)` | 用系统默认方式打开文件/目录（可选） |

### 6.4 构建系统多平台配置

```lua
-- xmake.lua 多平台差异配置
target("xlings")
    set_kind("binary")
    add_files("core/main.cpp", "core/**.cppm")
    add_includedirs("core/json")
    set_policy("build.c++.modules", true)

    if is_plat("linux") then
        -- 静态链接 C++ 运行时，避免依赖 SDK 路径
        add_ldflags("-static-libstdc++", "-static-libgcc", {force = true})
        add_ldflags("-Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2", {force = true})
    elseif is_plat("macosx") then
        add_ldflags("-lc++experimental", {force = true})
    elseif is_plat("windows") then
        -- Windows: MSVC 或 clang-cl，使用静态 CRT
        add_cxflags("/MT", {force = true})
    end
```

---

## 七、代码风格规范（mcpp-style-ref 落地）

> 完整规范参考: https://github.com/mcpp-community/mcpp-style-ref

### 7.1 标识符命名

| 类型 | 风格 | 示例 |
|------|------|------|
| 类型名（class/struct/enum） | 大驼峰 | `CommandProcessor`, `EnvInfo`, `PathInfo` |
| 函数名 | snake_case | `list_envs()`, `use_env()`, `get_home_dir()` |
| 公有数据成员 | 小驼峰 | `dataDir`, `isActive`, `homeDir` |
| 私有数据成员 | 小驼峰 + `_` 后缀 | `paths_`, `commands_`, `mirror_` |
| 私有函数 | snake_case + `_` 后缀 | `parse_()`, `cmd_init_()` |
| 全局/静态变量 | `g` 前缀 + 小驼峰 | `gRundir` |
| 命名空间 | 全小写 | `xlings`, `xlings::platform`, `xlings::env` |
| 常量 | 全大写下划线 或 `constexpr` 大驼峰 | `VERSION`, `PATH_SEPARATOR` |

### 7.2 模块命名规范

遵循 mcpp-style-ref 2.4（目录路径映射为模块名层级）：

```
core/cmdprocessor.cppm     → export module xlings.cmdprocessor;
core/platform.cppm         → export module xlings.platform;
core/platform/linux.cppm   → export module xlings.platform:linux;
core/env.cppm              → export module xlings.env;
```

### 7.3 强制要求

- **禁止** 在模块文件中使用 `#include <系统头文件>`，统一使用 `import std;`
- **允许** 在全局模块片段（`module;` 之后，`export module` 之前）中 `#include` 第三方 C 库头文件
- 所有对外接口必须使用 `export` 显式导出，未导出的视为模块内部实现
- 优先在 `.cppm` 文件中同时提供接口声明与实现（接口与实现不分离），除非实现过大
- **不使用** 宏（`#define`）定义逻辑，仅在不得不兼容 C 库时使用

### 7.4 类结构布局规范

```cpp
export namespace xlings::env {

class EnvManager {

public:  // 1. Rule of Five（构造/析构/拷贝/移动）
    EnvManager();
    ~EnvManager() = default;
    EnvManager(const EnvManager&) = delete;
    EnvManager& operator=(const EnvManager&) = delete;

public:  // 2. 公有接口
    std::vector<EnvInfo> list_envs() const;
    int use_env(const std::string& name);

private:  // 3. 私有成员与辅助函数
    std::filesystem::path configPath_;

    void reload_();
    bool env_exists_(const std::string& name) const;
};

} // namespace xlings::env
```

### 7.5 错误处理风格

- 优先使用返回值（`int` exit code，0 成功，非 0 失败）而非异常
- 错误信息使用 `xlings::log::error(...)` 输出到 stderr
- 使用 `std::optional<T>` 表示可能为空的返回值
- 不使用 `std::expected` 或自定义错误类型（保持简单）

---

## 八、构建与发布

### 8.1 构建依赖

| 依赖 | 版本要求 | 用途 |
|------|---------|------|
| GCC / Clang | GCC 15+ / Clang 18+ | C++23 Modules 支持 |
| xmake | 最新稳定版 | C++ 构建系统 |
| Cargo | 1.75+ | xvm Rust 构建 |
| xmake (lua runtime) | 随 xmake 安装 | xim 运行时（自包含） |

### 8.2 构建命令

```bash
# 配置（指定支持 C++23 Modules 的 GCC SDK）
xmake f -m release --sdk=/path/to/gcc-15

# 构建 C++23 主程序
xmake build

# 构建 xvm（Rust）
cd core/xvm && cargo build --release

# 一键构建并打包（Linux）
./tools/linux_release.sh [output_dir]
```

### 8.3 自包含发布包结构

```
xlings-<version>-<platform>-<arch>/
├── bin/
│   ├── xlings              # 入口脚本（设置 XLINGS_HOME 后调用主程序）
│   └── .xlings.real        # C++23 编译产物
├── xim/                    # Lua 包管理器（完整，自包含）
│   └── xmake.lua           # xmake 入口（供 xmake xim -P 调用）
├── config/
│   └── i18n/               # 国际化资源
├── data/
│   └── xim/
│       └── xim-pkgindex/   # 捆绑的包索引（避免首次拉取）
└── .xlings.json            # 默认配置文件
```

自包含检测：`bin/xlings` 入口脚本检测自身目录，设置 `XLINGS_HOME` 再调用 `.xlings.real`。

### 8.4 CI/CD 策略

| 平台 | 触发条件 | 产物 |
|------|---------|------|
| Linux x86_64 | push / PR | `xlings-<ver>-linux-x86_64.tar.gz` |
| macOS arm64 | push / PR | `xlings-<ver>-macosx-arm64.tar.gz` |
| Windows x86_64 | push / PR | `xlings-<ver>-windows-x86_64.zip` |

---

## 九、后续迁移路线图

> 本文档聚焦于当前阶段（主体 C++ 重构），以下为后续规划，不在本阶段实现范围内。

| 阶段 | 目标 | 说明 |
|------|------|------|
| v0.2.0 | 主体 C++ 重构 + 多环境支持 | 本文档描述的内容 |
| v0.3.0 | xim 部分功能迁移至 C++ | 将 install/remove 核心逻辑用 C++ 实现 |
| v0.4.0 | xvm 集成至 C++ 主程序 | 去除 Rust 依赖，统一为 C++ |
| v1.0.0 | 全 C++ 自包含 | 无外部运行时依赖（无需 xmake lua 运行时） |

---

## 十、参考资料

- [mcpp-style-ref](https://github.com/mcpp-community/mcpp-style-ref) — 现代 C++ 编码/项目风格参考
- [xlings GitHub](https://github.com/d2learn/xlings) — 项目主仓库
- [xmake 官方文档](https://xmake.io) — 构建系统文档
- [C++23 Modules (cppreference)](https://en.cppreference.com/w/cpp/language/modules) — 模块语言规范
