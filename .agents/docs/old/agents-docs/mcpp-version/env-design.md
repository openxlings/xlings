# xlings 多环境管理设计 — 深度方案文档

> 关联文档: [main.md — 第五章：多环境管理设计](main.md#五多环境管理设计)

---

## 一、问题背景与需求

### 1.1 为什么需要多环境

在工具链管理场景中，"多环境"是一个高频需求：

- **开发 vs 生产**：开发环境安装调试工具、最新版本；生产环境只保留稳定版
- **项目隔离**：项目 A 依赖 gcc@12，项目 B 依赖 gcc@15，互不干扰
- **实验性安装**：想尝试新工具但不想污染主环境
- **团队协作**：不同成员使用不同工具配置，统一由 xlings 管理
- **CI/CD 隔离**：构建环境与开发环境完全隔离，可复现

### 1.2 核心矛盾

| 矛盾 | 描述 |
|------|------|
| 隔离 vs 便利 | 隔离越彻底，使用越繁琐（激活脚本、容器启动等） |
| 轻量 vs 安全 | OS 级隔离最安全但资源消耗大；目录隔离轻量但依赖系统 |
| 灵活 vs 简单 | 支持自定义路径意味着配置复杂度上升 |

xlings 的选择：**目录隔离 + 配置驱动**，在便利性与隔离性之间取平衡点。

---

## 二、业界方案调研与对比

### 2.1 Nix / NixOS

**核心机制**: 不可变内容寻址存储（Content-Addressed Store）

```
/nix/store/
  ├── abc123-gcc-12.0/          # hash(gcc@12 + 所有依赖) → 路径唯一确定
  ├── def456-gcc-15.0/
  └── xyz789-myenv/             # 环境 = 一组包的软链集合
        └── bin/
              ├── gcc → /nix/store/def456-gcc-15.0/bin/gcc
              └── ...
```

**激活方式**: `nix-env -iA`, `nix shell`, `nix develop` 修改当前 shell 的 `PATH`

**Profile 切换**: `nix-env --switch-profile /nix/var/nix/profiles/my-profile`

**优点**:
- 真正不可变：同一哈希值永远对应同一构建产物
- 原子性：安装/回滚不会出现中间状态
- 多版本并存零冲突：路径天然唯一
- 可复现构建：`flake.nix` 锁定全部依赖

**缺点**:
- 学习曲线陡峭：Nix 语言、Flake、Derivation 概念
- 初始 store 体积大，相同库的不同版本不共享文件
- 对 Windows 支持有限（WSL 可用）
- 不适合"解压即用"场景：依赖 `/nix/store` 全局路径

**与 xlings 的差异**: xlings 不追求不可变性和内容寻址，目标是工具的便捷管理，而非构建系统级隔离。

---

### 2.2 Conda / Mamba

**核心机制**: 命名环境目录 + 激活脚本修改 shell 变量

```
~/miniconda3/
  ├── envs/
  │   ├── base/               # 默认环境
  │   │   ├── bin/
  │   │   └── lib/
  │   ├── myproject/          # 命名环境
  │   │   ├── bin/python
  │   │   └── lib/python3.11/
  │   └── data-science/
  └── condabin/conda           # conda 命令本身
```

**激活方式**: `conda activate myproject` → 修改 `PATH`、`CONDA_PREFIX`、`PS1`

**配置文件**: `environment.yml` 声明环境依赖，可复现创建

```yaml
name: myproject
channels: [conda-forge]
dependencies:
  - python=3.11
  - numpy>=1.24
```

**切换机制**:
1. `conda activate <name>` 执行 shell 函数（非独立进程）
2. 将 `envs/<name>/bin` 前置到 `PATH`
3. 设置 `CONDA_PREFIX`, `CONDA_DEFAULT_ENV` 等环境变量
4. `conda deactivate` 恢复原始 `PATH`

**优点**:
- 环境概念直观，`envs/` 目录结构清晰
- `environment.yml` 可共享和复现
- 跨平台（Linux / macOS / Windows）
- 激活后 shell 提示符自动显示当前环境名

**缺点**:
- 激活依赖 shell 钩子（`conda init` 修改 `.bashrc`），有侵入性
- 环境间无法真正隔离（系统库仍可影响）
- 大型环境（含 CUDA 等）体积庞大，切换慢
- 不适合非 Python 工具链（虽然支持，但生态以 Python 为中心）

**对 xlings 的借鉴**:
- `envs/<name>/` 目录结构 ✅ 直接采用
- 配置文件声明活跃环境 ✅ 采用（`.xlings.json` 中的 `activeEnv`）
- 激活脚本修改 `PATH` ⚠️ 部分采用（可选 shell profile，非强制）

---

### 2.3 Python venv

**核心机制**: 项目级目录隔离 + `pyvenv.cfg` 配置

```
my-project/
  └── .venv/
        ├── bin/
        │   ├── python → python3.11
        │   ├── pip
        │   └── activate          # 激活脚本
        ├── lib/python3.11/site-packages/
        └── pyvenv.cfg            # 指向基础 Python 路径
```

**激活方式**: `source .venv/bin/activate` → 临时修改当前 shell 的 `PATH`

**特点**:
- 极轻量：环境绑定到具体项目目录
- 无中央管理：每个项目自己管理 venv
- 不跨项目共享包（与 conda 相反）

**与 xlings 的差异**: venv 是项目级而非用户级，xlings 的环境是用户级命名环境（类似 conda），但比 conda 更轻量。

---

### 2.4 Rust Toolchain (rustup)

**核心机制**: 全局工具链注册 + 项目级覆盖

```
~/.rustup/
  ├── toolchains/
  │   ├── stable-x86_64-unknown-linux-gnu/
  │   └── nightly-x86_64-unknown-linux-gnu/
  └── settings.toml              # 记录 default_toolchain

# 项目级覆盖
my-project/
  └── rust-toolchain.toml        # 指定该项目使用的工具链版本
```

**切换机制**:
- 全局默认: `rustup default stable`
- 临时覆盖: `rustup run nightly cargo build`
- 项目级: 读取 `rust-toolchain.toml`（rustup 自动识别）

**shim 机制**: `~/.cargo/bin/cargo` 实际上是 shim，启动时读取 `rust-toolchain.toml` 再转发到正确工具链

**优点**:
- 项目级自动切换无需手动 activate
- 工具链版本精确到 channel + target triple
- shim 机制透明，用户无感知

**对 xlings 的借鉴**:
- xvm 已实现类似的 shim 机制 ✅
- 项目级工具版本声明（未来可通过 `xlings.toml` 实现）
- 全局默认 + 项目覆盖的两级策略

---

### 2.5 Docker / OCI 容器

**核心机制**: OS 级命名空间隔离（Namespace + cgroup）

```
容器 = 独立的 PID/Network/Mount/UTS/IPC namespace
  + rootfs（完整文件系统镜像）
  + cgroup 资源限制
```

**环境切换**: 启动/停止容器，或使用 `docker exec` 进入运行中的容器

**优点**:
- 最强隔离：进程、网络、文件系统完全独立
- 可复现：镜像内容完全确定
- 支持任意操作系统环境

**缺点**:
- 重量级：启动时间、存储开销大
- 开发体验割裂：需要挂载源码、端口映射等
- 需要 root 权限或 rootless 配置
- 不适合"解压即用"的轻量工具管理场景

**与 xlings 的差异**: xlings 不做 OS 级隔离，目标是工具安装隔离而非完整运行环境隔离。

---

### 2.6 Sandbox 方案（Firejail / Bubblewrap / macOS Sandbox）

**核心机制**: 基于 Linux seccomp + namespace 的轻量沙箱

```bash
# Firejail 示例
firejail --private=~/sandbox-dir myapp

# Bubblewrap (flatpak 使用)
bwrap --bind / / --dev /dev --proc /proc \
      --bind ~/my-env /usr/local \
      bash
```

**特点**:
- 比完整容器轻量，无需守护进程
- 仍需 Linux 内核支持（不跨平台）
- 主要用于安全隔离而非开发环境管理

**与 xlings 的关系**: 无直接借鉴，xlings 不做安全沙箱，只做目录隔离。

---

### 2.7 方案横向对比总结

| 方案 | 隔离级别 | 激活方式 | 跨平台 | 轻量性 | 复现性 | 适合 xlings 借鉴 |
|------|---------|---------|--------|--------|--------|-----------------|
| Nix | 内容寻址 | shell 变量 | 部分 | 低 | 最强 | 理念参考 |
| Conda | 目录隔离 | shell 激活 | ✅ | 中 | 强 | **结构直接借鉴** |
| Python venv | 目录隔离 | shell 激活 | ✅ | 高 | 中 | 轻量思路 |
| rustup | 工具链注册 | shim透明 | ✅ | 高 | 强 | **shim机制借鉴** |
| Docker | OS 命名空间 | 容器启动 | 部分 | 极低 | 最强 | 不适用 |
| Sandbox | OS 命名空间 | 命令包装 | Linux | 中 | 中 | 不适用 |
| **xlings env** | **目录隔离** | **配置切换** | ✅ | **极高** | **中** | — |

---

## 三、xlings 多环境核心实现原理

### 3.1 设计哲学

xlings 环境管理遵循三个原则：

1. **配置即状态**：环境切换 = 修改一个 JSON 字段，无需 shell 函数或守护进程
2. **目录即环境**：每个环境 = 一个独立目录树，删除目录即删除环境
3. **透明传播**：`XLINGS_DATA` 环境变量在主程序启动时设置，xim/xvm 自动感知，无需用户手动激活

### 3.2 实现层次

```
┌─────────────────────────────────────────────────────┐
│  用户层: xlings env use work                         │
└─────────────────────────┬───────────────────────────┘
                          │
┌─────────────────────────▼───────────────────────────┐
│  C++ 层: xlings.env 模块                            │
│    EnvManager::use_env("work")                      │
│      → 读取 .xlings.json                            │
│      → 验证环境存在                                  │
│      → 更新 activeEnv 字段                          │
│      → 原子写回 .xlings.json                        │
└─────────────────────────┬───────────────────────────┘
                          │ 下次 xlings 启动时
┌─────────────────────────▼───────────────────────────┐
│  Config 层: xlings.config 模块                      │
│    Config() 构造函数:                                │
│      → 读取 activeEnv = "work"                      │
│      → dataDir = XLINGS_HOME/envs/work/data         │
│      → set_env("XLINGS_DATA", dataDir)              │
└─────────────────────────┬───────────────────────────┘
                          │
┌─────────────────────────▼───────────────────────────┐
│  子进程层: xim / xvm                                │
│    读取 XLINGS_DATA → 操作 work 环境目录            │
└─────────────────────────────────────────────────────┘
```

### 3.3 原子写配置文件

配置文件更新必须原子化，避免写到一半时进程被中断导致配置损坏：

```cpp
// 实现原理：写临时文件 → rename（POSIX 原子操作）
int write_config_atomic_(const std::filesystem::path& configPath,
                         const nlohmann::json& newJson) {
    auto tmpPath = configPath.string() + ".tmp";
    {
        std::ofstream out(tmpPath);
        out << newJson.dump(2);     // 缩进 2 空格
    }
    // rename 在 POSIX 系统上是原子操作
    std::filesystem::rename(tmpPath, configPath);
    return 0;
}
```

Windows 上 `rename` 不保证原子性（目标文件存在时），需使用 `ReplaceFileW` API 或先删除再重命名。

### 3.4 环境目录结构

```
XLINGS_HOME/
└── envs/
    └── work/
        ├── .env.json           # 环境本地元数据（名称、创建时间、描述）
        └── data/               # 该环境的 XLINGS_DATA
            ├── bin/            # 已安装工具的可执行文件
            │   ├── gcc
            │   ├── clang
            │   └── node
            ├── lib/            # 库文件
            ├── xvm/            # xvm 版本数据库
            │   ├── config.xvm.yaml
            │   └── versions/
            └── xim/            # xim 包数据
                ├── xpkgs/      # 已安装的包
                └── xim-pkgindex/ # 包索引（可选，可 symlink 到 default）
```

`.env.json` 格式：

```json
{
  "name": "work",
  "created": "2026-02-23T10:00:00Z",
  "description": "工作环境 - GCC 15 + Node 22",
  "dataDir": ""
}
```

### 3.5 PATH 传播机制

与 conda 需要 `activate` 修改当前 shell 的 `PATH` 不同，xlings 采用**进程内传播**：

```
Shell (用户 PATH 不变)
  │
  └── xlings install gcc  (主程序启动)
        │
        ├── Config() 初始化: 确定 XLINGS_DATA = .../envs/work/data
        ├── set_env("XLINGS_DATA", ...)   ← 设置到当前进程环境
        │
        └── fork/exec: xmake xim -P $XLINGS_HOME install gcc
                          │
                          └── xim 读取 $XLINGS_DATA → 安装到 work/data/bin/
```

**用户端 PATH 配置**（可选，通过 shell profile）：

```bash
# ~/.bashrc 或 xlings-profile.sh
# 仅添加活跃环境的 bin，由 xlings 管理此行
export PATH="$(xlings env path):$PATH"
```

`xlings env path` 输出当前活跃环境的 `bin` 目录路径，可嵌入到 shell 配置中，实现 shell 级 PATH 生效。

### 3.6 环境快照与导出（设计预留）

未来可支持环境导出为声明式配置文件，类似 `conda env export`：

```yaml
# xlings-env.yaml（未来格式，当前阶段不实现）
name: work
xlings_version: "0.2.0"
packages:
  - gcc@15.0
  - node@22.0
  - cmake@3.28
xvm_versions:
  gcc: "15.0"
  node: "22.0"
```

---

## 四、`xlings.env` 模块详细设计

### 4.1 完整接口

```cpp
// core/env.cppm
export module xlings.env;

import std;
import xlings.config;
import xlings.json;
import xlings.log;
import xlings.platform;

export namespace xlings::env {

struct EnvInfo {
    std::string name;
    std::filesystem::path dataDir;
    std::string description;
    std::string created;
    bool isActive;
};

// 子命令入口（由 cmdprocessor 注册）
int run(int argc, char* argv[]);

// 列出所有已知环境
std::vector<EnvInfo> list_envs();

// 创建新环境
// customData: 为空则使用 $XLINGS_HOME/envs/<name>/data
int create_env(const std::string& name,
               const std::string& description = "",
               const std::filesystem::path& customData = {});

// 切换活跃环境（更新 .xlings.json 的 activeEnv）
int use_env(const std::string& name);

// 删除环境（不允许删除 default 和当前活跃环境）
int remove_env(const std::string& name, bool force = false);

// 获取指定环境的详情
std::optional<EnvInfo> get_env(const std::string& name);

// 输出当前活跃环境的 data/bin 路径（供 shell PATH 使用）
int print_active_path();

} // namespace xlings::env
```

### 4.2 子命令分发逻辑

```cpp
// run() 内部实现结构（伪代码）
int run(int argc, char* argv[]) {
    // argv: ["xlings", "env", <subcommand>, ...]
    if (argc < 3) {
        // 无子命令: 显示当前活跃环境 + 简要列表
        return cmd_status_();
    }

    std::string_view sub = argv[2];
    if (sub == "list")   return cmd_list_();
    if (sub == "new")    return cmd_new_(argc, argv);
    if (sub == "use")    return cmd_use_(argc, argv);
    if (sub == "remove") return cmd_remove_(argc, argv);
    if (sub == "info")   return cmd_info_(argc, argv);
    if (sub == "path")   return print_active_path();
    if (sub == "help")   return cmd_help_();

    log::error("Unknown env subcommand: {}", sub);
    return 1;
}
```

### 4.3 各子命令行为规范

#### `xlings env list`

```
输出示例:
  * default   ~/.xlings/envs/default/data   (3 packages)
    work      ~/.xlings/envs/work/data       (12 packages)
    test      /custom/test-data              (0 packages)

  * = active
```

- 从 `.xlings.json` 的 `envs` 字典读取所有环境
- 标记 `activeEnv` 对应的环境
- 统计 `data/bin/` 下的文件数作为"包数量"近似

#### `xlings env new <name> [--data <path>] [--desc <desc>]`

```
流程:
  1. 验证 name 合法（字母数字下划线，不与已有环境同名）
  2. 确定 dataDir（参数 > 默认 $XLINGS_HOME/envs/<name>/data）
  3. 创建目录: dataDir/bin, dataDir/lib, dataDir/xvm, dataDir/xim
  4. 写入 XLINGS_HOME/envs/<name>/.env.json
  5. 更新 .xlings.json: envs.<name> = { "data": customData || "" }
  6. 原子写回 .xlings.json
  7. 提示: [xlings:env] created 'work' → /path/to/work/data
           use 'xlings env use work' to activate
```

#### `xlings env use <name>`

```
流程:
  1. 读取 .xlings.json
  2. 验证 name 在 envs 字典中存在
  3. 验证 dataDir 存在（或提示初始化）
  4. 更新 activeEnv = name
  5. 原子写回 .xlings.json
  6. 输出: [xlings:env] switched to 'work'
           XLINGS_DATA: /path/to/work/data
           Note: restart shell or re-source profile to update PATH
```

#### `xlings env remove <name> [--force]`

```
流程:
  1. 拒绝删除 "default" 环境
  2. 拒绝删除当前 activeEnv（除非 --force）
  3. 若 --force 且为当前活跃: 先切回 default
  4. 从 .xlings.json 的 envs 字典移除 name
  5. 原子写回 .xlings.json
  6. 询问是否同时删除 data 目录（默认 No）
  7. 若确认: fs::remove_all(dataDir)
```

#### `xlings env info [name]`

```
输出示例（name 省略则显示活跃环境）:
  Environment: work
  Status:      active
  Data dir:    ~/.xlings/envs/work/data
  Created:     2026-02-23
  Description: 工作环境 - GCC 15 + Node 22

  Installed tools (data/bin/):
    gcc, g++, clang, cmake, node, npm (6 files)

  XVM versions (data/xvm/):
    gcc: 15.0 (active), 12.0
    node: 22.0 (active)
```

---

## 五、与 xim / xvm 的协作

### 5.1 xim 感知当前环境

xim（Lua）通过 `XLINGS_DATA` 环境变量感知当前活跃环境，C++ 主程序在启动时注入：

```
main() 启动
  → Config() 初始化: 解析 activeEnv → 确定 dataDir
  → platform::set_env_variable("XLINGS_DATA", dataDir)
  → cmdprocessor::xim_exec(...)
      → "xmake xim -P $XLINGS_HOME -- install gcc"
          → xim 读取 $XLINGS_DATA → 安装到 work/data/
```

xim 内部无需修改任何代码，`XLINGS_DATA` 即为其工作目录的来源。

### 5.2 xvm 感知当前环境

xvm（Rust）同样通过 `XLINGS_DATA` 确定数据目录（见 `core/xvm/xvmlib/config.rs`）：

```rust
// xvmlib/config.rs 路径解析逻辑（现有代码）
fn resolve_data_dir() -> PathBuf {
    if let Ok(d) = env::var("XLINGS_DATA") { return PathBuf::from(d); }
    if let Ok(h) = env::var("XLINGS_HOME") { return PathBuf::from(h).join("data"); }
    home_dir().unwrap_or_default().join(".xlings").join("data")
}
```

切换环境后，xvm 自动使用新环境的 `data/xvm/` 目录，版本数据库完全隔离。

### 5.3 环境隔离范围

| 内容 | 隔离 | 共享 |
|------|------|------|
| 已安装工具 (`data/bin/`) | ✅ 每环境独立 | — |
| 库文件 (`data/lib/`) | ✅ 每环境独立 | — |
| xvm 版本数据库 | ✅ 每环境独立 | — |
| 包索引 (`xim-pkgindex/`) | ⚠️ 可选共享 | 默认 symlink 到 default |
| xlings 主程序本身 | — | ✅ 所有环境共享 |
| xim Lua 脚本 | — | ✅ 所有环境共享 |
| `.xlings.json` 配置 | — | ✅ 全局单一配置 |

---

## 六、xlings vs 业界方案：定位对比

```
隔离强度 ↑
    │
    │  Docker ●────────────────── OS 级，完整文件系统隔离
    │
    │  Nix    ●────────────────── 内容寻址，不可变，最强复现性
    │
    │  Conda  ●────────────────── 目录隔离 + shell 激活，跨平台
    │
    │  rustup ●────────────────── 工具链注册 + shim 透明切换
    │
    │  venv   ●────────────────── 项目级目录隔离，极轻量
    │
    │ xlings  ●────────────────── 目录隔离 + 配置切换，解压即用
    │
    └──────────────────────────────────────────────────► 轻量性 →
```

**xlings 的独特定位**：

1. **解压即用**：无需任何激活命令，`XLINGS_HOME` 设置后所有操作自动路由到正确环境
2. **配置驱动切换**：切换环境 = 写一个 JSON 字段，无 shell 函数依赖
3. **多平台一致**：Linux / macOS / Windows 行为完全统一，无 OS 级特性依赖
4. **增量隔离**：不强制完全隔离，包索引等可在环境间共享，节省磁盘
5. **零侵入性**：不修改系统 PATH、不安装 shell 钩子（shell profile 是可选的）

---

## 七、边界情况与异常处理

| 情况 | 处理策略 |
|------|---------|
| 切换到不存在的环境 | 报错：`env 'foo' not found, use 'xlings env new foo'` |
| 删除当前活跃环境 | 拒绝（提示先切换），`--force` 时自动切回 default |
| `.xlings.json` 损坏 | 报错并提示 `xlings self init` 重建 |
| `dataDir` 目录不存在 | 自动创建（init 时），或提示 `xlings env init <name>` |
| 环境名含特殊字符 | 拒绝：只允许 `[a-zA-Z0-9_-]`，最长 64 字符 |
| 并发写 `.xlings.json` | 原子 rename 保证写完整；读写竞争概率极低（单用户工具） |
| `XLINGS_DATA` 环境变量已设置 | 优先级最高，完全绕过环境系统（用于 CI/CD 场景） |

---

## 八、实现路线

| 阶段 | 内容 | 优先级 |
|------|------|--------|
| P0 | `xlings.env` 模块骨架 + `list` / `new` / `use` 三个核心子命令 | 必须 |
| P0 | Config 层集成 `activeEnv` 解析，更新路径优先级逻辑 | 必须 |
| P1 | `remove` / `info` 子命令 | 高 |
| P1 | `xlings env path` 输出（用于 shell PATH 集成） | 高 |
| P2 | 环境名校验、`--desc` 描述支持 | 中 |
| P2 | 包索引 symlink 共享机制 | 中 |
| P3 | 环境导出/导入（`xlings-env.yaml`） | 低（未来版本） |
| P3 | 环境快照与回滚 | 低（未来版本） |

---

## 参考资料

- [Nix Package Manager Manual](https://nixos.org/manual/nix/stable/)
- [Conda User Guide — Managing Environments](https://docs.conda.io/projects/conda/en/latest/user-guide/tasks/manage-environments.html)
- [Python venv — venv: Creation of virtual environments](https://docs.python.org/3/library/venv.html)
- [rustup — The Rust toolchain installer](https://rust-lang.github.io/rustup/)
- [Bubblewrap — Unprivileged sandboxing tool](https://github.com/containers/bubblewrap)
- [mcpp-style-ref](https://github.com/mcpp-community/mcpp-style-ref)
