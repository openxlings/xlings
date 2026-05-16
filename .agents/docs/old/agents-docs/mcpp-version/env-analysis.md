# xlings 现有架构深度分析与增强方案整合

> 关联文档:
> - [main.md — 顶层设计方案](main.md)
> - [env-design.md — 多环境管理与业界对比](env-design.md)
> - [env-store-design.md — Nix 精髓移植方案](env-store-design.md)

---

## 一、重大发现：xlings 现有架构已实现 Nix 核心思想

通过对 `/home/xlings/.xlings_data/` 的实际数据分析，发现 **xlings 当前架构已经在实践层面独立实现了与 Nix 高度相似的设计**，这并非偶然，而是解决同一类问题的自然收敛。

### 1.1 实际数据结构

```
/home/xlings/.xlings_data/
├── bin/                          ← 616 个文件，全部是 xvm-shim 副本
│   ├── cmake          (621032B)  ← MD5: c65708465a9fc...  (与 xvm-shim 完全相同)
│   ├── code           (621032B)  ← MD5: c65708465a9fc...  (与 xvm-shim 完全相同)
│   ├── dadk           (621032B)  ← MD5: c65708465a9fc...  (与 xvm-shim 完全相同)
│   ├── d2x            (621032B)  ← MD5: c65708465a9fc...  (与 xvm-shim 完全相同)
│   └── xvm-shim       (621032B)  ← 原始 shim 二进制
│
├── xim/
│   └── xpkgs/                   ← 160 个包条目，已有多版本并存
│       ├── code/
│       │   ├── 1.93.1/           ← VSCode 5 个版本并存
│       │   ├── 1.100.1/
│       │   ├── 1.106.1/
│       │   ├── 1.108.0/
│       │   └── 1.108.1/
│       ├── d2x/
│       │   ├── 0.0.4/            ← d2x 4 个版本
│       │   ├── 0.1.0/
│       │   ├── 0.1.1/
│       │   └── 0.1.2/
│       ├── dadk/
│       │   ├── 0.2.0/            ← dadk 3 个版本
│       │   ├── 0.3.0/
│       │   └── 0.4.0/
│       ├── cmake/4.0.2/          ← cmake (ELF 真实二进制)
│       ├── fromsource-x-*/       ← 56 个从源码构建的库
│       ├── scode-x-*/            ← 22 个源码包（源码树）
│       └── local-x-*/            ← 17 个本地脚本包
│
└── xvm/
    └── .workspace.xvm.yaml       ← 版本激活清单（全局 workspace）
```

### 1.2 xvm workspace 版本清单（当前活跃状态）

```yaml
versions:
  cmake: 4.0.2
  dadk: 0.4.0           # dadk 装了 3 个版本，激活的是 0.4.0
  node: 22.17.1
  python: 3.10.16
  d2x: 0.1.2            # d2x 装了 4 个版本，激活的是 0.1.2
  npm: 11.0.0
  pnpm: 9.15.0
  qemu-system-x86_64: 8.2.2
  # ... 共 40+ 条目
```

---

## 二、三层架构解剖：与 Nix 的精确映射

### 2.1 架构对比图

```
Nix 架构                            xlings 现有架构
─────────────────────────────────   ─────────────────────────────────
/nix/store/                         xpkgs/
  abc123-gcc-15.0/                    gcc/15.0/          ← "版本寻址存储"
  def456-gcc-12.0/                    gcc/12.0/
  xyz789-cmake-3.28/                  cmake/4.0.2/

~/.nix-profile/bin/                 data/bin/
  gcc  →  /nix/store/abc123.../gcc    cmake  =  xvm-shim ← "shim 激活层"
  cmake → /nix/store/xyz789.../cmake  gcc    =  xvm-shim

~/.nix-profile/manifest.nix         xvm/.workspace.xvm.yaml
  { gcc = "15.0"; cmake = "3.28" }    cmake: 4.0.2      ← "Profile 清单"
                                       gcc: 15.0
```

### 2.2 xvm-shim 运行时行为（关键发现）

`data/bin/` 下的 616 个文件**完全相同**（MD5 一致），都是 `xvm-shim` 的副本。

每次执行 `cmake` 时，实际流程：

```
用户执行: cmake --version
    │
    └── data/bin/cmake  (= xvm-shim)
          │
          ├── 1. 读取自身文件名 → "cmake"
          ├── 2. 查找 xvm workspace: .workspace.xvm.yaml
          │         cmake: 4.0.2
          ├── 3. 解析真实路径: xpkgs/cmake/4.0.2/bin/cmake
          └── 4. exec(真实路径, argv)   ← 透明转发
```

这与 Nix profile 的 symlink 解析在逻辑上完全等价，只是实现机制不同：
- Nix：OS 级符号链接（内核解析，零开销）
- xlings：用户态 shim（一次额外进程替换，微小开销）

