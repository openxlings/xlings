# 2026.8.6.1 未做的四项:推进方案

日期:2026-08-06
输入:`2026-08-06-subos-architecture-proposal.md` 的落地(2026.8.6.1 / libxpkg 0.0.51)
性质:执行方案。每项给出**已测得的事实 → 为什么当时没做 → 方案 → 验收判据 → 风险**。

---

## 0. 这四项各自卡在哪一层

它们不是同一类工作,把它们放在一起是因为**卡住的理由不同**,而理由决定了推进方式:

| | 卡在 | 推进的第一步是 |
|---|---|---|
| **#55** B 线 | 缺**环境**(图形栈没装) | 装环境,不是写代码 |
| **#56** BMI 缓存 | 缺**诊断**(不知道为什么) | 做实验,不是改配置 |
| **#57** bump-index | 缺**决策**(补齐 vs 只保最新) | 定口径,再改脚本 |
| **#53** doctor 退出码 | 缺**影响面数据**(会让多少 home 变红) | 先量,再打开 |

四项里只有 **#55 是新功能**,其余三项都是**已经存在的机制没有闭合**——和本轮修的东西同一性质。

---

## 1. #56 —— BMI 缓存:先诊断,不要再猜

### 1.1 已测得的事实

BMI 存在 `~/.mcpp/bmi/<hash>/`,本机 **1270 个** hash 目录。每个目录里的
`std-module.json` 记录了这个 hash 覆盖什么:

```json
{ "compiler": "gcc", "compiler_version": "16.1.0", "cpp_standard": "c++26",
  "driver_identity": "2574ee8db660ec48",
  "std_build_commands": ["… --sysroot='…/subos/default' -B'…/binutils/2.42/bin' …"],
  "std_module_source_hash": "4aa4b9684b9cf219",
  "stdlib_version": "16.1.0", "target_triple": "x86_64-linux-gnu" }
```

**所以"hash 不够细"这个假设已经被排除**:编译器、版本、`-std`、driver 身份、
sysroot、binutils 路径、目标三元组、stdlib 版本、std 源码 hash 全都在里面。

另一条事实来自 CI 本身,是本轮唯一的硬证据:

```
387ff00   "Drop stale BMIs …"  success   → 构建通过
ee9300a   "Drop stale BMIs …"  skipped   → import 'std' has CRC mismatch
```

同一个 workflow、同一个 key、同一个 hash,中间只改了测试和文档。**唯一变量是
BMI 有没有被删。**

### 1.2 为什么当时没做

发布在等。当时能做的正确动作是**让它一定不出错**(无条件删),而不是**让它一定
不慢**。这个取舍是清醒的,但它是用 CI 时间换正确性:六个 workflow 各从 ~10 分钟
变 ~25 分钟。

### 1.3 三个候选假设,以及区分它们的实验

不要再加假设。设计一个能**同时证伪**三者的实验:

| 假设 | 如果成立,现象是 |
|---|---|
| **H1 保存的就是不自洽的集合** ——`xlings-ci-linux` 一个 job 里构建两次(dev gcc16 单测 + linux_release musl gcc15),两次写同一个 BMI 存储 | 本地"构建 dev → 构建 release → 再构建 dev"就能复现,**不需要经过缓存** |
| **H2 缓存往返破坏了元数据** ——mtime / 权限 / 硬链接在 tar 往返后改变,mcpp 的新鲜度判定因此重建了一部分单元、复用了另一部分 | 本地 tar/untar 往返能复现,而 H1 的双构建不能 |
| **H3 跨 job 污染** ——`~/.mcpp` 被别的 job 写过 | 单 job 内怎么折腾都复现不了 |

**实验(本机可做,不需要 CI):**

```bash
# 基线:干净构建
rm -rf ~/.mcpp/bmi ~/.mcpp/build-cache && mcpp build && echo BASELINE_OK

# H1:同一存储里先后跑两个工具链
mcpp build                                   # dev, gcc 16
mcpp build --target x86_64-linux-musl        # release, gcc 15 musl
mcpp build                                   # dev 再来一次 —— 挂了就是 H1

# H2:精确复制 actions/cache 的往返方式
tar --posix -cf /tmp/bmi.tar --use-compress-program zstdmt ~/.mcpp/bmi .mcpp 2>/dev/null
rm -rf ~/.mcpp/bmi .mcpp/bmi
tar -xf /tmp/bmi.tar -C /                    # 与 actions/cache 一致
mcpp build                                   # 挂了就是 H2
```

**先跑 H1**,因为它最便宜且最可能:`xlings-ci-linux` 确实在一个 job 里构建两次,
而 `mcpp.toml` 同时声明了 dev 工具链与 release target。

