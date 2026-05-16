# xlings install / remove / use / list / update 与 subos 的绑定分析

> 起因:用户在 `[xsubos:host]` 里 `xlings remove gcc` 之后,`gcc` 命令报 `no version set for 'gcc'`。
> 用户进一步指出:`xlings list / use / update` 当前显示的是**全局 xpkgs**所有版本,而不是当前 subos 的视图。subos 应该是个真正的"作用域",资源(payload)复用,但每个 subos 维护自己的"已注册版本"集合 + active 指针。

---

## 1. 当前数据模型(实测)

### 1.1 全局 `~/.xlings.json`(部分关键字段)

```json
{
  "activeSubos": "default",
  "subos": { "default": {...}, "host": {...} },
  "versions": {
    "gcc": {
      "type": "program",
      "bindings": {
        "xim-gnu-gcc":      { "11.5.0": "11.5.0", "15.1.0": "15.1.0",
                              "16.1.0": "16.1.0", "9.4.0": "9.4.0" },
        "xim-musl-gnu-gcc": { "15.1.0-musl": "15.1.0-musl" }
      },
      "versions": {
        "11.5.0": { "alias": [...], "path": "..." },
        "15.1.0": {...},
        "16.1.0": {...},
        ...
      }
    },
    "node": {...},
    ...
  }
}
```

`versions DB` 是**全局**的、按 program 名聚合。`bindings` 字段记录了"哪个 binding 根包(xim-gnu-gcc / xim-musl-gnu-gcc)提供了哪些版本"—— **命名空间在这里出现**,但只是"哪个上游产出了这个版本"的元数据,不是 subos 视角的"哪个 subos 拥有这个版本"。

### 1.2 每 subos `subos/<n>/.xlings.json`

```json
{
  "workspace": {
    "gcc": "15.1.0",      ← active 版本指针
    "python3": "3.13.5",
    ...
  }
}
```

`workspace` 是**每 subos** 的,只记录 active 版本指针。**没有"本 subos 注册了哪些版本"的集合**。

### 1.3 shim 文件(`subos/<n>/bin/<prog>`)

每个 subos 自己的 `bin/` 下有一组 shim(全部硬链到同一个 xlings 二进制)。shim 运行时:
1. 读 `Config::effective_workspace()` 拿 `workspace[prog]` → 得到 active 版本号
2. 用这个版本号到 `versions DB` 里 `match_version(name, ver)` 拿到真实 payload 路径
3. exec 真实 binary

→ shim 的"运行时上下文"完全等价于 `(workspace, versions DB)` 二元组。

---

## 2. 当前 `xlings <verb>` 各命令的 subos 感知度

| 命令 | 影响范围 | subos 感知吗? |
|---|---|---|
| `xlings install foo@v1` | 下载 payload(全局)+ 注册到 versions DB(全局)+ active 在当前 subos workspace(per-subos)+ 创建 shim 在当前 subos bin(per-subos) | **半感知**:payload 全局,active+shim 在当前 subos |
| `xlings remove foo` | 全局 versions DB 删 entry + 当前 subos workspace 抹除/auto-switch + 当前 subos bin 删 shim(若全没了) | **半感知**:全局删 entry **影响所有 subos**,但 workspace/shim 只动当前 subos |
| `xlings use foo` | 列**全局** versions DB 里所有 foo 的版本;改当前 subos 的 workspace | **不感知**:列表来自全局 |
| `xlings list [filter]` | 列**全局** versions DB 里所有(filter 后)的程序与版本 | **不感知** |
| `xlings update foo` | 全局拉新版本 → 全局注册到 versions DB → 当前 subos workspace 切到新版本 | **半感知** |

**核心问题**:`versions DB` 是全局的,但用户期望的语义是"每个 subos 有自己的视图"。

---

## 3. 用户场景下的具体 bug

### 3.1 复现路径

```
[xsubos:host] $ xlings install gcc@15           # gcc 15.1.0 已在版本 DB
                                                 # → 在 host 的 workspace 设 gcc=15.1.0
                                                 # → 在 host 的 bin 创建 gcc shim
[xsubos:host] $ gcc --version                    # → host shim → workspace[gcc]=15.1.0 → exec
                                                 #   "gcc 15.1.0" ✓

[xsubos:host] $ xlings remove gcc
  ◆ xim:gcc@15.1.0  (subos: host)               # ← UI 提示 from subos 'host'
  ✓ xim:gcc@15.1.0 removed  (subos: host)

[xsubos:host] $ gcc --version
[error] xlings: no version set for 'gcc'        # ← 报错
[error]   hint: xlings use gcc <version>
```

### 3.2 实际发生了什么(代码层追踪)

`xlings remove gcc` → `installer.uninstall("gcc")`:
1. resolve 到 `xim:gcc@15.1.0`(short-name 优先匹配 xim 仓库)
2. 跑 xim:gcc.lua 的 `uninstall()` hook,hook 里调:
   - `xvm.remove(prog, gcc_version)` — 大部分程序传了版本
   - `xvm.remove(prog)` — 少部分程序**未传版本**(如 `cc`、`xim-gnu-gcc`)
   - `xvm.remove("xim-gnu-gcc")` — 整个 binding 根
