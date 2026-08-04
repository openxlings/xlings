# subos slice 1 落地计划:任务拆分与跨仓依赖

**日期**: 2026-08-05
**类型**: 落地计划(landing plan)
**上游**: `2026-08-05-subos-minimum-design.md`(详细设计,schema/CLI 已定案)
**目标**: 把 slice 1 从设计变成已发布、可验证的能力

---

## 0. 设计文档的一处修正(施工前必须先改)

设计文档 §5.4 给出的老 client 探针是:

```lua
if subos.env then ... else ... end
```

**这个探针在所有 client 上恒为真,是错的。**

原因在 libxpkg 的 `prelude.lua::import()`:未知模块返回一个 permissive
proxy stub —— 它的 `__index` 对任意 key 都返回一个可调用的 proxy。老 client
没有 `xim.libxpkg.subos`,`import()` 于是返回 stub,`subos.env` 是一个 truthy
的 proxy table。recipe 会走"新路径",调用静默无效,装完什么也没发生。这正是
xlings 生态反复出现的 silent-success 形态。

V2 spec 里 `if xvm.files then` 之所以成立,是因为 `xvm` 是**已存在的模块**——
老 client 上 `xvm` 是真模块,`xvm.files` 才真的是 `nil`。**新模块不适用这个
写法。**

**新模块的探针必须判类型**:

```lua
if type(subos.env) == "function" then ... end
```

proxy 是带 `__call` 元方法的 **table**,真函数才是 `function`,两者可区分。

这条规则要写进 `xim-pkgindex/docs/V2/xpackage-spec.md`,并由一个跑真实旧
xlings 二进制的 e2e 断言(模板:`tests/e2e/xvm_files_probe_compat_test.sh`)。

---

## 1. 为什么必须动 libxpkg(而不是只动 xlings)

考虑过三条路,只有一条站得住:

| 方案 | 结论 |
|---|---|
| `subos.env` 放 xim-pkgindex 的 `libs/`(像 `xim.pkgindex.sysroot`) | ❌ index 是滚动更新的,模块在所有 client 上都存在,而消费 op 的 C++ 在 xlings 里。探针恒真 → silent-success |
| xlings 在执行 recipe 前往 `_LIBXPKG_MODULES` 注入模块 | ❌ 模块表在 libxpkg 的 `load_stdlib` 里写死,没有宿主注入口 |
| **放 libxpkg** | ✅ Lua 函数与消费它的 C++ 静态链接进同一个 xlings 二进制,"函数在不在"就等于"这个 client 支不支持"。单一真相源 |

所以 **libxpkg 0.0.48 是硬前置**,不是可选项。

---

## 2. 跨仓依赖图

```
┌─ libxpkg ─────────────────────────────────────────────┐
│  #12 subos.lua + XvmOp{var,value,mode} + 模块注册 + 测试 │
└───────────────┬───────────────────────┬───────────────┘
                │                       │
   ┌────────────▼──────────┐   ┌────────▼────────────────┐
   │ #13 发布链(串行)      │   │ #14 本地 registry 播种   │
   │  bump 0.0.48 → merge  │   │  ~/.mcpp/registry/data/  │
   │  → tag → gtc 镜像     │   │  …/0.0.48 + .mcpp_ok     │
   │  → mcpp-index 条目    │   │  (让 xlings 侧立刻能编)  │
   │  → publish-artifact   │   └────────┬────────────────┘
   └────────────┬──────────┘            │
                │              ┌────────▼──────────────────┐
                │              │ #15 manifest.cppm 核心    │
                │              │  schema/不变量/占位符/合并 │
                │              └────────┬──────────────────┘
                │                       │
                │        ┌──────┬───────┴──────┬──────────┐
                │        ▼      ▼              ▼          ▼
                │      #16    #17            #18        #19
                │   new       install/       use        doctor
                │   --runtime uninstall      --shell    D1–D5
                │             env ops        --cmd
                │        └──────┴───────┬──────┴──────────┘
                │                       ▼
                │                  #20 测试(unit + e2e)
                │                       │
   #21 规范/文档(可全程并行)──────────┤
                │                       │
                └───────────┬───────────┘
                            ▼
                   #22 版本号 + 单 PR + CI 全绿
                            ▼
                   #23 release + gtc 补 gitcode 资源
                            ▼
                   #24 xlings subos 真实验证
```

