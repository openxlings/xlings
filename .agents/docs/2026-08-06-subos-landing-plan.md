# subos 架构方案:落地计划与跨仓库依赖

日期:2026-08-06
输入:`.agents/docs/2026-08-06-subos-architecture-proposal.md`(§7 的 AD-1~AD-14 已 review 通过)
性质:执行计划。提案说"做什么和为什么",这份说"谁先谁后、在哪个仓库、怎么验"。

---

## 0. 四个仓库,一条发布链

改动落在四个仓库,顺序不可换——下游的东西不存在时上游的代码没法测:

```
openxlings/libxpkg          Lua stdlib(recipe 侧能力)+ C++ 执行器
  0.0.50 → 0.0.51                 A3 · #42-generic · O4-scan · B1
        │
        │  merge → git tag → GitHub tarball
        │  gtc release publish mcpp-res/xpkg(CN 镜像,字节相同)
        │  mcpplibs/mcpp-index pkgs/x/xpkg.lua(GLOBAL+CN+sha256 ×3 平台)
        │  publish-artifact.yml → xlings-res/mcpp-index
        ▼
openxlings/xlings           客户端(沙箱/subos/doctor/env 层)
  2026.8.5.3 → 2026.8.6.1         A4 · E1/E2 · C1/C2/C3 · B5 · O2/O3 · AD-13 · AD-2/AD-9
        │                         + mcpp.toml: xpkg 0.0.50 → 0.0.51
        │  release.yml → GitHub release + gtc 补 gitcode 资源
        ▼
openxlings/xim-pkgindex     recipe + 规范 + 构建流水线
        │                         #42-glibc · A1/A2/B3 · AD-12 · B2/B2' · AD-11-build · B4-build
        ▼
真实验证                     隔离 XLINGS_HOME + xlings subos
```

**关键约束**(来自 `project_ecosystem_release_chain`):

- libxpkg 的 mcpp-index 条目发布后有 artifact TTL,`rm -rf ~/.mcpp/registry/data/mcpplibs` 强制刷新;
- xlings 的 `release.yml` 里 `bump-index` 与 `mirror-binaries` **都会吞掉自己的失败**,发布后必须用 GET(不是 HEAD)核对 gitcode 资源,并核对 `latest.ref`;
- 改 `.xlings.json` 的 mcpp pin 要同步 6 个 workflow 的 `XIM_PKGINDEX_REF`。

---

## 1. 任务分解与依赖图

编号沿用提案。`⊘` 表示被门禁挡住。

```
 ── 第一批:无前置,四条并行 ───────────────────────────────────────

 [L1] libxpkg  A3        _find_tool 走 payload                    ─┐
 [L2] libxpkg  #42-gen   elfpatch.relocate_build_paths(通用重定位) ─┤
 [X1] xlings   A4        locate_proot_ 去掉 PATH 步骤              ─┤ 互不依赖
 [X2] xlings   E1/E2     隔离 home 成测试默认 + 断言写契约          ─┤
 [P1] index    A1/A2/B3  R1–R7 写进 xpackage-spec.md               ─┘

 ── 第二批:依赖第一批的产物 ──────────────────────────────────────

 [L2] ──→ [P2] index  #42-glibc   glibc recipe 改用通用重定位
                          └─ 需要 libxpkg 0.0.51 已发布

 [X3] xlings C3  doctor 报双绑定  ──→ [X4] C1 单版本执行点 ──→ [X5] C2 envs 派生
        顺序不可换(§3.4):先能报告,再改行为

 [X6] xlings B5  护栏扩到 __EGL_VENDOR_LIBRARY_DIRS / LIBGL_DRIVERS_PATH
 [X7] xlings O2/O3  安装报三个数 + host-link 解析结果持久化
 [X8] xlings AD-13  驱动耦合提示(doctor + 安装 host-link 时一次)
 [X9] xlings AD-2/AD-9  refcount 强删告警

 ── B 线:门禁在前 ────────────────────────────────────────────────

 [V]  §2.7 四项验证  GLX / Vulkan ICD / dlsym 语义 / stub 分发
        │  ← AD-14 的直接应用:未验完不写代码
        ▼
 [P3] index   AD-12  interposer stub 作为索引包
        ▼
 [L3] libxpkg B1     elfpatch.host_link_interposer
        ▼
 [P4] index   B2     nvidia-gl-host-link 切换,删 xlings-deps
 [P5] index   B2'    libcuda-host-link 用同一能力

 ── 需要重新构建载荷,本轮只改流水线 ──────────────────────────────

 [P6] index  AD-11-build  build-glibc.sh 换占位前缀
 [P7] index  B4-build     mesa/libglvnd 把 vendor/DRI 目录编进产物
        两者都只在**下一次构建**生效,不改变现有 tarball

 ── 第三批:依赖 R7 的枚举能力 ────────────────────────────────────

 [L4] libxpkg O4-scan  DT_NEEDED 传递闭包枚举
        ▼
 [P8] index   O4       host_deps 显式清单 + 安装期闭包断言
```

### 1.1 为什么这个顺序