3. installer.cppm 的 xvm_ops 循环逐条处理:
   - 有 version 的 → `xvm::remove_version(versions_mut, name, version)`
   - 无 version 但 `op.name == detachTarget`(name=="gcc",target=="gcc") → `effective_version = detachVersion` (= "15.1.0") → 走 versioned 路径
   - 无 version 且 `op.name != detachTarget`(如 `xim-gnu-gcc`) → 走 versionless 重器,**整条 erase + workspace 清空 + 删 shim**

4. **survivors check**:`Config::versions_mut().find("gcc")` 是否还有 versions:
   - DB 里 gcc 还有 11.5.0 / 16.1.0 / 9.4.0 → survivors = **true**
   - prev_active = host workspace[gcc] = "15.1.0"
   - effective_version = "15.1.0",match prev_active → **auto-switch** workspace[gcc] = pick_highest = "16.1.0"
5. `Config::save_workspace()` → 我修过的 else 分支 → 写到 `subos/host/.xlings.json`

**理论上**:host workspace[gcc] = "16.1.0" 后,`gcc --version` 应该跑 16.1.0,不该报 "no version set"。

### 3.3 为什么实际报 "no version set"?

最可能的原因(待用户实测确认):

- `xim-gnu-gcc` 的 versionless `xvm.remove()` op **整条 erase 了 versions DB 里的根条目**,这个根条目是 binding 的"根标识"。erase 之后,后续基于 binding 的查找可能返回空 → workspace[gcc] 设值时,内部状态不一致,save 出来 workspace[gcc] 实际为空。
- 或者:auto-switch 算的 highest 是 "16.1.0",但 host 之前从没有过 16.1.0 这个 binding 的本地激活记录,subos/host/bin 下没对应 shim,exec 找不到目标 → shim 内部表现是"workspace 是空的"。
- 或者:用户的 PATH 里 `subos/default/bin` 在 `subos/host/bin` 之后,shim resolution 路径走的是 default 的 shim,而 default workspace[gcc] 也被同一次 remove 操作影响了(?)。

**确切原因需要在用户机器上 dump 这两个文件确认**:
```bash
cat ~/.xlings/.xlings.json | jq '.versions.gcc'
cat ~/.xlings/subos/host/.xlings.json
cat ~/.xlings/subos/default/.xlings.json
ls -la ~/.xlings/subos/host/bin/gcc ~/.xlings/subos/default/bin/gcc
```

---

## 4. 用户期望的语义

> 资源复用,但环境要和当前 subos 一致

具体到每个命令:

| 命令 | 期望语义 |
|---|---|
| `xlings install foo@v1` | 下载 payload(全局复用),注册到当前 subos 的"已注册版本"集合,active 切到 v1,shim 在当前 subos bin |
| `xlings remove foo` | 从当前 subos 的"已注册版本"集合移除 foo,active 抹除,shim 删除。**不动**全局 payload(可能其它 subos 还在用) |
| `xlings list` | 列出**当前 subos 注册的**包,而不是全局 payload 集合 |
| `xlings use foo` | 列**当前 subos 已注册的** foo 的版本 |
| `xlings update foo` | 在当前 subos 的范围内升级 foo |

要做到这点,需要新引入:**每 subos 的 "registered versions" 集合**。

---

## 5. 三种修复方向

### 方向 A:最小改动 — 只修 shim 的 UX

**做什么**:

`shim.cppm` 的 `version.empty()` 报错路径,先查全局 versions DB —— 如果 0 个版本,提示 `xlings install`,而不是 `xlings use`。

```cpp
if (version.empty()) {
    auto db = Config::versions();
    auto all = get_all_versions(db, program_name);
    if (all.empty()) {
        log::error("xlings: '{}' is not installed", program_name);
        log::error("  hint: xlings install {}", program_name);
    } else {
        log::error("xlings: no version of '{}' active in current subos", program_name);
        log::error("  hint: xlings use {} <version>  (available: {})", ...);
    }
    return 1;
}
```

**优点**:几行 C++,不动语义,马上让用户看到"对的话术"。
**缺点**:不解决 root cause,只缓解症状。
**影响面**:零风险。
**适合**:0.4.18 patch。

### 方向 B:installer 收紧 versionless `xvm.remove` 的处理

**做什么**:

修改 `installer.cppm` 的 versionless 分支,**不再整条 erase versions DB 条目**,只移除当前正在卸载的版本对应的 entry,保护其它版本/binding。

```cpp
if (effective_version.empty()) {
    // OLD: erase whole entry → too aggressive when multiple versions exist
    // NEW: only erase if the only registered version was the one we're removing
    auto& vinfo = Config::versions_mut()[op.name];
    if (vinfo.versions.size() <= 1 ||
        (vinfo.versions.size() == 1 && vinfo.versions.contains(detachVersion))) {
        Config::versions_mut().erase(op.name);
        Config::workspace_mut().erase(op.name);
        remove_shim_if_present();
    } else {
        // multiple versions exist — leave DB alone, only update workspace
        // if active was the one being removed
    }
    continue;
}
```