**并行窗口**:#14 一旦完成,#15→#16/#17/#18/#19 与 #13 的发布链完全并行。
#21 全程可并行。真正的串行瓶颈只有 #12 → #14 → #15 → 四路扇出 → #20 → #22。

---

## 3. 各仓改动面

### 3.1 openxlings/libxpkg(#12, #13)

| 文件 | 改动 |
|---|---|
| `src/lua-stdlib/xim/libxpkg/subos.lua` | 新增。`M.env{var, op, value, binding}` |
| `src/xpkg-executor.cppm` | `XvmOp` 加 `var` / `value` / `mode`;`xvm_operations()` 读取;`load_stdlib` 注册 `subos` 模块 |
| `xmake.lua` | embed 列表加 `subos_lua` |
| `src/xpkg-lua-stdlib.cppm` | 由 xmake 从 `.lua` **自动生成**,不手改 |
| `tests/test_executor.cpp` | subos_env op 收集用例 |
| `mcpp.toml` | `0.0.47` → `0.0.48` |

**命名冲突处理**:用户面参数是 `op = "set"|"prepend"`,而 `XvmOp.op` 已经
表示 op 类别。内部 entry 用 `op = "subos_env"` + `mode = "set"|"prepend"`,
用户面不变。

### 3.2 openxlings/xlings(#14–#20, #22, #23)

| 文件 | 改动 |
|---|---|
| `src/core/subos/manifest.cppm` | **新增**,slice 1 的地基 |
| `src/core/subos.cppm` | `create()` 写 subos_info;`run()` 解析 `--runtime`;`use_emit_shell` / `use_spawn_shell` 注入 env |
| `src/core/xim/installer.cppm` | 消费 `subos_env` op;uninstall 按 binding 清段 |
| `src/core/xself/doctor.cppm` | D1–D5 |
| `src/cli/spec.cppm` | `subos new --runtime` 帮助文本 |
| `mcpp.toml` | 版本号 + `mcpplibs.xpkg = "0.0.48"` |
| `tests/unit/`、`tests/e2e/` | 见 #20 |

### 3.3 openxlings/xim-pkgindex(#21, #24)

| 文件 | 改动 |
|---|---|
| `docs/V2/xpackage-spec.md` | `subos.env` API + **新模块探针规则** |
| 验证载体 recipe | #24 需要一个真实声明 env 的包 |

### 3.4 mcpplibs/mcpp-index(#13 的一环)

`pkgs/x/xpkg.lua` 加 0.0.48 条目,GLOBAL + CN 双 URL + sha256,**三个平台块都要写**。

---

## 4. 已知的坑(来自既往教训,不是推测)

1. **`bump-index` 和 `mirror-binaries` 都吞掉自己的失败**(结尾 `|| echo "…(non-blocking)"`)。两个都可能报绿而什么都没干。#23 必须查产物:xlings-res 的版本条目 + `latest.ref`,以及用 **GET(不是 HEAD)** 验证 gitcode 资源。
2. **mcpp index 是 artifact 不是 git clone**。合并到 mcpp-index 后要等 `publish-artifact.yml`,客户端还有 TTL —— `rm -rf ~/.mcpp/registry/data/<ns>` 强制刷新。手改缓存里的 `pkgs/**` 无效。
3. **`mcpp build` 不重建测试二进制,只有 `mcpp test` 会**。改完测试跑 `mcpp build` 会留下过期 `test_main`,红相位可能假绿。
4. **libxpkg CI 会静默腐烂**,失败读起来像网络错误(`fetch 'mcpplibs.capi.lua@0.0.3' failed`)。`test_executor` 另有 4 个既存 elfpatch 失败,CI 用 `--gtest_filter=-ExecutorTest.ApplyElfpatchAuto_*` 过滤。
5. **隔离 home 测试不要预置 `data/`** —— 会让 index 变成 symlink,差分断言变得不可证伪。
6. **绝不写入被 flock 的文件**:rename 换掉 inode 会静默破坏锁。subos_info 的原子写要与 home config 的锁路径区分清楚。
7. **`quick_install` 忽略 `XLINGS_HOME`**,验证 release 要用 tarball 自带的 `self install`。

