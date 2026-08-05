# xlings subos slice 1 详细设计:Configuration 基质补完

**日期**: 2026-08-05
**类型**: 详细设计(detailed design)—— slice 1 可实施规格
**范围**: xlings 侧新增最小机制,让 `mesa` 端到端可装可跑;不引入 platform 抽象、不引入新 CLI 动词
**关联**:
- `2026-08-05-userspace-distro-hermetic-strategy.md`(运行时边界策略)
- `2026-08-05-ecosystem-three-tier-and-composable-distro.md`(生态定位讨论)

**状态**:所有 schema 与 CLI 决策已定案(2026-08-05 会话收敛);实施细节标 [OPEN] 待具体施工时定

---

## 0. TL;DR

**问题**:issue #352 类场景——xlings 的 subos 缺 Configuration 基质,GL/Vulkan/字体等子系统的发现协议(env vars + config 目录)没有 per-subos 承载。

**Slice 1 目标**:在 xlings 侧引入最小机制补完 Configuration 基质,让 `mesa` payload 装进 subos 后 GLFW 用户程序能跑起来。

**核心决策**:
- Manifest 新增单块 `subos_info`(对齐 `xlings subos info` CLI)
- 字段:`schema_version` / `runtime` / `envs` / `created_at` / `created_by`
- **不引入 platform 抽象**——`runtime` 就是一个 xvm binding 字符串,self-describing
- **不新增 CLI 动词**——复用 `xlings subos use --shell` 与 `--cmd`,slice 1 扩展它们的行为
- 新 xpkg API:`subos.env{}` 让 pkg config() 声明 env

**Slice 1 明确不做**:
- Platform manifest 概念
- 多 runtime(多 glibc 等)—— 生态阻塞项另议,slice 1 只需字段就位
- Capabilities_host / activation_hooks
- `xlings subos export/import/diff/snapshot/migrate`
- 跨 shell/OS 完备性(bash 优先,fish/zsh 之后补)
- 自动 schema upgrade

---

## 1. 分析框架:三层基质(分析工具,不是 schema 分块)

用户程序在 subos 里跑起来需要三层基质齐备:

| 层 | 内容 | xlings 现状 | Slice 1 |
|---|---|---|---|
| **1. Bootstrap** | PT_INTERP + CRT + libc/runtime | ✅ glibc pkg + elfpatch | 不动 |
| **2. Discovery** | PATH + RPATH + xvm binding | ✅ xvm.add/xvm.files + elfpatch | 不动 |
| **3. Configuration** | env vars + config 发现约定 | 🟡 无 per-subos 承载 | **补完** |

三层是分析工具,不是 schema 组织形式——schema 只有一个 `subos_info` 块。

---

## 2. 最小 subos 不变量

`xlings subos new <name>` 完成后,以下条件必须同时成立(doctor 用同一套判据检查):

**文件系统**:
- I1. `$XLINGS_HOME/subos/<name>/` 目录存在
- I2. `$XLINGS_HOME/subos/<name>/.xlings.json` 文件存在且合法 JSON
- I3. xvm 状态目录已初始化(即使为空)

**Manifest 结构**:
- I4. 文件顶级存在 `subos_info` 对象
- I5. `subos_info.schema_version` == `1`
- I6. `subos_info.runtime` 是非空字符串,格式 `<pkg>@<version>`
- I7. `subos_info.envs` 是对象(可空 `{}`)
- I8. `subos_info.created_at` / `created_by` 是非空字符串

**注册状态**:
- I9. `$XLINGS_HOME/.xlings.json` 的 `subos` 注册表包含 `<name>` 条目

**基质就绪**(逻辑判据,非文件检查):
- I10. `subos_info.runtime` 引用的 pkg 在 xvm 里已注册(Bootstrap 可用)
- I11. `envs` 段每个 key 对应的 pkg 在 xvm 里已装(env 与 xvm 一致)

**违反任一 → doctor 报错并给出修复动作**。Reporter 与 repairer 用同一函数,遵循 xlings 生态"reporter/repairer 谓词一致"原则。

**"空集合 ≠ 缺失"原则**:所有可增长段(`envs`)在 `new` 时初始化为空对象/数组,不省略 key。避免 reader 处理 optional。

---

## 3. subos_info schema(完整)

### 3.1 位置

