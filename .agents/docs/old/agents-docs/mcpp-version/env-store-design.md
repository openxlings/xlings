# xlings 环境隔离设计（修订版）

> 关联文档:
> - [env-design.md — 多环境管理与业界对比](env-design.md)
> - [env-analysis.md — 现有架构深度分析](env-analysis.md)
> - [main.md — 顶层设计方案](main.md)

---

## 一、核心结论：xpkgs 就是 Store，无需另建

上一版设计提出了单独的 `store/` 目录层，经过对实际数据的分析，这是不必要的。

**xpkgs 已经就是 Store：**

```
xim/xpkgs/
  gcc/15.0/       ← 安装后不修改，版本寻址，多版本并存
  gcc/12.0/
  cmake/4.0.2/
  d2x/0.1.2/
  d2x/0.1.0/      ← 旧版本保留，零冲突
```

结构完全等价于 Nix store，用 `name@version` 替代哈希寻址，已在生产环境运行：

```
Nix:    /nix/store/<hash>-gcc-15.0/
xlings: xim/xpkgs/gcc/15.0/          ← 功能完全等价
```

**不需要的事：**
- ❌ 新建 `store/` 目录
- ❌ 新增 `xlings.store` C++ 模块
- ❌ 改变 xim 的安装目标路径
- ❌ 改变 `bin/` 的物理副本方式（暂时保留）

---

## 二、现有三层架构（已就位，无需重建）

```
层级              现有实现                         职责
─────────────────────────────────────────────────────────────────
Store 层    xim/xpkgs/<name>/<version>/          包的真实文件存储
                                                 多版本并存，不可变

Profile 层  xvm/.workspace.xvm.yaml              激活版本清单
              versions:                           name → 当前激活版本
                cmake: 4.0.2
                dadk: 0.4.0

激活层      data/bin/<name> = xvm-shim 副本       运行时路由
                                                 读 workspace.yaml → exec 真实二进制
```

这三层已经完整工作。**多环境隔离只需要让每个环境拥有独立的 Profile 层和激活层，同时共享 Store 层（xpkgs）。**

---

## 三、多环境目录设计

### 3.1 核心思路

```
共享（全局）:  xim/xpkgs/       ← 所有环境共用，gcc@15.0 只有一份真实文件
隔离（每环境）: envs/<name>/bin/             ← 各环境的工具激活视图
               envs/<name>/xvm/workspace    ← 各环境的活跃版本清单
```

### 3.2 完整目录结构

```
XLINGS_HOME/
├── .xlings.json                     # 全局配置，含 activeEnv
│
├── xim/                             # xim 包管理器（自包含，不变）
│   ├── xpkgs/                       # ★ 全局共享 Store（已有，保持不变）
│   │   ├── gcc/
│   │   │   ├── 12.0/
│   │   │   └── 15.0/
│   │   ├── cmake/4.0.2/
│   │   └── d2x/0.1.2/
│   └── xim-pkgindex/                # 包索引
│
├── envs/                            # ★ 多环境根目录（新增）
│   ├── default/                     # 默认环境（迁移自现有 data/）
│   │   ├── .profile.json            # 世代清单（见 3.3）
│   │   ├── generations/             # 历史世代（见 3.4）
│   │   │   ├── 001.json
│   │   │   └── 042.json
│   │   ├── bin/                     # 此环境的激活工具（xvm-shim 物理副本）
│   │   │   ├── xvm-shim
│   │   │   ├── cmake    (= xvm-shim 副本)
│   │   │   └── dadk     (= xvm-shim 副本)
│   │   └── xvm/
│   │       └── .workspace.xvm.yaml  # 此环境的激活版本清单
│   │             versions:
│   │               cmake: 4.0.2
│   │               dadk: 0.4.0
│   │
│   └── work/                        # 自定义环境
│       ├── .profile.json
│       ├── generations/
│       ├── bin/
│       │   ├── xvm-shim
│       │   └── dadk     (= xvm-shim 副本，但 workspace 里版本不同)
│       └── xvm/
│           └── .workspace.xvm.yaml
│                 versions:
│                   dadk: 0.2.0      ← 和 default 用不同版本，xpkgs 里都有
│
├── bin/
│   ├── xlings
│   └── .xlings.real
└── config/
```

### 3.3 `.profile.json` — 替换 `.workspace.xvm.yaml` 的元数据层

`.workspace.xvm.yaml` 记录"哪些版本当前激活"，但没有历史。`.profile.json` 在其之上加一层世代元数据：

```json
// envs/default/.profile.json
{
  "current_generation": 42,
  "packages": {
    "cmake": "4.0.2",
    "dadk":  "0.4.0",
    "node":  "22.17.1"
  }
}
```