### 1.4 按结论分叉

- **H1 成立** → 真修复是让 BMI 存储按 target 分区(或让 mcpp 在切 target 时不
  复用),CI 可以恢复缓存 BMI,六个 workflow 各拿回 ~15 分钟。**这是上游
  (mcpp)的问题**,xlings 侧的临时办法是把两次构建拆成两个 job。
- **H2 成立** → 在恢复后按依赖序 `touch`,或改用 `actions/cache` 的
  `enableCrossOsArchive: false` + 显式 `--atime-preserve`。
- **H3 成立** → 回到 per-workflow key(已做),问题应当已经消失,需要重新解释
  为什么还在。
- **三者都不成立** → 记录实验结果,保持无条件删除,并把这份记录写进
  `project-ci-mcpp-cache-bmi-poisoning`,让下一个人不必重走。

### 1.5 判据

> 能用一条本机命令序列**按需重现** `import 'std' has CRC mismatch`。
> 在此之前,任何"修复"都只是又一次换前缀。

这条判据本身来自本轮的教训:三次前缀退役之所以看起来成立,正是因为**没有人能
按需重现它**。

### 1.6 风险

低。实验只在本机,失败也只是没结论。真正的风险是**不做**:CI 慢 2.5 倍会让所有
后续工作(尤其 #55 的 B 线,要反复迭代)付出复利成本。

---

## 2. #57 —— bump-index:分支名带版本号,所以"幂等"从不生效

### 2.1 已测得的事实

`tools/bump_index.sh:27`:

```bash
BRANCH="bump/${PROJ}-${VER}"
```

而同一个脚本第 91 行写着:

```bash
# Open PR — idempotent: a force-push updates an already-open PR in place.
```

**这句注释只在"同一版本重跑"时为真。** 分支名带版本号,所以跨版本永远是新分支、
新 PR。实际状态:

| PR | 分支 | 状态 |
|---|---|---|
| #503 | `bump/xlings-2026.8.5.2` | OPEN |
| #520 | `bump/xlings-2026.8.5.3` | OPEN |
| #524 | `bump/xlings-2026.8.6.1` | 本轮手动合并 |

三个堆在一起,`latest` 因此停在 **2026.8.5.1**——落后两版,而每次发布的
`bump-index` job 都报 success。

**更麻烦的一层**:每个 PR 的 diff 都是在 `latest = 2026.8.5.1` 时生成的,所以
**后合旧的会把 `latest` 设回去**。这不是理论——#503 现在合进去就会把指针从
2026.8.6.1 打回 2026.8.5.2。

第三条事实,决定方案是否有损:`version-check.py` 查的是
`api.github.com/repos/{owner}/{name}/releases/**latest**`(单数),**只追加一个
版本条目**。

### 2.2 为什么当时没做

发布链要先跑通。当时的正确动作是**只合最新的那个**并把陷阱记下来,而不是顺手
把三个都合了——那会让指针倒退。

### 2.3 方案:两步,顺序不可换

**第一步(结构):分支名去掉版本号。**

```bash
BRANCH="bump/${PROJ}"        # 不带 ${VER}
```

force-push 于是真正幂等:永远只有一个 open PR,永远是当前最新,**堆叠这个机制
本身消失**。这是 R3 意义上的修复——删掉回答者,而不是增加一条"合并前先检查顺序"
的规矩。

**第二步(决策):被跳过的版本条目要不要补?**

单分支方案**不会**补齐 8.5.2 / 8.5.3——`version-check.py` 只看 `releases/latest`。
两个口径:

| 口径 | 代价 | 适合 |
|---|---|---|
| **只保最新**(默认) | 用户不能 `xlings install xlings@2026.8.5.2` 精确装旧版 | 如果旧版精确安装不是需求 |
| **补齐全部** | `version-check.py` 改查 `releases` 复数,逐个比对并追加缺失条目 | 如果回滚到指定版本是需求 |

**这一条需要 sunrisepeak 定。** 我的建议是**补齐**:xlings 自身是包管理器,
"回滚到上一个能用的版本"是它必须支持自己的能力,而资产已经在 xlings-res 上,
只差索引条目。

**第三步(可选):green 即自动合并。** `gh pr merge --auto --squash`。这让
"只开 PR 不合"这个半成品状态消失。但它把发布的最后一环交给自动化,需要
sunrisepeak 判断是否接受。

### 2.4 判据

1. 连续发两个版本,`xim-pkgindex` 上**始终只有一个** open 的 bump PR;
2. 合并它之后 `latest` 等于**较新**的那个版本,不论合并顺序;
3.(若选补齐)索引里能查到区间内**每一个**已发布版本的条目。

### 2.5 收尾

#503 / #520 **不能按现状合并**。两个选择:关掉它们(接受 8.5.2/8.5.3 无条目),
或者在第二步落地后由重新生成的单分支 PR 带上它们的条目。**在第二步之前不要动
它们。**

---

## 3. #53 —— 三个 Error 级发现不计入退出码

### 3.1 已测得的事实

逐个数出来的:

| 发现 | level | 计入 `count_()` |
|---|---|---|
| `SubosManifest` | **Error** | ✗ |
| `SubosEnvOrphan` | **Error** | ✗ |
| `SubosEnvUnresolved` | **Error** | ✗ |
| `SubosEnvConflict` | Warning | ✗ |
| `SubosRuntimeMissing` | Warning | ✗ |
| `SubosDoubleBinding` | Error | ✓(本轮加的) |

后果有两层,第二层更隐蔽:

1. `xlings self doctor` 打印 `✗ subos env orphan …` 然后 **exit 0**。任何包着
   它的脚本都看不见。
2. `repair.healed = before - after.issues()`。一个**不计数**的发现即使被
   `--fix` 修好了,`healed` 也算不出来——**修复端做了事,报告端说没有**。这正是
   `reference_reporter_repairer_predicate_drift` 那条,只是换了个方向。

### 3.2 为什么当时没做

打开它们会**改变现有 home 的退出码**。本轮已经在改 subos 的行为,再叠一个退出码
变化,一旦 CI 变红就分不清是哪一个引起的。这是刻意的分离,不是遗漏。

### 3.3 方案:先量,再打开,分两个版本

**第一步:量影响面(不改代码)。**

```bash
# 真实 home + 每个 slice,只读
xlings self doctor 2>&1 | grep -cE "subos manifest|subos env orphan|subos env unresolved"
```

注意:真实 home(69G)上 doctor 要跑 **>2 分钟**,要留够超时。

同时在 CI 的 `doctor-acceptance.sh` 与 e2e 里搜一遍谁在消费退出码——这决定了
打开后会不会有测试变红。

**第二步:把三个 Error 计入 `count_()`。** 与 `SubosDoubleBinding` 同样处理,
`++c.broken`。同时把两个 Warning 计入 `c.warnings`——它们不该影响退出码,但
`healed` 的计算需要它们在场。

**第三步:`--fix` 的收敛性验证。** 对每一种发现,构造一个 home,跑
`doctor --fix`,再跑 `doctor`,断言**清零**。`doctor_fix_convergence_test.sh`
已经在做这件事,扩到这三种即可。

### 3.4 判据

> 对每一个 Error 级发现:`doctor` 退出非零 → `--fix` → `doctor` 退出零,
> 且第一次的 `healed` 大于零。

`healed > 0` 这一条是关键——它同时验证了"计数"和"修复"两端,而这两端正是历史上
分歧过三次的地方。

### 3.5 风险

中等,且可控。真实风险是**打开后发现现有 home 大面积报错**,那说明这些发现本来
就在被忽略。真要如此,先做一版**只报不计**的过渡(打印"下一版起这将影响退出码"),
给用户一个版本的时间。这条只在第一步的数据支持时才需要。

---

## 4. #55 —— B 线:门禁已过,缺的是环境

### 4.1 已测得的事实(2026-08-06 门禁验证)

四项全部通过,而且结论**比提案预测的好**:

- **运行时 dlopen 由发起它的对象的 DT_RPATH 服务。** 提案原写"任何 RPATH 机制
  都够不到运行时 dlopen",实测是错的(三组对照:无 rpath 失败 / DT_RUNPATH 成功
  / DT_RPATH 成功)。
