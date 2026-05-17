# xlings RPATH 解决方案 & OS 级包管理器演进

> 关联文档:
> - [env-analysis.md — 现有架构分析](env-analysis.md)
> - [env-store-design.md — 环境隔离设计](env-store-design.md)

---

## 一、问题再定义

RPATH 问题的根本是：**xlings 的安装路径在不同机器、不同用户上各不相同**，导致编译时嵌入的绝对路径在换机器或移动目录后失效。

```
机器 A: /home/alice/.xlings/xim/xpkgs/fromsource-x-gcc/15.0/lib
机器 B: /opt/xlings/xim/xpkgs/fromsource-x-gcc/15.0/lib
                ↑ 路径不同 → 硬编码的 RPATH 失效
```

---

## 二、现有基础设施的关键发现

在深入阅读 `core/xvm/xvmlib/shims.rs` 和 `versiondb.rs` 后，发现 xvm 的版本数据库（`.workspace.xvm.yaml`）**已经支持对每个工具设置独立的环境变量**：

```rust
// versiondb.rs
pub struct VData {
    pub alias: Option<String>,
    pub path: String,
    pub envs: Option<IndexMap<String, String>>,  // ← 每个工具独立的环境变量
    pub bindings: Option<IndexMap<String, String>>,
}
```

```rust
// shims.rs - 已实现 LD_LIBRARY_PATH 注入
pub fn add_env(&mut self, key: &str, value: &str) {
    } else if key == "LD_LIBRARY_PATH" || key == "DYLD_LIBRARY_PATH" {
        self.ld_library_path_env = Some(...)  // ← 已实现
    }
}
```

`path` 字段已支持相对路径（相对于 `XLINGS_DATA`）：

```rust
// shims.rs 第 192 行
let mut base = if Path::new(&self.path).is_relative() {
    XVM_WORKSPACE_DIR.get().map(|w| Path::new(w).join(&self.path))
    // ...
};
```

**结论：解决 RPATH 问题所需的底层机制已经完全实现，只差一个关键功能：环境变量值中的路径变量替换。**

---

## 三、最简洁的解决方案：变量替换 + `$ORIGIN`

### 3.1 核心思路

在 xvm 读取 workspace.yaml 的 `envs` 字段时，对值进行变量替换：

```
${XLINGS_HOME}  →  实际的 XLINGS_HOME 路径
${XLINGS_DATA}  →  实际的 XLINGS_DATA 路径
```

这样 xpkg 安装脚本可以写出**与路径无关的 LD_LIBRARY_PATH**：

```yaml
# envs/default/xvm/.workspace.xvm.yaml
versions:
  gcc:
    path: ${XLINGS_HOME}/xim/xpkgs/fromsource-x-gcc/15.0
    envs:
      LD_LIBRARY_PATH: "${XLINGS_HOME}/xim/xpkgs/fromsource-x-gmp/6.3.0/lib:\
                        ${XLINGS_HOME}/xim/xpkgs/fromsource-x-mpc/1.3.1/lib"
```

无论 XLINGS_HOME 在哪里，xvm 在运行时展开 `${XLINGS_HOME}` 后，LD_LIBRARY_PATH 永远正确。

### 3.2 实现位置

修改 `core/xvm/xvmlib/shims.rs` 中 `set_vdata()` 方法，在读取 envs 时做展开：

```rust
// shims.rs 新增辅助函数
fn expand_xlings_vars(value: &str) -> String {
    let home = env::var("XLINGS_HOME").unwrap_or_default();
    let data = env::var("XLINGS_DATA").unwrap_or_default();
    value
        .replace("${XLINGS_HOME}", &home)
        .replace("${XLINGS_DATA}", &data)
}

// 在 set_vdata() 中调用
pub fn set_vdata(&mut self, vdata: &VData) {
    self.set_path(&expand_xlings_vars(&vdata.path));  // path 也展开
    if let Some(envs) = &vdata.envs {
        for (k, v) in envs {
            self.add_env(k, &expand_xlings_vars(v));  // ← 展开后注入
        }
    }
    // ...
}
```

**改动量：约 15 行 Rust 代码**，无破坏性变更（无 `${...}` 的现有 yaml 不受影响）。

### 3.3 两层方案组合

| 层次 | 方案 | 适用场景 | 实现成本 |
|------|------|---------|---------|
| 构建时 | `$ORIGIN/../lib` 作为 RPATH | **同一包内**的 lib 依赖 | xpkg build hook 中加一行编译参数 |
| 运行时 | xvm `${XLINGS_HOME}` 变量替换 → LD_LIBRARY_PATH | **跨包**的 lib 依赖 | ~15 行 Rust |