---

## 5. 本次落地的范围边界

**做**:libxpkg 0.0.48 + xlings slice 1 全部(设计文档 §9.2)+ 规范文档 + 发布 + 真实验证。

**不做**(设计文档 §9.4 已明确,外加):
- `compat.mesa` payload 本身(#8–#10,设计文档 §11 估 2–3 周,独立并行轨)
- 多 runtime 并存
- platform 抽象

#24 的验证载体因此不是 compat.mesa,而是一个小体量的真实 recipe —— 目的是证明
**机制**端到端通,payload 规模是另一件事。

---

## 6. 完成判据

- [x] libxpkg 0.0.48 已发布,mcpp-index 可解析,gitcode 资源 GET 可下载
- [x] xlings 单 PR 含全部 slice 1 实现 + 测试 + 版本号,CI 全绿
- [x] 新老 client 探针差分测试通过(老二进制走 legacy 分支且不 silent-success)
- [x] release 产物已验证:版本条目、`latest.ref`、gitcode 资源三者一致
- [x] `xlings subos` 真实跑通:new → install → use → env 生效 → uninstall 清段 → doctor 干净

---

## 7. 落地结果(2026-08-05)

| 仓库 | 产出 | 状态 |
|---|---|---|
| openxlings/libxpkg | #32 → tag `0.0.48` → gitcode `mcpp-res/xpkg` | 已合并发布 |
| mcpplibs/mcpp-index | #153(三个平台块 + CN 镜像同源校验) | 已合并,artifact 已发布 |
| openxlings/xlings | #480 → **2026.8.5.1** | 已合并、已发布 |
| openxlings/xim-pkgindex | #496(V2 spec:API + 新模块探针规则)、#497(latest 前移) | 均已合并 |

### 7.1 计划外的两处修正

**xmake 全面退场(libxpkg)**。施工中发现 `xvm.files` 在 0.0.47 只改了生成物
`xpkg-lua-stdlib.cppm`,源 `xvm.lua` 从未有过 —— 任何人跑一次 `xmake build`,
before_build 就会用陈旧源重新生成、**静默抹掉该特性**。按用户要求改为 mcpp 单一
构建入口:`build.mcpp` 在构建期把 Lua stdlib 生成到 `MCPP_OUT_DIR`,生成物不再进
版本库,两份拷贝对不上这种状态在结构上不再可能。同时清掉 `xmake.lua`、
`CMakeLists.txt`(`mcpplibs-templates` 模板残留)与 xmake 版 `release.yml`,并把
CI 钉子刷新到 xlings 2026.8.4.2 / mcpp 2026.8.4.1。

**CI 缓存 BMI 中毒**。依赖版本变更(非仅版本号)会命中 `restore-keys` 回退,
拿回另一套依赖图下构建的 BMI,报 `import 'std' has CRC mismatch`;失败的运行**还会
把中毒状态按新精确键存回**,使后续所有运行精确命中它。修法两半缺一不可:回退时丢弃
BMI(保留 payload)+ 作废 `mcpp-` 键空间为 `mcpp-v2-`。详见
`project_ci_mcpp_cache_bmi_poisoning` 记忆。

### 7.2 真实硬件验证

发布版二进制 → 隔离 home → `subos new gfx --runtime glibc@2.39` → 从生产索引装入
175 条 workspace → 一个声明 `LIBGL_DRIVERS_PATH` / `XDG_DATA_DIRS` 的包 →
**Godot 在 RTX 4080 上开真实窗口渲染,并从自身进程环境读到这两个变量**
(`prepend` 与宿主既有 `XDG_DATA_DIRS` 拼接而非替换)→ 卸载后 `envs: {}`、变量消失、
doctor 干净。

这正是 compat.mesa 将依赖的机制:GL 发现变量到达了一个 xlings 并不包装的进程。

### 7.3 仍未做

设计文档 §9.4 全部照旧。本机是 NVIDIA 闭源栈(RTX 4080),按 §9.4 属 slice 2;
compat.mesa payload(#8~#10)仍是独立并行轨。