`xvm/.workspace.xvm.yaml` 仍然保留，作为 xvm 读取的格式（向后兼容）。`.profile.json` 由 C++ 主程序维护，两者保持同步：

```
安装/删除包后:
  1. C++ 更新 .profile.json（写新世代）
  2. C++ 同步更新 xvm/.workspace.xvm.yaml（xvm 继续正常读取）
```

### 3.4 世代历史（Generations）

```json
// envs/default/generations/042.json
{
  "generation": 42,
  "created": "2026-02-23T14:30:00Z",
  "reason": "install dadk@0.4.0",
  "packages": {
    "cmake": "4.0.2",
    "dadk":  "0.4.0",
    "node":  "22.17.1"
  }
}
```

每次安装/删除都追加一个世代文件，不修改历史。

---

## 四、XLINGS_DATA 的重新定义

### 4.1 当前

```
XLINGS_DATA = ~/.xlings_data    (单一全局数据目录)
  bin/                           ← 激活的工具
  xim/xpkgs/                     ← 包真实文件
  xvm/.workspace.xvm.yaml        ← 激活版本清单
```

### 4.2 多环境后

```
XLINGS_HOME = ~/.xlings

XLINGS_DATA（per-env）= $XLINGS_HOME/envs/<activeEnv>
                          bin/
                          xvm/.workspace.xvm.yaml

xpkgs 路径（全局）= $XLINGS_HOME/xim/xpkgs    ← 所有环境共享
```

**关键变化**：`XLINGS_DATA` 从"全局数据根"收窄为"当前环境的激活视图根"，xpkgs 从 `$XLINGS_DATA/xim/xpkgs/` 提升为 `$XLINGS_HOME/xim/xpkgs/`（全局共享）。

### 4.3 xim 的最小改动

xim 当前读取 `$XLINGS_DATA/xim/xpkgs/` 作为包安装目标。多环境后需要将 xpkgs 指向全局路径，有两种方案：

**方案 A（推荐）：新增环境变量 `XLINGS_PKGDIR`**

```bash
# C++ 主程序启动时设置
XLINGS_PKGDIR = $XLINGS_HOME/xim/xpkgs    # 全局 xpkgs
XLINGS_DATA   = $XLINGS_HOME/envs/default  # 当前环境（bin/ + xvm/）
```

xim 优先读取 `XLINGS_PKGDIR`（若有），否则 fallback 到 `$XLINGS_DATA/xim/xpkgs/`（向后兼容）。xim Lua 代码改动极少（约 2 行）。

**方案 B：兼容模式 symlink**

```
envs/default/xim/xpkgs → ../../xim/xpkgs  (符号链接，Linux/macOS)
```

xim 代码零改动，通过 symlink 将旧路径透明重定向到全局 xpkgs。

---

## 五、安装流程（更新后）

```
xlings install dadk@0.4.0
  │
  ├─ [Config] activeEnv = "default"
  │   XLINGS_HOME    = ~/.xlings
  │   XLINGS_DATA    = ~/.xlings/envs/default
  │   XLINGS_PKGDIR  = ~/.xlings/xim/xpkgs
  │
  ├─ [xim] 检查 xpkgs/dadk/0.4.0/ 是否已存在
  │    ├─ 存在 → 跳过下载（跨环境共享，装过一次所有环境都能用）
  │    └─ 不存在 → 下载解压到 xpkgs/dadk/0.4.0.tmp/ → rename（原子）
  │
  ├─ [C++] xim 返回后，更新 Profile
  │    读取当前世代 N (generations/042.json)
  │    写新世代 043.json { ..., dadk: "0.4.0" }
  │    原子更新 .profile.json { current_generation: 43 }
  │    同步写 xvm/.workspace.xvm.yaml { versions: { dadk: "0.4.0" } }
  │
  ├─ [C++] 更新 bin/ 视图
  │    envs/default/bin/dadk = xvm-shim 副本（已是物理副本方式）
  │    （如已存在则跳过，shim 本身无状态）
  │
  └─ 输出: [xlings] installed dadk@0.4.0 (env: default, gen: 43)
```

---

## 六、回滚流程

```
xlings env rollback [--to <N>]
  │
  ├─ 列出 generations/*.json，按编号排序，打印历史
  │    * 43  2026-02-23  install dadk@0.4.0    (current)
  │      42  2026-02-22  install cmake@4.0.2
  │      ...
  │
  ├─ 加载目标世代 JSON（packages 字典）
  │
  ├─ 同步写 xvm/.workspace.xvm.yaml（回滚后的版本清单）
  │
  ├─ 更新 .profile.json { current_generation: N }
  │    （bin/ 下的 xvm-shim 副本无需变化，shim 读 workspace 动态路由）
  │
  └─ 输出: [xlings] rolled back to gen 42
```