**优点**:在不引入新数据结构的前提下,修住"versionless remove 误杀"的 root cause。
**缺点**:语义微妙,需要充足的多包同名场景 e2e。
**影响面**:中等。
**适合**:0.4.19 / 0.5.x(需要更多测试覆盖)。

### 方向 C:per-subos "registered versions" 集合

**做什么**:

在 `subos/<n>/.xlings.json` 里新增一个字段 `registered`:

```json
{
  "workspace": { "gcc": "15.1.0" },
  "registered": {
    "gcc": ["15.1.0"],
    "python3": ["3.12.0", "3.13.5"]
  }
}
```

每个命令的语义重写:

- `install foo@v` (in subos X):全局 payload 复用 + `registered[X][foo].push(v)` + 设 workspace[foo]=v + 创建 X/bin/foo
- `remove foo[@v]` (in X):`registered[X][foo].erase(v)`;若 v 是 active → workspace.erase 或 auto-switch 到 registered[X] 中剩余;若 registered[X][foo] 空 → 删 X/bin/foo shim;**永不动**全局 versions DB
- `list` (in X):列 `registered[X]` 的 entries
- `use foo` (in X):列 `registered[X][foo]` 的版本
- `update foo` (in X):全局拉新版本 → registered[X][foo] 加新版本 + active 切

**优点**:真正实现"subos 是真正的作用域";资源复用 + 环境一致兼得;符合用户的 mental model。
**缺点**:数据模型变更,需要 schema 迁移,代码改动 ~400 行,e2e 大量新增。
**影响面**:大。
**适合**:**0.5.0**(minor version,可以接受向前兼容的 schema migration)。

---

## 6. 推荐路线

### 0.4.18(patch)

- **方向 A** + 已经在 PR #272 的紫色 prompt
- 加这份分析文档进 `docs/plans/`
- 不改 install/remove 语义,只改 shim 报错 hint

理由:0.4.17 已经被反复打补丁,0.4.18 应该是**收口 patch**,只做"零风险 + 立即可见的痛点缓解"。

### 0.4.19 或者 0.5.0-rc.1

- **方向 B**:installer versionless remove 的收紧
- 跟 xim-pkgindex 协作,把所有 `xvm.remove(prog)` 改成 `xvm.remove(prog, version)`(双保险)
- 增加多包同名 e2e 覆盖

### 0.5.0(minor)

- **方向 C**:per-subos registered versions 集合
- schema 迁移工具
- `xlings list / use / update` 重写为 subos-aware
- 完整 e2e 矩阵 + 文档

---

## 7. 数据流图(供后续参考)

```
现在(0.4.x):
                ┌─────────────────────────┐
                │  ~/.xlings.json         │
                │   versions: {           │  ← 全局 payload + 元数据
                │     "gcc": {            │
                │       versions: {       │
                │         "11.5.0": {...}, │
                │         "15.1.0": {...}, │
                │         ...             │
                │       }                 │
                │     }                   │
                │   }                     │
                └─────────────────────────┘
                          ↑
                          │ install / remove (全局)
                          │
        ┌─────────────────┴────────────────────────────┐
        │                                              │
┌───────▼─────────┐                          ┌─────────▼────────┐
│ subos/default   │                          │ subos/host       │
│  workspace:     │                          │  workspace:      │
│   "gcc" → 15... │  ← active 指针(每 subos)│   "gcc" → 15...  │
└─────────────────┘                          └──────────────────┘

未来(0.5.x):
                ┌─────────────────────────┐
                │  ~/.xlings.json         │
                │   versions: {  ← 全局 payload, 复用 │
                │     ...                 │
                │   }                     │
                └─────────────────────────┘
                          │
                          │ payload reuse (download once)
                          │
        ┌─────────────────┴────────────────────────────┐
        │                                              │
┌───────▼─────────────────────┐         ┌──────────────▼──────────────┐
│ subos/default               │         │ subos/host                  │
│  registered:                │         │  registered:                │
│   "gcc": ["11.5.0","15.1.0"]│         │   "gcc": ["15.1.0"]          │
│  workspace:                 │         │  workspace:                 │
│   "gcc" → 15.1.0            │         │   "gcc" → 15.1.0            │
└─────────────────────────────┘         └─────────────────────────────┘

  install/remove/list/use 全在 subos 局部  +  payload 全局复用
```

---

## 8. 一句话回答用户的核心诉求

> "list / use / update 没有和当前 subos 环境对齐 ... 资源复用 但是环境要和当前 subos 一致"

**现状**:`versions DB` 是全局的,这些命令直接读全局 → 没有 subos 视图。
**修复路径**:**方向 C**(per-subos registered 集合),0.5.0 的事。
**0.4.18 临时缓解**:**方向 A**,把 shim 报错 hint 改对,至少不再误导用户去跑 `xlings use <pkg>`。