**Per-subos** `.xlings.json`:
- Home 全局 subos:`$XLINGS_HOME/subos/<name>/.xlings.json`
- 项目 subos:`<project>/.xlings/subos/<name>/.xlings.json`

**不出现在**:
- 项目根 `.xlings.json`(那里 `subos` 是字符串选择器)
- Home 根 `.xlings.json`(那里 `subos` 是注册表 map)

### 3.2 完整结构

```json
{
  "workspace": { ... },              // 现有段,不动

  "subos_info": {                     // slice 1 新增
    "schema_version": 1,
    "runtime":       "glibc@2.39",
    "envs":          { },
    "created_at":    "2026-08-05T14:23:11Z",
    "created_by":    "xlings 0.4.71"
  }
}
```

### 3.3 字段规约

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `schema_version` | integer | ✅ | 当前 = 1,未来通过 probe pattern 演化 |
| `runtime` | string | ✅ | xvm binding 格式 `<pkg>@<version>`,self-describing |
| `envs` | object | ✅ | Per-pkg env 声明,可空 `{}` |
| `created_at` | string (ISO-8601) | ✅ | wall clock,信息字段 |
| `created_by` | string | ✅ | 创建时 xlings 版本,信息字段 |

### 3.4 runtime 字段:self-describing 约定

```
runtime: "glibc@2.39"       → Linux + glibc 家族
runtime: "musl@1.2.5"        → Linux + musl 家族
runtime: "wasi-libc@0.1"     → WASI 家族(未来)
runtime: "macos_sdk@14.5"    → macOS 家族(未来)
runtime: "ucrt@10.0.22000"   → Windows 家族(未来)
```

**家族信息通过 pkg 名字前缀由 xlings 侧辅助函数派生**,不作为独立字段冗余存储:

```
family_of(runtime_binding) →
    glibc@*      → linux-<arch>-glibc
    musl@*       → linux-<arch>-musl
    wasi-libc@*  → wasm32-wasi
    macos_sdk@*  → darwin-<arch>
    ucrt@*       → windows-<arch>-ucrt
    默认          → "unknown"
```

映射表在 xlings 侧代码中维护,新 OS 加入时更新。**Slice 1 只需 `glibc@*` 一项**。

### 3.5 envs 段结构

```json
"envs": {
  "mesa@25.0.0": [
    { "var": "LIBGL_DRIVERS_PATH",        "op": "set",     "value": "${pkgdir}/lib/dri" },
    { "var": "__EGL_VENDOR_LIBRARY_DIRS", "op": "set",     "value": "${pkgdir}/share/glvnd/egl_vendor.d" },
    { "var": "XDG_DATA_DIRS",             "op": "prepend", "value": "${pkgdir}/share" }
  ]
}
```

**Schema**:
- Key:pkg binding,格式 `<name>@<version>`
- Value:数组,每项 `{ var: string, op: enum, value: string }`

**op 允许取值**:
| op | 语义 | Slice 1 |
|---|---|---|
| `set` | 无条件设置(覆盖既有值) | ✅ 实现 |
| `prepend` | 前置到既有值(冒号分隔) | ✅ 实现 |
| `set-if-unset` | 仅在未设时设置 | ⬜ 未来 |
| `append` | 追加到既有值 | ⬜ 未来 |

**Slice 1 只实现 `set` 与 `prepend`**——足够覆盖 GL/Vulkan/fontconfig 类场景。

### 3.6 value 占位符

在 emit 时(activate 或 run)展开:

| 占位符 | 展开为 |
|---|---|
| `${pkgdir}` | 该 pkg 的 install 目录绝对路径 |
| `${subosdir}` | 该 subos 的根目录绝对路径 |
| `${home}` | 用户 home 目录 |
| `${xlings_home}` | `$XLINGS_HOME` |

**占位符是可迁移性的关键**——manifest 内 value 不含机器特定绝对路径。

### 3.7 Provider-scoped 所有权

`envs` 以 pkg binding 为 key,与 xvm.add / xvm.files 同一所有权模型:
- pkg install 时其 config() 通过 `subos.env{}` 写入
- pkg uninstall 时 xlings 自动删除该 key 整段
- 不允许 cross-pkg 声明(A pkg config() 声明 B pkg 的 env)

### 3.8 冲突语义(activate 时)

同一变量被多个 pkg 声明时:

