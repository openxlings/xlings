# xlings 项目架构

## 目录结构

```
xlings/
├── core/                   # 核心代码 (C++23 单二进制)
│   ├── main.cpp            # C++ 入口 (multicall: argv[0] 分发)
│   ├── cli.cppm            # CLI 命令定义 + 分发
│   ├── config.cppm         # 配置管理 (三层 .xlings.json)
│   ├── log.cppm            # 结构化日志
│   ├── i18n.cppm           # 多语言 (编译期嵌入)
│   ├── ui.cppm             # 控制台 UI (ftxui)
│   ├── platform.cppm       # 跨平台抽象
│   ├── platform/           # 平台实现 (linux, macos, windows)
│   ├── subos.cppm          # Subos 管理 (版本视图环境)
│   ├── xself.cppm          # 自安装/更新
│   ├── xim.cppm            # xim 模块入口 (re-export 所有子模块)
│   ├── xim/                # 包管理器 (C++23 原生实现)
│   │   ├── types.cppm      # 类型定义 (PlanNode, InstallPlan, DownloadTask, ...)
│   │   ├── repo.cppm       # Git 仓库同步 (clone/pull + 节流)
│   │   ├── index.cppm      # 索引管理 (构建/搜索/版本匹配，基于 libxpkg)
│   │   ├── resolver.cppm   # DAG 依赖解析 (DFS 拓扑排序 + 循环检测)
│   │   ├── downloader.cppm # 并行下载 (jthread 并发 + SHA256 校验)
│   │   ├── installer.cppm  # 安装编排 (download → install → config → 注册版本)
│   │   └── commands.cppm   # 高层命令 (install/remove/search/list/info/update)
│   └── xvm/                # 版本管理器 (C++23 原生实现，已融合为核心模块)
│       ├── types.cppm      # VData, VInfo, VersionDB, Workspace 类型定义
│       ├── db.cppm         # VersionDB CRUD + 模糊版本匹配 + JSON 序列化
│       ├── shim.cppm       # Multicall shim 分发 (argv[0] → 版本查找 → execvp)
│       └── commands.cppm   # xvm 命令 (use, list_versions, register_version)
├── config/                 # 模板与 i18n
│   ├── i18n/, shell/
│   └── xlings.json         # 默认配置模板
├── tests/
│   ├── unit/test_main.cpp  # 81 个 gtest 单元测试 (18 个测试套件)
│   └── e2e/                # 端到端测试脚本
├── tools/                  # 安装脚本、模板、release 脚本
├── docs/
├── xmake.lua
└── README.md
```

## 单二进制架构

xlings 采用 **multicall 单二进制** 设计，所有功能融合在一个可执行文件中。

### argv[0] 分发机制

```
argv[0]      → 行为
─────────────────────────────────
xlings       → CLI 主入口
xim          → 等同 xlings（别名）
gcc/node/... → shim 模式：查版本 → exec 真实程序
```

### HOME 目录结构

```
~/.xlings/                          # XLINGS_HOME
├── xlings                          # 单二进制
├── subos/
│   ├── default/                    # 默认版本视图
│   │   ├── bin/                    # shim 硬链接 → ../../xlings
│   │   │   ├── gcc → ../../xlings
│   │   │   └── node → ../../xlings
│   │   ├── lib/                    # 库文件链接
│   │   ├── usr/                    # headers
│   │   └── .xlings.json           # 版本视图（workspace）
│   ├── dev/                        # 用户创建的 subos
│   └── current → default           # 当前活跃 subos
├── data/
│   ├── xim-pkgindex/               # 包索引 git 仓库
│   ├── xpkgs/<name>/<ver>/         # 包安装目录（全局共享）
│   └── cache/                      # 下载缓存
└── .xlings.json                    # 全局配置
```

## 配置系统 (.xlings.json)

### 三层配置层级

```
优先级: 项目配置 > 当前 subos 配置 > 全局配置 > 硬编码默认值
```

| 层级 | 位置 | 职责 |
|------|------|------|
| 全局 | `~/.xlings/.xlings.json` | versions（全局版本数据库）、lang、mirror、activeSubos |
| Subos | `~/.xlings/subos/<name>/.xlings.json` | workspace（版本视图，指定每个工具的活跃版本） |
| 项目 | `<project>/.xlings.json` | workspace 覆盖、本地 versions 补充 |