**回滚为什么简单**：`bin/` 里的 xvm-shim 副本是无状态的，它在运行时读 `.workspace.xvm.yaml` 决定路由目标。回滚只需改 yaml 文件，无需动 `bin/` 里的任何文件。

---

## 七、GC — 清理 xpkgs 中的无用版本

xpkgs 全局共享后，GC 需要扫描所有环境的引用：

```
xlings store gc [--dry-run]
  │
  ├─ 扫描 envs/*/.profile.json 和 envs/*/generations/*.json
  │   收集所有引用: { gcc@12.0, gcc@15.0, cmake@4.0.2, dadk@0.4.0, ... }
  │
  ├─ 遍历 xim/xpkgs/<name>/<version>/
  │   若 name@version 不在引用集 → 候选删除
  │
  ├─ dry-run → 打印 "would remove xpkgs/dadk/0.2.0 (18MB)"
  │   实际    → fs::remove_all(xpkgs/dadk/0.2.0/)
  │
  └─ 输出: freed 1.2GB (5 packages removed)
```

命令入口：`xlings self gc` 或 `xlings store gc`（挂在 `xself` 或新增 `xlings.store` 的轻量查询模块下）。

---

## 八、C++23 模块变化（最小化）

相比上一版设计，**不再需要 `xlings.store` 模块**（xpkgs 由 xim 管理，C++ 侧只需知道路径），**只需新增 `xlings.profile`**：

```
原有模块（保持不变）:
  xlings.cmdprocessor
  xlings.config
  xlings.platform / :linux / :macos / :windows
  xlings.xself
  xlings.log / utils / i18n / json

新增模块:
  xlings.env      ← 用户命令: new / use / list / remove / rollback
  xlings.profile  ← Profile 世代管理: load / commit / list_generations / rollback
                     + 同步写 xvm/.workspace.xvm.yaml
                     + GC 逻辑（扫描引用集，删除 xpkgs 条目）
```

`xlings.profile` 接口：

```cpp
// core/profile.cppm
export module xlings.profile;

import std;
import xlings.config;
import xlings.json;

export namespace xlings::profile {

struct Generation {
    int number;
    std::string created;
    std::string reason;
    std::map<std::string, std::string> packages;  // name → version
};

// 读取当前世代
Generation load_current(const std::filesystem::path& envDir);

// 提交新世代（安装/删除后调用）并同步 workspace.yaml
int commit(const std::filesystem::path& envDir,
           std::map<std::string, std::string> packages,
           const std::string& reason);

// 列出历史世代
std::vector<Generation> list_generations(const std::filesystem::path& envDir);

// 回滚到第 N 世代
int rollback(const std::filesystem::path& envDir, int targetGen);

// GC: 删除 xpkgs 中无任何 profile 引用的条目
int gc(const std::filesystem::path& xlingHome, bool dryRun = false);

} // namespace xlings::profile
```

---

## 九、现有数据迁移（向后兼容）

```
现有结构:
  $XLINGS_DATA/
    bin/
    xim/xpkgs/
    xvm/.workspace.xvm.yaml

迁移目标:
  XLINGS_HOME/
    xim/xpkgs/              ← 从 $XLINGS_DATA/xim/xpkgs/ 移动（或软链）
    envs/default/
      bin/                  ← 从 $XLINGS_DATA/bin/ 移动（或软链）
      xvm/workspace.yaml    ← 从 $XLINGS_DATA/xvm/ 移动（或软链）
      .profile.json         ← 从 workspace.yaml 生成（初始世代）
```

`xlings self migrate`（或 `xlings self init` 的一部分）执行迁移，原目录可保留软链向后兼容。

---

## 十、实现优先级

| 阶段 | 内容 | 说明 |
|------|------|------|
| P0 | `xlings.env`: new / use / list | 多环境基础命令 |
| P0 | `xlings.profile`: commit / load_current | 世代写入与读取 |
| P0 | Config 支持 activeEnv → XLINGS_DATA 路由 | 核心路径解析 |
| P0 | `xlings self migrate` 迁移现有数据 | 平滑升级 |
| P1 | `xlings.profile`: rollback | 回滚命令 |
| P1 | `xlings.profile`: gc | 清理旧版本包 |
| P1 | xim XLINGS_PKGDIR 支持 | xpkgs 全局共享 |
| P2 | `xlings env remove` | 删除环境 |
| P2 | `xlings env info` | 环境详情 |
| P3 | `.profile.json` 导出/导入 | 环境复现 |