# #524 / #525 实施计划 —— 任务拆分、依赖关系、跨仓库协作

> 设计与根因：`.agents/docs/2026-08-10-issue-524-525-root-cause-and-fix-design.md`
>
> 日期：2026-08-10 ｜ 决策见该文档 §5（D1–D5）
>
> 仓库：`openxlings/libxpkg`、`openxlings/xlings`、`openxlings/xim-pkgindex`
> （+ `mcpplibs/mcpp-index` 与 gitcode 镜像作为发布链的中转）

---

## 1. 八个角度的取舍

这一轮改的是**两条被当成契约的错误前提**，所以每个角度的判据都是「契约现在由谁承担」。

**架构。** 两条 issue 的共同点是**把责任放在了拿不到信息的一侧**。
`dep_install_dir` 要求调用方重述解析器已经知道的版本；GLX 可达性要求消费者传一个它不知道要传的链接参数。
两处都改成**由掌握信息的一侧承担**：`resolved_deps` 自己判定唯一性；`libGLX.so.0` 自己带搜索路径。
E1 进一步把 vendor 目录放进 libglvnd 的 payload，用 `$ORIGIN` 表达——**位置信息由文件自身携带，不由环境表达**。

**稳定性。** 三条防线，都在缺陷发生的那一层：
libxpkg 侧的 Lua 单测（今天是 0，本轮补齐；0.0.55 的 593 行新测试全在 C++ executor 面，改的却是 Lua）；
index 侧的结构化 lint（调用形状与声明必须一致）；
xlings 侧的**冷** home e2e（热 home 跳过安装、根本不跑 config hook，这正是 CI 看不见 #524 的原因）。

**优雅简洁。** Fix A 是**净删除**一个判据（`_is_exact_store_version` 前置），不是新增分支——
唯一性守卫本来就在，它一直在做正确的事。
Fix E1 与已有的 `declare_egl_vendor()` 完全对称，`GLX_VENDOR_DIR` 与 `EGL_VENDOR_DIR` 并列，不引入新概念。

**用户体验。** 目标是**没有失败可看**。退而求其次，失败必须指向可执行的下一步：
撞名时点名撞了谁（而不是「找不到」），GLX 不可达时说的是哪一个 vendor 目录空着。
顺手删掉 `gcc.lua:545` 那条硬编码的 `xim:glibc@2.39` —— 它把 issue 报告人引到了一条假线索上。

**兼容性。** Fix A **只放宽不收紧**：0.0.55/0.0.56 能解析的，之后全部照旧能解析；
歧义依旧失败关闭。没有 roots 字段的老客户端走原来的 scan 路径，完全不变。
E1 只往 libglvnd payload 里**加**一个目录、往 `libGLX.so.0` 上**加**一条 RPATH，
没装 graphics 栈的 home 里那个目录是空的，`dlopen` 照旧落到原来的路径上。

**跨平台。** `dep_install_dir` 是全平台的，Fix A 的改动不含任何平台分支。
E1 是 **linux-only** 的（glvnd 只在 linux），必须用平台条件包住，
不能出现「macOS/Windows 上多出一个空目录 + 一次 patchelf 失败」。
lint 与 e2e 要按平台枚举 recipe 的 `xpm.<platform>` 段，不能只看 linux 那一段。

**一致性。** 「一个问题一个回答者」：`resolved_deps` 是依赖坐标的唯一回答者，
`tool_payload_dir` 是 libxpkg 自身工具的唯一回答者（clangd 的 `llvm-tools` 归它，不归前者）；
GLX vendor 目录是 GLX vendor 可达性的唯一回答者。
godot 不再问一个它没声明的包——**要用就要声明**。

**无感升级。** 这是硬约束：**已经装好的 home 升级后不能变红、不能要求重装**。
- Fix A 纯放宽，老 home 的任何已装包不受影响。
- E1 的 RPATH 写在 libglvnd 的 config hook 里，只有**安装/重装 libglvnd 时**才执行。
  存量 home 里 `libGLX.so.0` 还是老的 RPATH → GLX 依旧不可达 → **这就是要解决的那个问题本身**。
  所以 libglvnd 必须**换版本键**（同 artifact、新版本），让消费者的 `>=` 下界把它拉起来重跑 config，
  这是 fontconfig 2.15.0.1 用过的模式。**不这样做，存量 home 拿不到修复，而且没有任何提示。**
  → 落到 **T7 的验收里，必须在一个「已装老 graphics 栈」的 home 上验证升级路径。**

---

## 2. 任务 DAG