### 2.3 xpkgs 包类型体系

通过对 160 个包条目的命名规范分析，xim 已形成了完整的包类型体系：

| 类型前缀 | 数量 | 说明 | 示例 |
|---------|------|------|------|
| `<name>/<version>/` | ~50 | 标准二进制包 | `cmake/4.0.2/`, `d2x/0.1.2/` |
| `fromsource-x-<name>/<ver>/` | 56 | 从源码构建，包含完整 `bin/lib/include/` | `fromsource-x-gcc/`, `fromsource-x-glib/` |
| `scode-x-<name>/<ver>/` | 22 | 源码树（仅下载，不构建） | `scode-x-linux/5.11.1/` |
| `local-x-<name>/` | 17 | 本地脚本包（无版本目录） | `local-x-git-autosync/` |
| `config@<name>/` | ~5 | 配置类包（镜像、环境设置） | `config@rust-crates-mirror/` |
| `<ns>@<name>/<ver>/` | ~10 | 命名空间包 | `dragonos@dragonos-dev/0.2.0/` |

---

## 三、现有架构的问题与缺口

### 3.1 已实现的部分（超出预期的完整度）

| 功能 | 状态 | 实现位置 |
|------|------|---------|
| 版本寻址存储 | ✅ 已有 | `xpkgs/<name>/<version>/` |
| 多版本并存 | ✅ 已有 | `xpkgs/code/` 有 5 个版本 |
| 运行时版本透明切换 | ✅ 已有 | xvm-shim + `.workspace.xvm.yaml` |
| 版本激活清单 | ✅ 已有 | `.workspace.xvm.yaml` 的 `versions:` |
| 跨平台 shim | ✅ 已有 | Rust 实现的 xvm-shim |
| 多种包类型 | ✅ 已有 | binary/fromsource/scode/local/config |

### 3.2 缺失的部分（需要新增）

| 功能 | 缺口描述 | 影响 |
|------|---------|------|
| **多环境隔离** | 只有一个全局 `data/` 目录，无法创建互相隔离的环境 | 无法实现"工作环境 vs 实验环境"隔离 |
| **Profile 世代 / 回滚** | 没有历史快照，`use` 切换后无法撤销 | 误操作无法恢复 |
| **Store 去重跨环境共享** | `xpkgs/` 在单一 `data/` 下，多环境后将各自维护副本 | 多环境会导致磁盘浪费 |
| **GC 垃圾回收** | 未使用的旧版本包永久占用磁盘 | `xpkgs/code/` 的 5 个版本都占空间 |
| **环境声明式导出** | 没有"这个环境装了什么"的可分享清单 | 无法复现环境 |

### 3.3 bin/ 目录的问题：shim 副本 vs 符号链接

当前 `data/bin/` 下每个工具都是 xvm-shim 的**物理副本**（不是符号链接）：

```
data/bin/cmake  = 621032 字节（xvm-shim 副本）
data/bin/code   = 621032 字节（xvm-shim 副本）
data/bin/dadk   = 621032 字节（xvm-shim 副本）
... 616 个副本 × 621032 字节 ≈ 363MB 仅用于 shim
```

**问题**：
1. 磁盘浪费（363MB 存放完全相同的文件）
2. xvm-shim 升级时，需要替换所有 616 个副本
3. 未来多环境后，每个环境都要维护各自的 616 个副本

**改进方向**：
- Linux/macOS：改为硬链接（同 inode，节省空间，升级时仍需批量更新）
- 或改为两层结构：一个真正的 xvm-shim 二进制 + 每个工具一个符号链接指向它
- Windows：维持副本方式（符号链接权限问题），但升级时只需替换原始 shim

---

## 四、修订后的整体架构设计

结合现有实现与缺口分析，提出以下修订架构：

### 4.1 目录结构（增量改进，保持向后兼容）

```
XLINGS_HOME/
├── .xlings.json                    # 全局配置 (activeEnv, mirror 等)
│
├── store/                          # ★ 全局共享 Store（从 xpkgs/ 升级而来）
│   ├── cmake/4.0.2/                # 真实二进制，不可变
│   ├── code/1.108.1/
│   ├── d2x/0.1.2/
│   ├── fromsource-x-gcc/15.0/
│   └── ...                         # 所有环境共享，不重复存储
│
├── envs/
│   ├── default/                    # 默认环境（对应现有 data/）
│   │   ├── .profile.json           # 版本激活清单（从 .workspace.xvm.yaml 迁移）
│   │   ├── generations/            # ★ 世代历史
│   │   │   ├── 001.json
│   │   │   └── 042.json            # current
│   │   └── bin/                    # shim 或符号链接（替代现有 data/bin/）
│   │       ├── cmake ──→ (shim/link → store/cmake/4.0.2/bin/cmake)
│   │       └── dadk  ──→ (shim/link → store/dadk/0.4.0/bin/dadk)
│   └── work/                       # 新隔离环境
│       ├── .profile.json           # 独立版本清单
│       ├── generations/
│       └── bin/
│           └── dadk ──→ (shim/link → store/dadk/0.2.0/bin/dadk)  ← 不同版本!
│
├── shims/
│   └── xvm-shim                    # ★ 唯一的 shim 二进制（所有 bin/ 共享此一个）
│
├── xim/                            # Lua 包管理器（不变）
├── config/                         # i18n 等（不变）
└── bin/
    ├── xlings                      # 入口脚本
    └── .xlings.real                # C++23 主程序
```