- **GLX 不需要任何进程全局变量。** 无 `LD_LIBRARY_PATH` 时,带 DT_RPATH 的
  dispatcher 解析到我们的 interposer,不带的宿主进程解析到
  `/lib/x86_64-linux-gnu/libGLX_nvidia.so.0`。**两条规则同时成立。**
- **GLX vendor 与 Vulkan ICD 是同一个文件。** `libGLX_nvidia.so.0` 同时导出
  `__glx_Main`、`vk_icdGetInstanceProcAddr`、
  `vk_icdNegotiateLoaderICDInterfaceVersion`,三个入口透过 25 KB 的 interposer
  全部可达。一个 interposer 服务两条路径。
- **生产只需 patchelf**:`--set-soname` + `--add-needed <宿主 vendor 绝对路径>`
  + `--set-rpath --force-rpath`。安装期不需要编译器。

### 4.2 为什么当时没做

**门禁验证不需要图形栈**(用合成实验 + 宿主自带的 vendor 库就够),所以先做了。
**B1/B2 的端到端验证需要**,而本机的 home 里 mesa / libglvnd /
nvidia-gl-host-link 都没装。没装就写,等于再造一张手写表——AD-14 明确禁止的
那件事。

### 4.3 方案:四步,前两步是准备

**第 0 步:装图形栈,并记录基线。**