| 场景 | 规则 |
|---|---|
| 多个 `set` 同变量 | doctor 报 warning;运行期以 **binding 序后到者**为准 |
| 多个 `prepend` | **binding 序后到者**在前(靠近变量头) |
| `set` 与 `prepend` 混合 | `set` 生效,`prepend` 忽略,doctor 报 warning |
| 用户已在 shell 里 export | **用户值优先**,pkg 声明不覆盖(见 UC-1) |

> **施工修正(2026-08-05)**:本节原写"以**装机顺序**后到者为准"。**装机顺序不在
> manifest 里**,要实现它就得加一个字段,而那个字段唯一的作用是让结果取决于历史 ——
> 两台机器持有逐字节相同的 manifest 会导出不同的值。这与 SH-1(可分享性)直接矛盾:
> 一份只在写它的机器上成立的描述不算描述。
>
> 改为按 **binding 字典序**。它由数据本身决定,可复现,且让 manifest 成为完整答案。
> 已由单测 `DoesNotDependOnDeclarationOrder` 钉住。

**冲突检测在 activate 时进行**,不在 write 时(简化 write 路径,写多次不 fail)。

---

## 4. xlings subos new(最小改动)

### 4.1 CLI 签名

现有:
```
xlings subos new <name> [--storage <mode>] [--image-size <size>] [--from <source>]
```

Slice 1 新增:
```
xlings subos new <name> [--runtime <spec>] [其他现有 flag]

--runtime <spec>   subos 的 runtime pkg binding,如 "glibc@2.39"
                   缺省:xlings 内嵌 fallback(当前为 "glibc@2.39")
```

**不新增其他 flag**——`--platform` / `default_platform` 不做。

### 4.2 执行流程

```
xlings subos new <name> [--runtime <spec>]

Step 1: 前置校验
  - <name> 未占用、命名合法
  - Home 目录可写

Step 2: 解析 runtime
  --runtime CLI 参数 → 若无 → xlings 编译时嵌入默认(slice 1: "glibc@2.39")
  → 记为 R

Step 3: 校验 R 可用
  - R 对应的 pkg 在索引里存在
  - 若目标机未装 → prompt "install now? [Y/n]"(默认 Yes)

Step 4: 物理创建
  - mkdir -p $XLINGS_HOME/subos/<name>/
  - 初始化 xvm 状态子目录
  - 若需要 → 装 R

Step 5: 写 subos_info 到 subos/<name>/.xlings.json
  {
    "workspace": {},
    "subos_info": {
      "schema_version": 1,
      "runtime":     "<R>",
      "envs":        {},
      "created_at":  "<ISO-8601 now>",
      "created_by":  "xlings <version>"
    }
  }

Step 6: 更新 home .xlings.json subos 注册表

Step 7: 验证不变量 I1~I11,任一失败回滚

Step 8: 输出提示
  "Created subos '<name>' with runtime <R>.
   Enter with:      xlings subos use <name>
   Run one command: xlings subos use <name> --cmd '<command>'
   Emit env script: xlings subos use <name> --shell"
```

### 4.3 错误场景

| 场景 | 退出码 | 提示 |
|---|---|---|
| `<name>` 已存在 | 1 | "use another name or `subos remove`" |
| `--runtime` 引用的 pkg 索引里无 | 2 | "no such runtime pkg; check `xlings install --search`" |
| Runtime 与 host arch 不容 | 3 | 提示 arch 详情 |
| Runtime 未装且用户拒绝安装 | 4 | 提示手动安装 |
| Step 4~6 部分成功后失败 | 5 | 反向回滚;若回滚失败,提示手动清理路径 |

失败必须显式,不 silent-success(memory: silent-success pattern)。

---

## 5. subos.env{} xpkg API

### 5.1 Lua API 签名

```lua
subos.env{
    var     = "LIBGL_DRIVERS_PATH",
    op      = "set",                                    -- 或 "prepend"
    value   = "${pkgdir}/lib/dri",
    binding = "mesa@" .. pkginfo.version()
}
```

### 5.2 参数

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `var` | string | ✅ | 环境变量名 |
| `op` | string | ✅ | `set` 或 `prepend`(slice 1 只这两种) |
| `value` | string | ✅ | 值,支持 §3.6 占位符 |
| `binding` | string | ✅ | Provider 标识,必须与本 pkg 一致(cross-pkg 拒绝) |

### 5.3 语义