两层结合，完全覆盖所有场景，且都与 XLINGS_HOME 的绝对路径无关。

### 3.4 xpkg 安装脚本的写法（示例）

```lua
-- fromsource-x-gcc/xpkg.lua 中的 config 钩子
function config()
    local xpkgdir = system.xpkgdir()  -- = XLINGS_HOME/xim/xpkgs
    -- 构建时已用 $ORIGIN/../lib，同包 lib 不需要处理
    -- 只需声明跨包依赖的 LD_LIBRARY_PATH
    xvm.add("gcc", {
        path = "${XLINGS_HOME}/xim/xpkgs/fromsource-x-gcc/15.0",
        envs = {
            LD_LIBRARY_PATH = table.concat({
                "${XLINGS_HOME}/xim/xpkgs/fromsource-x-gmp/6.3.0/lib",
                "${XLINGS_HOME}/xim/xpkgs/fromsource-x-mpc/1.3.1/lib",
                "${XLINGS_HOME}/xim/xpkgs/fromsource-x-mpfr/4.2.0/lib",
            }, ":"),
        },
    })
end
```

xim 调用 `xvm.add()` 写入 workspace.yaml，xvm 在 exec 前展开并注入，整个链路只需一次配置，永久有效。

---

## 四、xlings 作为操作系统包管理器的演进

### 4.1 关键洞察：xpkgs 结构天然适合 OS 级包管理

```
xpkgs/<name>/<version>/
  bin/
  lib/
  include/
  share/
```

这个结构与 Nix store、Homebrew Cellar、pkgsrc 的思路完全一致。唯一的区别是路径是否固定。

**当 xlings 作为 OS 级包管理器时，只需确立一个固定的系统级 store 路径，RPATH 问题自然消失。**

### 4.2 三阶段演进路线

```
阶段 1（现在）: 用户空间工具管理器
─────────────────────────────────────────────────────
XLINGS_HOME = 任意路径（~/.xlings, /opt/xlings, ...）
xpkgs = $XLINGS_HOME/xim/xpkgs/（可移动）
解决方案: ${XLINGS_HOME} 变量替换 + $ORIGIN RPATH


阶段 2（中期）: 多用户系统级包管理器
─────────────────────────────────────────────────────
系统 store = /xls/store/<name>/<version>/  （固定路径）
用户 envs  = ~/.xlings/envs/<name>/        （仍然灵活）

每个用户有自己的环境（bin/ + workspace.yaml）
所有用户共享同一份系统 xpkgs
RPATH 可直接写 /xls/store/<dep>/<ver>/lib  → 永久有效


阶段 3（远期）: xlings OS 的原生包管理器
─────────────────────────────────────────────────────
/xls/store/         ← 系统不可变 store（系统启动时只读挂载）
/xls/system/        ← 当前系统环境（xvm workspace，root 管理）
/xls/boot/          ← 引导层 + 内核模块
/home/<user>/.xlings/envs/  ← 用户环境（与系统 store 共享包）
```

### 4.3 固定系统路径 `/xls/store/`

选择 `/xls/` 而非 `/nix/` 的理由：

- 简短（3 字符），路径嵌入二进制时占用少
- 唯一，不与任何 FHS 标准目录冲突
- 跨平台概念：Linux（`/xls/`）、macOS（`/xls/` 或 `/Library/xls/`）、Windows（`C:\xls\`）

固定路径带来的好处：

```
/xls/store/fromsource-x-gcc/15.0/bin/gcc
  RPATH = /xls/store/fromsource-x-gcc/15.0/lib
          /xls/store/fromsource-x-gmp/6.3.0/lib