### 全局配置示例

```json
{
  "lang": "en",
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "versions": {
    "gcc": {
      "type": "program",
      "filename": "gcc",
      "versions": {
        "15.1.0": { "path": "/usr/bin", "alias": ["15"] },
        "14.2.0": { "path": "${XLINGS_HOME}/data/xpkgs/gcc/14.2.0/bin" }
      },
      "bindings": {
        "g++": { "15.1.0": "g++-15", "14.2.0": "g++-14" }
      }
    }
  },
  "subos": { "default": {} }
}
```

### Subos 配置示例

```json
{
  "workspace": {
    "gcc": "15.1.0",
    "node": "22.0.0"
  }
}
```

### 项目配置示例

```json
{
  "workspace": {
    "gcc": "14.2.0"
  }
}
```

## 模块说明

### xim - 包管理器 (`core/xim/`)

C++23 原生实现的跨平台包安装管理器。

**架构：**
```
用户 → xlings(C++) → xim::cmd_install/remove/search/...
                         ↓
                    IndexManager (libxpkg 构建索引)
                         ↓
                    Resolver (DAG 依赖解析)
                         ↓
                    Downloader (并行下载 + SHA256)
                         ↓
                    Installer (libxpkg executor 运行 Lua hook)
                         ↓
                    处理 _XVM_OPS (Lua hook 收集的 xvm 操作)
                         ↓
                    xvm::add_version / install_headers (注册到版本数据库)
```

**子模块：**

| 模块 | 文件 | 职责 |
|------|------|------|
| `xlings.xim.types` | `types.cppm` | PlanNode, InstallPlan, DownloadTask 等类型 |
| `xlings.xim.repo` | `repo.cppm` | Git 仓库同步 (clone/pull，7天节流) |
| `xlings.xim.index` | `index.cppm` | 索引构建/搜索/版本匹配 (封装 libxpkg) |
| `xlings.xim.resolver` | `resolver.cppm` | DAG 依赖解析 (DFS + 循环检测 + 拓扑排序) |
| `xlings.xim.downloader` | `downloader.cppm` | 并行下载 (jthread + mutex/condvar，SHA256 校验) |
| `xlings.xim.installer` | `installer.cppm` | 安装编排 (libxpkg PackageExecutor 运行 hook) |
| `xlings.xim.commands` | `commands.cppm` | CLI 命令实现 (install/remove/search/list/info/update) |

### xvm - 版本管理器 (`core/xvm/`)

C++23 原生实现，融合为 xlings 核心模块，不再需要独立二进制。

**子模块：**

| 模块 | 文件 | 职责 |
|------|------|------|
| `xlings.xvm.types` | `types.cppm` | VData, VInfo, VersionDB, Workspace 类型 |
| `xlings.xvm.db` | `db.cppm` | VersionDB CRUD、模糊版本匹配、JSON 序列化 |
| `xlings.xvm.shim` | `shim.cppm` | Shim 分发 (argv[0] 检测 → 版本查找 → execvp) |
| `xlings.xvm.commands` | `commands.cppm` | use 命令、版本注册、shim 链接管理、头文件/库 symlink |

**Shim 流程：**
1. `main()` 检测 `argv[0]`，非 xlings/xim 则进入 shim 模式
2. 读取 `effective_workspace()` (项目 > subos > 全局)
3. 模糊匹配版本号 (e.g. "15" → "15.1.0")
4. 从 `VersionDB` 查路径，展开 `${XLINGS_HOME}`
5. 设置环境变量 (envs, PATH)
6. 处理 bindings (e.g. `g++` → `g++-15`)
7. `execvp()` 真实程序

**关键设计决策：**
- C++ 层控制流程、并行、错误恢复；Lua 层执行包特定逻辑 (hook)
- 索引通过 libxpkg `build_index()` 构建
- 下载使用 `curl` CLI (简化构建依赖)
- 包脚本由 libxpkg `PackageExecutor` 在嵌入 Lua 中执行
- 安装完成后通过 `_XVM_OPS` 操作表自动注册到 VersionDB
- 头文件使用 symlink 安装到 sysroot，版本切换时自动更新