### 4.2 xpkgs → store 迁移策略

现有 `data/xim/xpkgs/` 与新设计的 `store/` 在结构上**几乎完全相同**，迁移成本极低：

```
现在:  $XLINGS_DATA/xim/xpkgs/<name>/<version>/
目标:  $XLINGS_HOME/store/<name>/<version>/

迁移方案:
  1. 将 xpkgs/ 整体移动（或 rename）到 XLINGS_HOME/store/
  2. 在 XLINGS_DATA/xim/ 建立 xpkgs → ../../store 的符号链接（向后兼容）
  3. xim Lua 代码无需修改，透过符号链接仍可正常工作
```

### 4.3 .workspace.xvm.yaml → .profile.json 迁移

```yaml
# 现在 (xvm/.workspace.xvm.yaml)
versions:
  cmake: 4.0.2
  dadk: 0.4.0

# 新格式 (envs/default/.profile.json)
{
  "current_generation": 42,
  "packages": {
    "cmake": "4.0.2",
    "dadk": "0.4.0"
  }
}
```

xvm 仍可读取旧格式（向后兼容），新的 C++ 层同时维护新格式。

### 4.4 bin/ 优化：shim 引用方式

```
现在:    data/bin/cmake = xvm-shim 的 621032 字节副本（× 616 个）
                          总计 ≈ 363MB

目标 (Linux/macOS):
         shims/xvm-shim = 唯一真实文件（621032 字节）
         envs/default/bin/cmake → ../../shims/xvm-shim  (符号链接, 几字节)
         总计 ≈ 621KB + 几字节 × N

目标 (Windows):
         shims/xvm-shim.exe = 唯一真实文件
         envs/default/bin/cmake.exe = 硬链接（同 inode，节省 IO 但占 inode）
         或: cmake.cmd = 单行批处理脚本（最简单，无权限问题）
```

---

## 五、shim 机制演进：从版本路由到环境感知

### 5.1 现有 xvm-shim 工作原理

```rust
// core/xvm/shim/main.rs (当前实现)
fn main() {
    let exe_name = /* argv[0] 的文件名 */;
    let version  = xvm::lookup_version(exe_name);         // 查 .workspace.xvm.yaml
    let real_bin = xvm::resolve_path(exe_name, version);  // 拼出 xpkgs/name/ver/bin/name
    exec(real_bin, args);
}
```

### 5.2 增强版 shim：增加环境感知

```rust
// 增强版 shim（伪代码）
fn main() {
    let exe_name = get_exe_name();

    // ★ 新增：感知当前活跃环境
    let xlings_home = resolve_xlings_home();   // 从 XLINGS_HOME 或可执行文件位置推导
    let active_env  = read_active_env(xlings_home);  // 读 .xlings.json 的 activeEnv

    // 从对应环境的 profile 查版本
    let profile  = load_profile(xlings_home, active_env);  // envs/<env>/.profile.json
    let version  = profile.packages[exe_name];

    // 路由到 store
    let real_bin = xlings_home / "store" / exe_name / version / "bin" / exe_name;
    exec(real_bin, args);
}
```

这样 shim 就变成了**环境感知的路由器**：

```
同一个 xvm-shim 副本:
  在 default 环境:  cmake → store/cmake/4.0.2/bin/cmake
  在 work 环境:     cmake → store/cmake/3.28.0/bin/cmake  ← 自动路由到不同版本
```

---

## 六、完整数据流：一次 `xlings install dadk@0.4.0` 的全生命周期