```

路径固定 → RPATH 永远有效 → 二进制可以跨机器共享（前提是相同架构）。这正是 Nix 的核心优势，xlings 通过固定 `/xls/store/` 获得同等能力。

### 4.4 用户模式 vs 系统模式的统一架构

```
┌──────────────────────────────────────────────────────────────────┐
│                         xlings 统一架构                           │
│                                                                  │
│  用户模式:                     系统模式:                          │
│  XLINGS_HOME = ~/.xlings       XLINGS_HOME = /xls               │
│  xpkgs = ~/...xpkgs/          xpkgs = /xls/store/               │
│  env 变量: ${XLINGS_HOME}/...  固定绝对路径                       │
│                                                                  │
│  ┌────────────┐  共用  ┌────────────────────────────────────┐   │
│  │  xim (Lua) │──────▶│  xpkgs/<name>/<version>/           │   │
│  │  包安装器  │        │  (用户模式: 可移动路径)             │   │
│  └────────────┘        │  (系统模式: /xls/store/)           │   │
│                        └────────────────────────────────────┘   │
│  ┌────────────┐  共用  ┌────────────────────────────────────┐   │
│  │  xvm-shim  │──────▶│  workspace.yaml (版本激活清单)      │   │
│  │  版本路由  │        │  + ${XLINGS_HOME} 变量展开         │   │
│  └────────────┘        └────────────────────────────────────┘   │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │  C++23 主程序 (xlings)                                     │ │
│  │  - 用户模式: 路径解析 + 变量注入                           │ │
│  │  - 系统模式: 权限管理 + 多用户隔离 + 原子系统更新          │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### 4.5 系统模式的新能力需求

作为 OS 包管理器，xlings 需要在现有基础上增加：

| 能力 | 对应现有概念 | 新增内容 |
|------|------------|---------|
| 多用户隔离 | 多环境（envs/） | 系统 env（root） + 用户 env（per user） |
| 系统原子更新 | profile 世代 | 系统级世代 + 引导时激活 |
| 依赖声明与解析 | xpkg 的 `deps` 字段 | 完整依赖图解析（拓扑排序） |
| 签名与信任 | 无 | 包签名验证（Ed25519） |
| 内核模块 | 无 | `kmod-x-*` 包类型 |
| 系统服务 | 无 | `service-x-*` 包类型 + init 集成 |
| 不可变系统根 | 无 | `/xls/store/` 只读挂载 + 覆盖层 |

### 4.6 不可变系统根的实现思路

```
/xls/store/          ← 只读（squashfs 或 overlayfs lower）
/xls/overlay/upper/  ← 可写层（tmpfs，重启清空）
/xls/overlay/merged/ ← 合并视图（系统真正看到的 /xls/store）
```

通过 overlayfs，`/xls/store/` 本身是不可变的（可以验证完整性），系统更新 = 写入新的 squashfs 镜像。这与 OSTree、NixOS 的系统更新模型一致。

---

## 五、RPATH 方案在 OS 演进中的一致性

```
现在（用户模式）:
  gcc RPATH = $ORIGIN/../lib                    ← 同包内
  workspace.yaml envs:
    LD_LIBRARY_PATH = ${XLINGS_HOME}/xim/...   ← 跨包，运行时展开

迁移期（系统+用户混用）:
  系统包 RPATH = /xls/store/dep/ver/lib         ← 固定，永久有效
  用户包 RPATH = $ORIGIN/../lib                 ← 同包内，可移动
  workspace.yaml envs:
    LD_LIBRARY_PATH = ${XLINGS_HOME}/xim/...   ← 用户包
                    = /xls/store/dep/ver/lib    ← 系统包（直接写死）

OS 模式:
  所有包 RPATH = /xls/store/<dep>/<ver>/lib     ← 统一固定路径
  不再需要 ${XLINGS_HOME} 变量替换
  不再需要 LD_LIBRARY_PATH 注入
```

三个阶段的 RPATH 方案是**向上兼容**的：用户模式的变量替换方案在系统模式下不需要移除，只是不再必要。

---

## 六、总结

### RPATH 问题的最简解法

```
两行代码概括:
  1. xpkg build hook: 用 $ORIGIN/../lib 处理同包依赖
  2. xvm 展开 ${XLINGS_HOME}: 处理跨包依赖的 LD_LIBRARY_PATH

实现代价: ~15 行 Rust (expand_xlings_vars 函数)
         xpkg 脚本约定: 用 ${XLINGS_HOME}/xim/xpkgs/... 写依赖路径
```

### OS 演进的核心

**xpkgs 已经是正确的数据结构，只差一个固定的系统挂载点 `/xls/store/`。**

xlings 从用户工具到 OS 包管理器的演进，不需要重写任何核心架构，只需要：

1. 确立 `/xls/store/` 系统路径约定
2. 增加权限模型（root 操作系统包，用户操作私有环境）
3. 增加依赖图解析
4. 增加包签名验证
5. 增加 `kmod-x-*`、`service-x-*` 等系统包类型

xim / xvm / xpkg 的现有设计无需大改，只是在其上叠加系统级的能力。