**依赖库：**
| 库 | 用途 |
|----|------|
| `mcpplibs-xpkg` | 包索引构建、搜索、Lua hook 执行 |
| `mcpplibs-capi-lua` | Lua C API (libxpkg 的传递依赖) |
| `mcpplibs-cmdline` | CLI 参数解析 |
| `ftxui` | TUI 组件 (进度条、表格) |
| `nlohmann/json` | JSON 读写 (core/json/json.hpp) |

## CLI 命令

```
xlings <command> [options] [arguments]

Commands:
  install   <pkg[@ver]> ...   安装包（支持多个，安装后自动注册版本）
  remove    <pkg>             卸载包
  update    [pkg]             更新包索引或升级包
  search    <keyword>         搜索包
  list      [filter]          列出包
  info      <pkg>             显示包详情
  use       <pkg> [ver]       切换版本（原 xvm use，直接 C++ 实现）
  config                      查看配置
  subos     <sub-command>     管理子环境
  self      <sub-command>     自管理

Global Options:
  -y, --yes                   跳过确认提示
  -v, --verbose               详细输出
  -q, --quiet                 静默模式
  --lang <en|zh>              覆盖语言
  --mirror <GLOBAL|CN>        覆盖镜像
```

## 路径约定

| 路径 | 默认值 | 说明 |
|------|--------|------|
| `XLINGS_HOME` | `$HOME/.xlings` | xlings 安装目录 |
| `$XLINGS_HOME/data` | | 运行数据目录 |
| `$XLINGS_HOME/data/xim-pkgindex` | | 包索引仓库 (git) |
| `$XLINGS_HOME/data/xpkgs/<name>/<ver>` | | 包安装目录 |
| `$XLINGS_HOME/subos/<name>/bin` | | 可执行文件目录 |
| `$XLINGS_HOME/subos/<name>/lib` | | 库文件目录 |
| `$XLINGS_HOME/subos/<name>/.xlings.json` | | Subos 版本视图 |

所有路径基于 `XLINGS_HOME` 计算（不再有 `XLINGS_DATA`、`XLINGS_SUBOS` 环境变量）。

## 构建

```bash
# 配置 (需指定 GCC SDK 路径，若使用 import std)
xmake f -m release --sdk=/path/to/gcc-15

# 构建 C++23 单二进制
xmake build

# 运行测试 (81 个 gtest 单元测试)
xmake build xlings_tests && xmake run xlings_tests
```

## 测试

**单元测试** (`tests/unit/test_main.cpp`):
- I18nTest (7): 多语言翻译
- LogTest (4): 日志级别与文件输出
- UtilsTest (5): 字符串工具
- CmdlineTest (4): CLI 解析
- UiTest (2): UI 组件
- XimTypesTest (4): xim 类型定义
- XimIndexTest (9): 索引构建/搜索/匹配
- XimResolverTest (4): 依赖解析/拓扑排序
- XimDownloaderTest (3): 下载任务
- XimInstallerTest (4): 安装编排
- XimCommandsTest (5): 命令执行
- XvmTypesTest (3): xvm 类型构造
- XvmDbTest (6): VersionDB CRUD/模糊匹配/路径展开
- XvmJsonTest (8): JSON 序列化/反序列化往返测试
- XvmShimTest (2): Shim 程序名提取/识别
- XvmConfigTest (4): 配置文件读写/覆盖/目录结构
- XvmVDataFieldsTest (4): VData includedir/libdir 字段构造、JSON 序列化
- XvmHeaderSymlinkTest (3): 头文件 symlink 安装/覆盖/删除

**E2E 测试** (`tests/e2e/`):
- `linux_usability_test.sh` — Linux 可用性验证
- `macos_usability_test.sh` — macOS 可用性验证
- `windows_usability_test.ps1` — Windows 可用性验证

## 发布与本地测试 (release 脚本)

`tools/linux_release.sh` 会构建 C++ 单二进制，组装 Linux 可用的 xlings 目录。

```bash
./tools/linux_release.sh              # 输出到 build/xlings/ 并验证
./tools/linux_release.sh /path/out   # 输出到指定目录
```
