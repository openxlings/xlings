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

**分支名那一步(R1)按方案落地,无修正**,并额外修了一处:PR 标题与正文现在随
force-push 更新,否则分支带着 2026.8.6.1 而标题写着 2026.8.5.3。

### 7.3 #53 —— 按方案落地,影响面为零

三个 Error 计入 `count_()`,两个 Warning 计入 `c.warnings`。

**先量后开**这一步有回报:四个 doctor e2e(含两个明确断言 `doctor` 退出 0 的)**全部
仍然通过**,所以过渡版本不需要。

判据 `healed > 0` 落为 `doctor_fix_convergence_test.sh` 的 S7,实测
`healed=2`。写它时又踩了一次 `set -e`——`out=$(...)` 在非零退出时直接终止脚本,
而非零退出正是这条场景要断言的东西。

### 7.4 #55 —— 未开始,前置未完成

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