- 调用时 append 到 `subos_info.envs[binding]` 数组
- 幂等:同 binding 内 (var, op, value) 三元组已存在则不重复写
- 写入原子:走 atomic write 路径(避免 flock 场景;memory: atomic write vs flock)
- **write 时不做冲突检测**——只在 activate 时统一按 §3.8 规则解冲突

### 5.4 老 client 兼容(V2 spec probe 模式)

> **施工修正(2026-08-05)**:本节原写的是 `if subos.env then`。**那个探针在所有
> client 上恒为真**,必须用 `type(...)`:
>
> ```lua
> if type(subos.env) == "function" then
>     subos.env{ var = "...", op = "set", value = "...", binding = binding }
> else
>     -- 老 client 无此能力,走原有路径
> end
> ```
>
> 原因在 libxpkg `prelude.lua::import()`:未知模块返回 permissive proxy stub,
> 它的 `__index` 对任意 key 都返回一个 truthy 的可调用 table。老 client 没有
> `xim.libxpkg.subos`,`subos.env` 于是是个 truthy proxy —— recipe 走"新分支",
> 调用静默无效,装完什么也没发生。
>
> V2 spec 里 `if xvm.files then` 成立,只因为 `xvm` 是**已存在的模块**,老 client
> 上那个 *field* 才真的是 `nil`。缺失的 *module* 永远不是。stub 是带 `__call` 的
> **table**,真函数才是 `function`,`type()` 是唯一能区分两者的判据。
>
> 已由 e2e 用真实旧二进制钉住(见 §11)。规则已写进
> `xim-pkgindex/docs/V2/xpackage-spec.md`。

### 5.5 卸载路径

pkg `uninstall()` 无需显式清理 env——xlings 侧 uninstall 流程会按 binding 删除对应 envs key。

---

## 6. 复用 `xlings subos use`(不新增命令)

### 6.1 现有 CLI(不动)

```
xlings subos use <name>
    --global                Persist the active SubOS
    --shell [KIND]          Emit shell activation code
    --sandbox [BACKEND]     Enable sandbox
    --cmd <COMMAND>         Run one command
    --keep / --no-keep      Namespace keeper
    --ttl <SECONDS>         Keeper idle timeout
    --gpu                   Expose GPU devices
```

### 6.2 Slice 1 扩展:`--shell` 输出内容

现有 `--shell` 已经 emit shell 激活源码。**Slice 1 在其输出里追加 subos_info.envs 展开的 export 语句**:

Bash 输出示例:
```bash
# 现有 xvm binding 相关 export(不动)
export PATH="..."

# Slice 1 新增:subos_info.envs 展开
export LIBGL_DRIVERS_PATH="/home/user/.xlings/subos/default/pkg/mesa-25.0.0/lib/dri"
export __EGL_VENDOR_LIBRARY_DIRS="/home/user/.xlings/subos/default/pkg/mesa-25.0.0/share/glvnd/egl_vendor.d"
export XDG_DATA_DIRS="/home/user/.xlings/subos/default/pkg/mesa-25.0.0/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
```

**注入规则**:
1. 读 subos_info.envs,按 binding 迭代
2. 按 §3.8 冲突规则汇总
3. 展开 §3.6 占位符
4. 探测目标 shell(bash / fish / zsh),emit 对应语法
5. **不覆盖用户已 export 的变量**——通过 `${VAR:+...}`(bash)、`$VAR`(fish)保持既有值

### 6.3 Slice 1 扩展:`--cmd` 环境注入

现有 `--cmd <COMMAND>` 已经在 subos 上下文里执行命令。**Slice 1 在 exec 前把 subos_info.envs 计算后的 env dict 注入进程**:

- 保留当前进程 env 作为基线
- 叠加 envs 段计算结果(用户已设的 env 优先,pkg 声明不覆盖)
- `execvpe(cmd, argv, effective_env)`——不启动 shell

### 6.4 Shell 覆盖(Slice 1 范围)

| Shell | Slice 1 支持 |
|---|---|
| bash | ✅ 主 emitter |
| zsh | ✅(与 bash 兼容语法) |
| fish | ✅(独立 emitter) |
| PowerShell/cmd | ⬜ 未来 |

用户可通过 `--shell [KIND]` 显式指定(已在现有 CLI)。

### 6.5 UC-2 输出实际生效清单