```
                  ┌───────────────────────────────────────────┐
                  │ T1  同步/分支/设计doc/计划  (done)          │
                  └───────────────┬───────────────────────────┘
          ┌───────────────┬───────┴────────┬──────────────┬─────────────┐
          ▼               ▼                ▼              ▼             ▼
  ┌───────────────┐ ┌──────────┐  ┌──────────────┐ ┌──────────┐ ┌──────────┐
  │T2 libxpkg     │ │T5 index  │  │T7 index E1   │ │T8 index  │ │T10 xlings│
  │   Fix A       │ │  Fix B   │  │   GLX vendor │ │  Fix F   │ │  e2e+tool│
  └───────┬───────┘ ├──────────┤  └──────┬───────┘ │  probes  │ │  +docs   │
  ┌───────▼───────┐ │T6 index  │         │         └────┬─────┘ └────┬─────┘
  │T3 libxpkg     │ │  Fix C   │         │              │            │
  │   Lua tests   │ ├──────────┤         └──────────────┘            │
  └───────┬───────┘ │T9 index  │            F 必须先红、E1 后绿        │
  ┌───────▼───────┐ │  lint    │                                      │
  │T4 libxpkg     │ └──────────┘                                      │
  │   0.0.56 PR   │                                                   │
  └───────┬───────┘                                                   │
          │                                                           │
          └──────────────────────┬────────────────────────────────────┘
                                 ▼
                    ┌────────────────────────┐
                    │T11 xlings bump+PR+CI    │
                    └────────────┬───────────┘
                                 ▼
                    ┌────────────────────────┐
                    │T12 发布链               │
                    │ libxpkg tag → gtc →     │
                    │ mcpp-index → xlings rel │
                    └────────────┬───────────┘
                                 ▼
                    ┌────────────────────────┐
                    │T13 subos --sandbox 实测 │
                    └────────────────────────┘
```

**可并行的**：T2/T3（libxpkg）与 T5/T6/T7/T8/T9（index）与 T10（xlings）三条线互不阻塞。
**真串行的只有三处**：T4→T11（xlings 要消费已发布的 libxpkg）、T11→T12（发布链有严格顺序）、T12→T13（对着发布产物验）。

**index 侧的三条 PR 内顺序**（同一 PR，但提交顺序有意义）：
T8（探针去掉 `--disable-new-dtags`）先落 → **它会红** → T7（E1）再落 → **它转绿**。
这个红→绿是本轮唯一能证明「修的就是这个缺陷」的判据；顺序反了就只剩一句自我声明。

---

## 3. 跨仓库发布顺序（唯一可行的那个）

```
openxlings/libxpkg    PR 合并 → git tag 0.0.56 → GitHub 自动 tarball
        ↓
gitcode 镜像          gtc release publish（字节相同的副本，只改名）
        ↓             ⚠ 校验用 GET，不能用 HEAD（HEAD 401 / GET 302→200）
mcpplibs/mcpp-index   pkgs/x/xpkg.lua —— GLOBAL + CN URL + sha256，
        ↓             三个平台段（linux/macosx/windows）都要写
        ↓             等 publish-artifact.yml 跑完；客户端有 TTL，
        ↓             `rm -rf ~/.mcpp/registry/data/mcpplibs` 强制刷新
openxlings/xlings     mcpp.toml/mcpp.lock 依赖 bump → 版本号 bump → PR → CI → release
        ↓             ⚠ bump-index 与 mirror-binaries 都吞自己的失败，
        ↓             永远不要读成绿：直接查 latest.ref 和 gitcode 资源
openxlings/xim-pkgindex  recipes 合并（Fix B/C/E1/F/lint）
        ↓             ⚠ index 是构建产物不是 git clone，合并后 CI 仍会取到旧 index
mcpp-community/mcpp   把 xlings pin 从 2026.8.9.2 提回（由 mcpp 侧执行）
```

**index 侧为什么放在 xlings 之后**：Fix B/C 依赖 Fix A 才生效；先合 index、后发 xlings，
中间窗口里 index 已经改成 namespaced 坐标而客户端还是老 libxpkg —— 那条路径本来就是通的（对照行已证），
所以**这个顺序是安全的**，但反过来（先发 xlings 后合 index）窗口更短，优先。

---

## 4. 验收（全部可证伪，判据见设计文档 §4）

| # | 归属 | 判据 |
|---|---|---|
| A1/A2 | T3 | `repro-dep-install-dir.sh` ≥5/7 ok；撞名 nil **且**点名双方 |
| A3/A4 | T13 | 冷 home `xlings install -y xim:gcc@16.1.0` 退出 0 且注册 3 个 shim；`meson` 同样 |
| A5 | T7/T13 | `readelf -d libGLX.so.0` 含 vendor RPATH；**且** integrity 清理没删掉跨包写入的符号链接 |
| A6 | T13 | imgui 模板**默认 dtags** 构建后出窗口；判据是 `/proc/self/maps` 里 GL 对象的路径，**不是** `glxinfo` 的 renderer 字符串 |
| A7 | T8→T7 | `verify-stack.sh` 去掉 `--disable-new-dtags` 后：E1 之前**红**，之后**绿** |
| A8 | T7 | **存量 home 升级路径**：已装老 graphics 栈的 home，升级后 GLX 可达（靠 libglvnd 换版本键 + 消费者 `>=` 下界） |

A7 与 A8 是这一轮最容易被跳过、也最能说明问题的两条。