```
用户: $ xlings install dadk@0.4.0
         │
         ▼ [C++ 主程序]
┌─────────────────────────────────────────────────┐
│ 1. Config() 初始化                               │
│    activeEnv = "default"                        │
│    XLINGS_HOME = ~/.xlings                      │
│    XLINGS_DATA = ~/.xlings/envs/default         │
│    set_env("XLINGS_DATA", ...)                  │
└──────────────────────┬──────────────────────────┘
                       │
                       ▼ [xim Lua]
┌─────────────────────────────────────────────────┐
│ 2. xim 下载并安装到 store                        │
│    下载到: cache/downloads/dadk-0.4.0.tar.gz    │
│    解压到: store/dadk/0.4.0.tmp/                │
│    rename: store/dadk/0.4.0.tmp → dadk/0.4.0   │  ← 原子
│    (旧版本 dadk/0.2.0, dadk/0.3.0 保留不动)     │
└──────────────────────┬──────────────────────────┘
                       │ 安装完成，通知主程序
                       ▼ [C++ 主程序 - Profile 层]
┌─────────────────────────────────────────────────┐
│ 3. 更新 Profile 世代                             │
│    读取当前世代 N (generations/042.json)         │
│    创建新世代 N+1:                               │
│      { packages: { ..., dadk: "0.4.0" } }       │
│    写入 generations/043.json                    │
│    原子更新 .profile.json { current: 43, ... }  │
└──────────────────────┬──────────────────────────┘
                       │
                       ▼ [C++ 主程序 - 视图层]
┌─────────────────────────────────────────────────┐
│ 4. 重建 bin/ 视图                                │
│    envs/default/bin/dadk:                       │
│      Linux: symlink → ../../store/dadk/0.4.0/  │
│      Windows: shim → shims/xvm-shim.exe         │
│    (配套 .shim 文件记录真实路径)                 │
└──────────────────────┬──────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────┐
│ 5. 输出                                          │
│    [xlings] installed dadk@0.4.0                │
│    [xlings] env: default, gen: 43              │
│    旧版本 dadk@0.3.0 仍在 store，未被删除        │
└─────────────────────────────────────────────────┘
```

---

## 七、现有架构与目标架构差距摘要

| 维度 | 现状 | 目标 | 迁移难度 |
|------|------|------|---------|
| 包存储结构 | `xpkgs/<name>/<version>/` | `store/<name>/<version>/` | ⭐ 极低（rename + symlink 兼容） |
| 多版本并存 | ✅ 已支持 | ✅ 保持 | 无需改动 |
| 版本激活 | `.workspace.xvm.yaml` | `envs/<env>/.profile.json` | ⭐⭐ 低（格式转换） |
| shim 机制 | ✅ 已有（物理副本） | 符号链接 + 单一 shim | ⭐⭐ 低 |
| 多环境隔离 | ❌ 无 | `envs/<name>/` 目录 | ⭐⭐⭐ 中 |
| 跨环境 store 共享 | ❌ 无（单环境） | `store/` 全局共享 | ⭐⭐⭐ 中 |
| Profile 世代/回滚 | ❌ 无 | `generations/*.json` | ⭐⭐ 低 |
| GC | ❌ 无 | `xlings store gc` | ⭐⭐ 低 |
| 声明式环境导出 | ❌ 无 | `.profile.json` 可分享 | ⭐ 极低（已有数据结构） |

---

## 八、结论与建议

### 8.1 核心结论

**xlings 当前的 xpkgs + xvm-shim + workspace.yaml 三层架构，已经独立实现了 Nix 设计哲学的核心精髓**，并且以更轻量、更跨平台的方式实现了它。这是一个非常优秀的基础。

Nix 三个核心思想在 xlings 中的映射：

```
Nix: /nix/store/<hash>-name/          ←→  xlings: xpkgs/<name>/<version>/
Nix: profile symlink → store entry    ←→  xlings: bin/<name> = xvm-shim (runtime lookup)
Nix: profile manifest                 ←→  xlings: .workspace.xvm.yaml versions:
```

### 8.2 迁移路径建议

基于现有架构的连续性，推荐以下渐进式演进路径：

**第一步（低风险，立即可做）**：
- 将 `data/bin/` 下的 shim 副本改为符号链接（Linux/macOS），节省 363MB
- 将 `data/xim/xpkgs/` rename/link 为 `store/`，为多环境做准备

**第二步（中等工作量，v0.2.0 核心目标）**：
- 实现 `xlings.env` 模块：`new/use/list/remove` 命令
- Config 层支持 `activeEnv` 解析，动态路由 `XLINGS_DATA`
- 将 `.workspace.xvm.yaml` 升级为带世代历史的 `.profile.json`

**第三步（完善阶段，v0.2.x）**：
- `xlings store gc` GC 命令
- `xlings env rollback` 回滚命令
- 环境声明式导出/导入

### 8.3 不需要做的事

以下 Nix 特性在 xlings 场景下**无需实现**：

- ❌ 内容哈希寻址（`name@version` 已足够，真正的内容寻址需要构建系统支持）
- ❌ 依赖闭包追踪（xim 包脚本已处理依赖，无需在 store 层重复）
- ❌ 沙箱构建（xlings 管理预编译包，不负责从源码构建）
- ❌ 守护进程（所有操作都是文件系统操作，无需后台进程）