```bash
xlings install nvidia-gl-host-link mesa libglvnd -y
# 基线探针:当前(LD_LIBRARY_PATH 方案下)的 GL_RENDERER / EGL 设备数
```

没有基线就没有 A/B,B2 的"切换后仍然是 NVIDIA"就无法证明是切换的功劳。

**第 1 步:AD-12 —— interposer stub 作为索引包。**
每 arch 一份预置空 stub,走正常的索引/镜像/校验流程。消费它的 recipe 通过
`pkginfo.resolved_dep()` 拿 **payload 路径**(R6)。

**第 2 步:B1 —— `elfpatch.host_link_interposer`(libxpkg 0.0.52)。**

```lua
elfpatch.host_link_interposer{
    vendor  = "<宿主 vendor 的绝对路径>",
    deps    = <从 resolved_deps 推导的载荷 libdir 列表>,
    out     = "<我们 payload 里的 interposer 路径>",
    soname  = "libGLX_nvidia.so.0",   -- GLX 要求这个文件名
}
```

`deps` **必须从 `resolved_deps` 推导**,不能手写——§2.2 的手写表漏了五个库,
那正是 R7 的反面教材。

**第 3 步:B2 / B2' —— 两个 recipe 切换。**
`nvidia-gl-host-link` 删掉 `lib/xlings-deps/`;`libcuda-host-link` 用同一能力
(它今天**既不收拢依赖也不声明任何东西**,全部来自宿主,是同一问题的第二个答案)。

**第 4 步(独立,可并行):B4 —— 把决定编进产物。**
mesa / libglvnd 的构建把 vendor 目录与 DRI 目录设为自身载荷路径,删掉那两条
`subos.env`。这一步**不依赖 interposer**,但需要重新构建并发布 mesa/libglvnd
的 tarball,是独立的构建发布 train。

### 4.4 判据

| 步 | 判据 |
|---|---|
| B1 | `deps` 的内容等于 `resolved_deps` 推导出的闭包,**不含任何手写条目** |
| B2 | `LD_LIBRARY_PATH` 上**只有宿主驱动目录**,`GL_RENDERER` 仍是 NVIDIA,且与第 0 步基线一致 |
| B2 | 同一会话里 `/bin/bash`、`ldd`、宿主的 `glxinfo` 全部正常——这是 8-05 崩溃的直接回归测试 |
| B2 | 宿主进程的 `GL_RENDERER` **不是** llvmpipe(§2.6 实测的静默降级) |
| B4 | 删掉两条 `subos.env` 后,我们的 GL 栈仍能找到驱动 |

第三条判据最重要:它把"我们的库不进宿主进程"从**意图**变成**可测**。

### 4.5 风险

| 风险 | 触发信号 | 应对 |
|---|---|---|
| `DT_RPATH` 被 glibc 移除 | 未来 glibc 报错 | AD-7 的 wrapper 方案(nixGL 式,per-program) |
| interposer 顶替文件名与宿主 vendor 冲突 | 宿主进程加载到我们的 interposer | 用 `__GLX_VENDOR_LIBRARY_NAME` 换一个我们拥有的 vendor 名 |
| 不同机器驱动版本不同 | 换机器后 GPU 不可用 | AD-8:这是物理约束,做成**可见的提示**而不是继续尝试消除 |

---

## 5. 顺序

```
#56 实验(本机,~1 小时)
     │  结论决定 CI 是否能拿回 15 分钟/workflow
     ▼
#57 第一步(改分支名,一行)  ──┐
#53 第一步(量影响面,只读)  ──┤ 三者互不依赖,可并行
#55 第 0 步(装图形栈)      ──┘
     ▼
#57 第二步(口径待定 —— 需要 sunrisepeak 拍板)
#53 第二/三步(计数 + 收敛测试)
#55 第 1→2→3 步(AD-12 → B1 → B2/B2')
     ▼
#55 第 4 步 B4(独立的构建发布 train)
```

**#56 排最前**,不是因为它最重要,而是因为它是**复利成本**:B 线要反复迭代,
每轮多花 15 分钟 × 6 个 workflow。

**只有一处需要决策**:#57 第二步的"补齐 vs 只保最新"。其余都有明确判据。

---

## 6. 一条贯穿的自查