`--shell` stderr 输出实际注入的变量清单(不进 eval,只做提示):

```
[xlings] subos: default (runtime glibc@2.39)
[xlings] env: 3 vars from 1 packages
[xlings]   mesa: LIBGL_DRIVERS_PATH, __EGL_VENDOR_LIBRARY_DIRS, XDG_DATA_DIRS
```

避免用户 `eval` 后不清楚加了什么。

---

## 7. 卸载 + doctor 集成

### 7.1 pkg uninstall 路径

Xlings 侧 uninstall 流程新增一步:

```
uninstall(pkg):
    1. 调用 pkg uninstall() hook(既有)
    2. 从 xvm 撤销 binding(既有)
    3. [新增] 从 subos_info.envs 移除 <name>@<version> 为 key 的整段
    4. Atomic write .xlings.json
    5. 若移除后 envs 为空 → 保留 {}(遵循 I7)
```

### 7.2 doctor 检查项

| # | 检查 | 判据 | 修复动作 |
|---|---|---|---|
| D1 | Manifest 结构完整 | I1~I8 全成立 | 引导用户重建 subos |
| D2 | envs binding 与 xvm 一致 | 每个 envs key 对应 pkg 在 xvm 已装 | 移除孤儿 envs 段 or 提示重装 |
| D3 | envs value 占位符可解 | `${pkgdir}` 等指向目录存在 | 提示重装对应 pkg |
| D4 | envs 变量无冲突 | 同 var 无多个 `set` | warning(非 error) |
| D5 | runtime 引用一致 | subos_info.runtime 引用的 pkg 在 xvm 已装 | 提示装 runtime |

**Reporter 与 repairer 用同一函数**(memory: reporter/repairer predicate drift),不允许两处描述判据。

---

## 8. 契约条款

### 8.1 SH(可分享性)

- **SH-1** 值可迁移:`subos_info` 内所有 value 必须用占位符,禁止绝对路径、机器特定 ID
- **SH-2** envs 是 derived cache:pkg config() 是权威源;recreate 时重跑 config() 而非直接 apply

### 8.2 UC(用户覆盖权)

- **UC-1** 用户 env 优先:用户在 shell 里 export 的变量,activate/run 不覆盖
- **UC-2** 无静默选择:activate 输出实际生效清单到 stderr
- **UC-3** 遵循 v2 spec probe pattern:新字段/API 通过 probe 兼容老 client,不用 `min_xlings`

---

## 9. Slice 1 边界

### 9.1 已完成(不动)

- glibc pkg + elfpatch PT_INTERP/RPATH(Bootstrap)
- xvm.add / xvm.files provider-scoped 注册(Discovery)
- xvm shim + PATH(Discovery)
- `xlings subos new/list/remove/info/use/stop` 基础 CLI
- `~/.xlings.json` subos 注册表 + activeSubos 机制
- doctor 框架

### 9.2 Slice 1 新增(xlings 侧)

代码估算 ~800~1200 行:

| 组件 | 估算 |
|---|---|
| `subos_info` 读写 + schema 校验 | ~200 |
| `subos.env{}` xpkg API + provider-scoped 记账 | ~150 |
| `xlings subos use --shell` env emitter (bash/zsh/fish) | ~200 |
| `xlings subos use --cmd` env 注入 | ~100 |
| `xlings subos new --runtime` + 不变量校验 | ~150 |
| Uninstall envs 清理 | ~60 |
| Doctor D1~D5 检查项 | ~120 |
| 单元 + 集成测试 | ~250 |

### 9.3 Slice 1 新增(xim-pkgindex 侧)