| 边 | 理由 |
|---|---|
| L2 → P2 | recipe 调用的函数必须先存在于已发布的 libxpkg |
| X3 → X4 | 现有 home 里已有双绑定(prodhome 就是)。先能报告再改行为,否则用户遇到"突然开始替换"而无从解释(§3.4) |
| X4 → X5 | `envs` 从绑定集合派生,前提是绑定集合已经是"恰好一个" |
| V → P3 → L3 | AD-14:手写清单不通过 R7。没验证过 GLX/Vulkan 就写 B1,等于再造一张手写表 |
| L4 → P8 | O4 的断言依赖闭包枚举,而闭包枚举本身是 R7 的可执行形式 |

### 1.2 本轮不做的

| 项 | 为什么 |
|---|---|
| AD-11 / B4 的**产物** | 需要重新构建 glibc / mesa / libglvnd 并重新发布 tarball,是独立的构建发布train。本轮只把构建脚本改对,下次构建生效 |
| #33 Intel 显卡、#34 vulkan-loader | 与本方案正交,不在提案范围 |

---

## 2. 单 PR 策略

目标是"尽量单 PR 全部实现",但发布链强制三个仓库分三个 PR——libxpkg 不发版,xlings 就没法用新能力。所以是**每仓库一个 PR**,三个 PR 一条线:

| 仓库 | PR | 版本 | 内容 |
|---|---|---|---|
| libxpkg | [#35](https://github.com/openxlings/libxpkg/pull/35) | 0.0.50 → 0.0.51 | L1 A3 · L2 relocate_build_paths |
| xlings | 本 PR | 2026.8.5.3 → 2026.8.6.1 | A3(第二个站点)· A4 · B5 · C1/C2/C3 · E1/E2 + xpkg pin 0.0.51 |
| mcpp-index | [#164](https://github.com/mcpplibs/mcpp-index/pull/164) | — | xpkg 0.0.51 条目(GLOBAL+CN+sha256 ×3 平台) |
| xim-pkgindex | [#522](https://github.com/openxlings/xim-pkgindex/pull/522) | (无版本号) | #42-glibc · A1/A2/B3 规范 · AD-11 构建前缀 |

**B 线(B1/B2/AD-12)不在本轮。** §2.7 的四项门禁 2026-08-06 全部通过,结论记录在提案
§2.7,但实现需要图形栈装好才能端到端验证,而本机的 home 里 mesa / libglvnd /
nvidia-gl-host-link 都没装。见任务 #55。

三个 PR 都带完整测试,CI 各自全绿后按链顺序合并、发布。

---

## 3. 验证策略

### 3.1 每一项的验收判据

判据必须是**可执行的**,不是"看起来对了"。逐项:

| 项 | 判据 |
|---|---|
| A3 | 在一个**同时存在** payload patchelf 与 `/usr/bin/patchelf` 的 home 上安装包,`_find_tool` 解析到 payload。宿主候选被用时必须打印一行 |
| #42 | 安装后 `bash -n` 过每个被改写的 shell 脚本;载荷里 `grep -r` 不到构建机路径 |
| A4 | PATH 上放一个假 proot,沙箱仍拒绝它并给出 `xlings install proot` 提示 |
| E1/E2 | 测试 home 在与 `$HOME` 无共同前缀的路径下;S8 断言的是契约不是实现 |
| C3 | 在 prodhome 的副本(双 mesa)上 doctor 报告;`--fix` 与报告共用同一谓词 |
| C1 | 装 `pkg@B` 到已有 `pkg@A` 的 subos,A 解绑、B 绑定、store 里 A 还在 |
| B5 | 声明 `__EGL_VENDOR_LIBRARY_DIRS` 指向我们载荷 → 安装期报告 |
| O2 | host-link 安装结束打印三个数,且三数之和 = 闭包大小 |
| B1/B2 | 端到端:`LD_LIBRARY_PATH` 上只有宿主驱动目录,`GL_RENDERER` 仍是 NVIDIA |

### 3.2 最终真实验证

`.agents/tools/verify-release-lifecycle.sh --bin <released xlings>`。

在隔离 `XLINGS_HOME` 下,用**发布产物**(不是 dev build)跑完整生命周期,并断言两件
一般测试不断言的事:

1. **被测 home 不是开发者的**,且与 `$HOME` 无共同前缀。shim 会把 `XLINGS_HOME` 改写
   成拥有它的那个 home,所以"我们跑在隔离 home 里"是一个需要**核对**的断言,不是前提。
2. **真实 `~/.xlings` 事后逐字节未变**。不是"我们没打算动它",是查过。

做成脚本而不是清单,是因为手工执行的清单只测一次;8-06 那四个 home 缺陷里有三个,
是靠同一测量做了两遍、结果不一致才发现的。

---

## 4. 风险与回退

| 风险 | 触发信号 | 回退 |
|---|---|---|
| A3 改变了 patchelf 的身份,产物 RPATH 形态变化 | 同源断言开始报错 | payload 优先可以用一个环境变量关掉,回到旧顺序 |
| #42 的枚举式重写误伤二进制文件 | `bash -n` 或同源断言失败 | 重写只作用于文本文件,二进制走 patchelf |
| C1 的替换语义打破现有用户的并存假设 | doctor 在升级后大面积报告 | C3 先上线一个版本,C1 在下一个版本 |
| B1 的 DT_RPATH 被 glibc 移除 | 未来 glibc 报错 | AD-7 的 wrapper 方案 |
| 发布链两个 job 吞掉失败 | 无信号——必须主动查 | 发布后按 §0 的三项核对 |