这四项里有三项(#53 #56 #57)是**已有机制没有闭合**,而闭合它们的判据都长一个样:

> 让"没发生"和"成功了"产生**不同的**输出。

- #53:`healed > 0` 而不是"打印了一行"
- #56:能**按需重现**而不是"换了前缀就好了"
- #57:**始终只有一个** open PR,而不是"记得按顺序合"

这正是提案 §4 的那条规则。它们没被闭合,不是因为难,而是因为**在默认路径下它们
看起来是好的**——和本轮修的七处一模一样。

---

## 7. 执行结果(2026-08-06,同日)

方案写完当天推进,三项落地、一项转为取证。**执行过程本身推翻了方案里的两处判断**,
按本文档自己的规矩记在这里,而不是悄悄改掉上文。

### 7.1 #56 —— 四个假设全部证伪,机制仍未知

`§1.3` 列了三个候选假设。实测(本机,注册表快照后还原,机器状态未变):

| 假设 | 实测 |
|---|---|
| **H1** 一个 job 内两个工具链共用 BMI 存储 | `dev → release → dev` **正常**。证伪 |
| **H2** tar 往返破坏元数据 | `.gcm` 集合**逐字节存活**,大小与 mtime 的 sha256 完全相同。证伪 |
| **H2b** 恢复 BMI 后全量重建项目 | **正常**。证伪 |
| **H4**(新增)恢复后 `prune_registry` 剪注册表 | **正常**。证伪 |

另外,"hash key 不够细"是**读**出来被排除的:`std-module.json` 显示 key 已覆盖
编译器、版本、`-std`、`driver_identity`、`--sysroot`、binutils 路径、目标三元组、
stdlib 版本、std 源码 hash。

**所以判据"能按需重现"仍未满足,** 而按本文档的规矩,这意味着**不能再改配置**。
落地的是**取证**:八处缓存点各加一个 `continue-on-error` 的诊断步骤,在缓存命中时
记录 BMI 集合的规模与每个目录的构建身份。`XLINGS_KEEP_RESTORED_BMIS=1` 可在一次性
分支上跳过删除,让失败带着记录发生。

CI 每轮多花的 ~15 分钟仍在账上。**这一项没有完成,只是从"不知道"变成"知道不是
哪四种"。**

### 7.2 #57 —— 方案的判据错了两次,都是实测发现的

**第一次:"比 `latest` 新"是错的判据。**
方案说"遍历 `releases` 复数,补齐缺失版本"。实现后在 main 上一测:`latest` 已经是
2026.8.6.1,而 8.5.2 / 8.5.3 **不比它新**,于是补齐逻辑永远够不到它们——
`check_package` 在 `upstream == current` 时直接提前返回。

正确判据是**"索引里没有这个条目"**,而"最新"应当意味着**"没有缺失"**,不是
"指针对得上"。这与 R1(记录必须全量)是同一条规则,只是换了个对象。

**第二次:第一个下界让范围失控。**
"比索引里最老的条目新"这个 floor,在真实索引上一路补到 `0.3.2` —— 16 个条目、
每个一次 sha256 下载。索引**故意**保留很老的条目,所以这个 floor 不可用。

改为**最近 10 个已发布版本的窗口**:有界、可预测,且与补齐的目的吻合(被 PR 堆叠
跳过的版本总是最近的)。`XIM_BACKFILL_WINDOW` 可覆盖。

双向验证:main 上报出 `['2026.8.5.2','2026.8.5.3']`;把这两个条目注入后立刻报
`up-to-date`。

**分支名那一步(R1)按方案落地**,并在实现与自查中额外修了两处:

1. **PR 标题与正文随 force-push 更新** —— 否则分支带着 2026.8.6.1 而标题写着
   2026.8.5.3,记录与它描述的改动不一致。
2. **只认 `OPEN` 的 PR**(自查发现)。单分支复用同一个 head,`gh pr view $BRANCH`
   也会匹配到上一轮**已合并**的 PR;编辑它并跳过 `pr create`,会让新提交躺在分支上
   **没有任何 PR** —— 正是单分支要消除的形状,在低一行的位置被重新引入。

**规模比方案里写的大得多。** 方案说"三个 PR 堆在一起",那只是**还开着**的部分。
实测索引上有 **49 个陈旧的 `bump/*` 分支**:mcpp 36 个、xlings 12 个、
mingw-cross-gcc 1 个 —— 这个脚本每发一次版就创建一个、然后遗弃一个。

(这条也是纠正:我一度从 PR 标题推断 `bump/mcpp` 是不带版本号的,查了分支名才发现
是 `bump/mcpp-2026.8.6.1`。从标题推断分支名是没有根据的。)

### 7.3 #53 —— 按方案落地,影响面为零

三个 Error 计入 `count_()`,两个 Warning 计入 `c.warnings`。

**先量后开**这一步有回报,但**我第一次的量取样不足** —— 只跑了文件名带 `doctor` 的
四个,CI 随后在 `subos_env_declaration_test.sh` 上失败:

```sh
DOC="$(x self doctor 2>&1)"     # doctor 现在返回非零 → set -e 直接终止脚本
```

它写于 doctor 对 orphan 返回 0 的年代,和 S12 是同一形状 —— **测试把旧行为编进了
控制流**,而不是断言里。

按 R7 重新枚举:**12 个** e2e 会跑 doctor,不是 4 个。全部跑过后 12/12 通过,
过渡版本仍然不需要。判断没变,但先前那个判断**当时没有证据支持**。

`|| true` 加在捕获上,因为断言的是**报告**,不是**判决**。

判据 `healed > 0` 落为 `doctor_fix_convergence_test.sh` 的 S7,实测
`healed=2`。写它时又踩了一次 `set -e`——`out=$(...)` 在非零退出时直接终止脚本,
而非零退出正是这条场景要断言的东西。

### 7.4 #55 —— 未开始,前置未完成(已被 §7.6–§7.8 取代)

图形栈正在装进隔离 home。**门禁已过不等于可以写 B1**:没有装好的栈就没有基线,
B2 的"切换后仍是 NVIDIA"证明不了是切换的功劳。

### 7.5 对 §6 那条自查的回访

§6 说三项的闭合判据都长一个样——让"没发生"和"成功了"输出不同。执行结果:

| | 判据 | 是否达成 |
|---|---|---|
| #53 | `healed > 0` | ✅ 实测 `healed=2` |
| #57 | 始终只有一个 open PR | ✅ 机制删除,不再依赖记忆 |
| #56 | 能按需重现 | ❌ **未达成**,四个假设被排除 |

两项达成、一项没有。没达成的那项**没有被当作达成**,这本身就是这条规矩的用处。

### 7.6 #55 B 线:核心命题已在真实栈上验证(2026-08-06)

图形栈用 `xlings config --mirror CN` 装进隔离 home(22 个包)。**先前两次判它
"网络不可行/卡住"都是错的** —— 第一次是隔离 home 默认 GLOBAL 镜像,第二次是我用
"20 秒零字节"给一个正在下载与解压之间的安装下了死刑。用单目录短时增长判定长任务,
不是测量。

#### 基线(glprobe,渲染并读回像素)

| 环境 | `GL_RENDERER` | `PIXEL` |
|---|---|---|
| 宿主 | NVIDIA GeForce RTX 4080/PCIe/SSE2 | 336699 |
| **进 subos(今天的 `LD_LIBRARY_PATH` 方案)** | **llvmpipe (LLVM 20.1.7)** | 336699 |

**§2.6 的缺陷被实测到了**:同一个链接宿主 libEGL 的二进制,进 subos 后从 GPU 掉到
软件渲染,没有任何提示。两边像素都对 —— 所以"能不能渲染"这个检查抓不住它,只有
渲染器名字能。

#### interposer 对照

27 KB,patchelf 三步(`--set-soname` / `--add-needed <宿主 vendor 绝对路径>` /
`--set-rpath --force-rpath <从载荷推导的闭包>`),安装期不需要编译器。

| | `GL_RENDERER` | `LD_LIBRARY_PATH` |
|---|---|---|
| interposer + **我们的** loader | **NVIDIA GeForce RTX 4080/PCIe/SSE2**,`PIXEL=336699`,`RESULT=ok` | **只有 `/usr/lib/x86_64-linux-gnu`** |

`lib/xlings-deps` 完全不参与。**B2 的验收判据当场满足。**

#### 边界:interposer 只对跑在我们 loader 上的消费者安全

同一个 interposer 给**宿主二进制**(宿主 loader、宿主 libc)用:

```
librt.so.1: undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
```

正是 8-05 那次崩溃的同一条错误。原因清楚:interposer 的 RPATH 指向**我们的**
glibc,而消费者的 libc 是宿主的 —— §2.3 说的"两半来自不同构建"。

**这不是缺陷,是适用域**,而且与 §2.3 的推断一致:vendor 被 dlopen 进的那个进程
本来就该是我们的(INTERP 指向我们的 glibc)。但它给 B1 加了一条**必须写进契约**的
前置条件:

> `host_link_interposer` 产出的对象,只可由 INTERP 指向我们载荷的消费者加载。
> 宿主二进制必须继续走宿主自己的 vendor。

这条正是 §2.6/B4 存在的理由:把 vendor 目录编进**我们自己构建的** libglvnd,宿主的
libglvnd 就用宿主默认目录,两条路径天然分开。

#### 与门禁验证的关系

门禁(§2.7)证明的是**机制可行**(DT_RPATH 传递、dlsym 穿透、GLX 无需全局变量);
这一节证明的是**在真实 NVIDIA 栈上有效**,并测出了适用域。两者都不能替代对方。

### 7.7 #55 B 线落地:三个"看起来成功"的东西被逐一测穿(2026-08-06)

§7.6 证明了机制。落地时,每一步都先给出一个通过的结果,再被更严的测量推翻。

#### 一:载荷是空的,而断言救了场

`interposer-stub` 装完,payload 目录空无一物。install hook 自己的断言把它拦下并
点名了包和路径。根因不是解包布局,是 **resource 声明位置错了** ——
`url_template` 被我嵌进了版本条目里,而它是版本表的**兄弟**;带 per-arch sha256
的条目要用 `x86_64 = { url, sha256 }` 子表(`aria2-next` 是样板)。解析器对此只
说了一句 `resource has neither url nor source`,然后什么也没下载。

**如果 install hook 没写那句断言,这个包会"安装成功"并留下一个空目录。**

#### 二:`glxinfo` 报出 RTX 4080,而它证明的是宿主栈

subos 里跑 `glxinfo -B`:

```
OpenGL renderer string: NVIDIA GeForce RTX 4080/PCIe/SSE2
```

看着是 B 线成了。实际上 `command -v glxinfo` = `/usr/bin/glxinfo`,
`patchelf --print-interpreter` = `/lib64/ld-linux-x86-64.so.2` —— **宿主二进制跑在
宿主 loader 上**,我们的载荷一个字节都没参与。这行输出在 B 线成功与彻底没发生时
**完全一致**。

改成从 `/proc/self/maps` 报出每个 GL 对象的**实际来源路径**后,真相立刻可读。
两个探针现在都这么做,harness 断言我们的 interposer 在其中。

#### 三:`interposer: yes`,而四个入口点只覆盖了一个

glvnd **按名字 dlopen** 每个 vendor 库,所以 `libEGL_nvidia.so.0`、
`libGLX_nvidia.so.0`、`libGLESv1_CM_nvidia.so.1`、`libGLESv2_nvidia.so.2`
各是一条独立载入链的**根**。DT_RPATH 只沿链**向下**传递,永远不会横跨到另一个根。

只 interpose 了 libEGL 时:EGL 在 4080 上渲染,GLX 的整个闭包仍来自 `/usr/lib`。
而安装日志说的是 `interposer: yes`。

现在四个入口点全部 interpose,日志报的是**分数** `4/4`。`libGLX_nvidia.so.0`
同时是 Vulkan ICD,所以这一个根承载两套 API。

#### 四:`deps.build` 声明了,patchelf 没装(libxpkg loader 缺陷)

`deps` 表只要有数组部分,loader 就走 legacy 分支:**`build` 子表被丢弃,数组项
被复制进 `build_deps` 顶替**。声明照写、安装照样报成功、两件事都没做。症状出现在
两层之外 —— elfpatch 警告 "patchelf 解析到 host"。

libxpkg 0.0.52 修了混合形状,但**索引不能依赖这个修复**:它要服务所有版本的
客户端,而 recipe 里没有办法探测 loader 的形状能力(不像 Lua 函数可以 `type()`
探测)。所以 recipe 改用纯 split 形式,并加了一条静态测试禁止混合形状 ——
括号配对判断,不是正则(`deps` 表里几乎总有嵌套表和注释)。

#### 顺带纠正一处我自己的错话

我曾说"约 30 个用 elfpatch 的 recipe 在静默回落到宿主 patchelf"。两处都错:
`_find_tool` 的顺序是 **payload → subos view → home bin → host**,回落时**会警告**
(那句警告正是 A3 加的),且 `tool_payload_dir` 会**扫整个 store** —— 只要 home 里
装过 patchelf 就走载荷,与是否声明无关。真实缺口只剩一个窄口子:**从未装过
patchelf 的 home**。声明 build dep 堵的是这个。

#### 最终验证

`.agents/tools/graphics/verify-host-link.sh`,真实 RTX 4080(驱动 550.144.03),
隔离 home,**12/12**:四个入口点形状正确、EGL 渲染出像素、GLX 渲染、两者都经过
**我们的** interposer、宿主真 vendor 由绝对 DT_NEEDED 拉入、`LD_LIBRARY_PATH` 为空、
宿主驱动文件未被改动。

#### 这次落地新增的一条契约前置

vendor 的 dlopen 由**调用方**的搜索路径服务,而 `libGLX.so.0` 自己的 RPATH 是
`$ORIGIN` —— 它看不到 vendor 包。真正让它解析成功的是:**DT_RPATH 会沿载入链向上
一直搜到可执行文件**。DT_RUNPATH 不会 —— 同一个探针用 `--enable-new-dtags` 构建,
X 连接正常、GLX 扩展正常,却一个 vendor 都找不到。

> 消费者必须携带 **DT_RPATH**(`--force-rpath`),不能是 DT_RUNPATH。

### 7.8 收尾:四仓的实际执行顺序,与两个新发现(2026-08-06)

#### 跨仓依赖顺序 —— 计划与实际

§5 给的顺序是「libxpkg → xlings → 索引」。实际执行时它被拆得更细,因为
**mcpp-index 是一个此前没有画进依赖图的中间环节**:xlings 的构建从 mcpp 的
registry 解析 `mcpplibs.xpkg`,而那条链有自己的发布与镜像。真实顺序:

```
libxpkg PR 合并
   → 打 tag        (GitHub 的 archive tarball 即产物)
   → gtc 镜像到 gitcode.com/mcpp-res/xpkg   (CN,与 GLOBAL 同一份字节)
   → mcpp-index 加条目 + 合并
   → Publish Index Artifact                  (滚动指针 mcpp-index-pointers.json)
   → xlings 才能 pin 这个版本并通过 CI
   → xlings release
   → xim-pkgindex bump(xlings)               (bump_index.sh 自动开 PR)
```

**这一环走了两遍**(0.0.52、0.0.53),第二遍是因为 #487 的诊断修复要一起发。
每一遍都要等 artifact 发布,xlings CI 才拿得到。把它画进依赖图是这次最该记下的
东西 —— 之前的「libxpkg → xlings」看起来是一条边,实际是六步。

#### 发布结果(2026.8.6.3)

| 环节 | 结果 |
|---|---|
| xlings PR #490 CI | 8/8 |
| release | 全 job 成功,4 平台 |
| GitHub 资产 | 8/8(4 tarball + 4 sha256) |
| GitCode | 补齐后 8/8,4 个 tarball 与 GitHub 哈希逐一致 |
| 索引 `latest` | 三平台均 2026.8.6.3(#536,13/13) |
| 生命周期验证 | PASS 8/8,发布二进制 + 隔离 home,`~/.xlings` 未被写 |
| B 线真实验证 | 11/12,RTX 4080 + 驱动 550.144.03 |

`mirror-binaries` 这一次只传了 4 个 `.sha256` 边车,**tarball 本体 404**。用本地
gtc 补齐后独立复验(GET,不用 HEAD —— GitCode 对 HEAD 返 401)。这与
[[project-release-cancel-recovery]] 记的是同一个缺口,再次出现。

#### 新发现一:`subos` 与 `subos use` 对同一个 subos 给出相反答案(#491)

B 线 12 项里唯一失败的那项,追下去不是图形栈的问题:

- `xlings subos`(列表)枚举 `<home>/subos/` 下的**目录**
- `xlings subos use` 查的是**状态 JSON**(`subos.cppm:743`)

装包会建出 `subos/<name>/lib`、`usr`(sysroot 链接)却不把这个 subos 注册进
JSON。于是目录侧说存在、JSON 侧说不存在,而提示语把「记录缺失」说成
「你还没创建」。**同一个问题两个答案源**,与本文档 §6 那条自查是同一形状。

#### 新发现二:pmwrapper 时代的依赖名指向空气(#534)

`project-graph` 的 `deps = { "webkit2gtk" }` 在索引里没有对应包,recipe 顶部还留
着当时的 TODO。pmwrapper 模式已经不用,所以这条依赖现在什么都解析不到。之所以
一直没暴露:install-test 只测 changed-files 里的 recipe,而这个包很久没被改过 ——
是本轮给它加平台差异标注时才第一次被测到。

已排期为**从源码自建**(不用 host-link sentinel),完整功能档,建在钉死
glibc 2.39 的 `xlings subos` sandbox 里。约 19 个缺失硬依赖 + WebKit 本体。

#### 本轮真正的产出不是修了几个 bug

七个「看起来成功」的东西被更严的测量逐一推翻:空载荷装成功、`glxinfo` 报出
RTX 4080 而全部对象来自 `/usr/lib`、`interposer: yes` 而四个入口只覆盖一个、
`deps.build` 声明了却没装、CI 报绿而零个包被测、我加的诊断从不运行、断言里写死
了会变的版本号。

其中最后一类的根因——**一次 bump 只落进 `xpm.linux`**——现在由两个索引仓库的
CI 规则挡着(`check_platform_version_parity.lua` /
`test_platform_version_parity.py`),提交前都对修复前的文件证伪过。真实差异用
`platform_versions_diverge = true` 显式声明退出,那是**有人特意做出的主张**,与
遗漏的区别就在这里。