- `mesa` recipe(独立设计,见 task #8~9)
- 一份预构建 tarball 上传 xlings-res
- dlopen 闭包一次性人工审计
- 一份 bwrap 空 host smoke test

### 9.4 明确不做(non-goals)

- Platform manifest / 可组合发行版
- 多 runtime(多 glibc / musl 并存)—— 生态阻塞项另议
- `capabilities_host` / `activation_hooks` 段
- `xlings subos export/import/diff/snapshot/migrate` CLI
- 用户 `--force` flag(UC-1 是原则,CLI 后续 slice 补)
- 自动 schema upgrade
- PowerShell / cmd shell 支持
- macOS / Windows 上的 runtime(仅数据模型预留,不实现)
- NVIDIA 闭源栈 payload(slice 2)

---

## 10. 开放问题

| # | 问题 | 阻塞谁 |
|---|---|---|
| O1 | `--shell fish` 与 bash 的语义等价 fixture 是否需要? | 测试 |
| O2 | Runtime 未装且用户拒绝 `--yes` 时的具体退出码语义 | subos new UX |
| O3 | Doctor D4(env 冲突)是 warning 还是 error?判据严格性统一 | doctor |
| O4 | ~~`mesa` payload 实际 size(可能超 800MB,是否分包)~~ **已关闭**:实测 241 MB(mesa 103 + LLVM 137),不分包 | ~~pkgindex 侧 slice 1~~ |
| O5 | subos 迁移/克隆场景下 `${pkgdir}` 是否需要在 subos_info 里持久化解析结果? | 未来 subos 迁移能力 |

不阻塞 slice 1 起步,施工时定。

---

## 11. 施工顺序建议

1. **Task #2 定案**:本 spec §3 schema 已定,可直接进入实施
2. **Task #4**:实现 `subos.env{}` API + 写入路径(2~3 天)
3. **Task #3**:`xlings subos new --runtime` + 不变量校验(2~3 天)
4. **Task #5**:`xlings subos use --shell` env emitter (bash 优先,~5 天)
5. **Task #6**:`xlings subos use --cmd` env 注入(2 天)
6. **Task #7**:uninstall 清理 + doctor D1~D5(3~5 天)
7. 并行 **Task #8~10**:pkgindex 侧 mesa(2~3 周)
8. 集成 + e2e smoke test(1 周)

**总估算 4~6 周**。

---

## 13. 施工实况(2026-08-05)

Slice 1 的 xlings + libxpkg 两侧已实现并验证。落地拆分见
`2026-08-05-subos-slice1-landing-plan.md`。

### 13.1 与本设计的三处偏离

| # | 设计原文 | 实际 | 原因 |
|---|---|---|---|
| 1 | 探针 `if subos.env then` | `type(subos.env) == "function"` | 前者恒真,见 §5.4 修正 |
| 2 | 冲突按装机顺序 | 按 binding 序 | 装机顺序不在 manifest 里,见 §3.8 修正 |
| 3 | (未提及)`default` subos | `self init` 也写 manifest | 它不走 `subos::create`,否则新能力在**所有人实际在用的那个 subos 上**全程静默无效,同时成为老 home 的迁移路径 |

### 13.2 设计未覆盖、施工中必须补的

- **早退分支**:`process_xvm_operations_` 在没有 xvm 注册项时会提前 return。
  只声明 env、不注册任何 xvm 节点的包会被静默丢弃 —— env 消费必须放在早退**之前**。
- **doctor 渲染器的 `default: break;`**:新 FindingKind 不加 case 就不打印,
  查得出、报不出。
- **卸载按包名而非精确 binding 匹配**:removal 会经命名空间和 group 成员解析版本,
  安装时记录的 binding 在卸载点无法可靠重建;同一包装过两个版本时会留下另一段。

### 13.3 覆盖

- 单测 `tests/unit/test_subos_manifest.cpp` — 29 例(schema/不变量/占位符/冲突/确定性)
- libxpkg `tests/test_executor.cpp` — 6 例(op 收集/校验/探针语义)
- E2E-60 `subos_env_declaration_test.sh` — 装→声明落盘→`--shell`→`--cmd`→用户覆盖→doctor→卸载清段
- E2E-61 `subos_env_probe_compat_test.sh` — 真实旧二进制上的双读数差分

### 13.4 仍未做

§9.4 全部照旧。§10 的 O1(fish/bash 等价 fixture)已由 E2E-60 覆盖 bash 侧,
fish emitter 只有构造正确性、无行为断言;O4 已由 `2026-08-05-graphics-stack-design.md`
实测关闭(241 MB,不分包);O5 仍开放。

mesa 与 NVIDIA 闭源栈的详细设计见
`2026-08-05-graphics-stack-design.md`。

---

## 12. 一句话总括

> **Slice 1 = 一个 `subos_info` 块 + 一个 `subos.env{}` API + `xlings subos use --shell/--cmd` 两处扩展。总代码 ~800~1200 行。补完 Configuration 基质,让 mesa 端到端可跑;所有更大的野心(platform、多 runtime、canonical build 等)明确挂到后续 slice,不阻塞当前工作。**
