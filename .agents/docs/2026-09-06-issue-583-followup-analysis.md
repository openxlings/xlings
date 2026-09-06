# issue #583 追加回复分析 — 报告者的四个回答,以及 2026.9.4.1 对这个场景做了什么

日期:2026-09-06 · 输入:[#583](https://github.com/openxlings/xlings/issues/583) 正文 + 三条评论(最后一条 [comment-5556100037](https://github.com/openxlings/xlings/issues/583#issuecomment-5556100037))
代码基线:`main` @ 8bfa8fa(2026.9.4.1)· 实测二进制:`~/.xlings/bin/xlings` = 2026.9.4.1

---

## 0. 结论先说

1. **设计层面这个 issue 已经收敛。** 四个回答把 relocation 的范围钉死为「一次性目录整理」,won't-do 被接受,`51 stayed at 51` 的更正也被接受。按报告者自己的表述,剩下的诉求只有一条:**doctor 说出「这个 home 好像从 X 移到了 Y」**。

2. **但优先级和他说的相反 —— 这条不再是 nice-to-have。** 实测:在 2026.9.4.1 上,对一个被移动过的 home 跑 `self doctor --fix`,**会把库农场(`<subos>/lib`)里的链接删掉**,而它指向的 payload 在新根下一直存在。2026.8.30.2 不会删 —— 因为那时 `lib/` 根本不在扫描范围里。也就是说:**#585 修好了扫描盲区,同时把这个场景从「可用一条 symlink 完全救回」变成了「救不回来」**。报告者的 workaround 正好挡住了它,所以他自己还没撞上。

3. 所以要做的不是「加个提示」,而是 **把两条破坏性修复(注册剪枝、悬空链接删除)统一门控在「payload 在新根下存在」这个事实上**。提示是副产品。

---

## 1. 四个回答分别定下了什么

| 问题 | 回答 | 定下的事 | 它没说但可以推出的事 |
|---|---|---|---|
| 1. 为什么移动 | 一次性,整理 projects 目录,连带移动了两个 checkout 和里面的 per-project home | relocation 机制 won't-do 成立,产出上限 = 诊断 | 「一次性」是对**他**成立;`$MCPP_HOME` registry 长在 checkout 里,而人天天移动 checkout(改名、归组、换 worktree)。对人群而言这是**结构性复发**,不是偶发 |
| 2. 为什么不重建 | 只因为时间:两个 home 约 4 GB / 1.5 GB,重新 provision 后首次是三目标全量重建,≈1 小时;老路径加 symlink 立刻全好,所以是**推迟**不是放弃 | 「丢缓存重建」这条受支持路径有真实价格,不是零成本 | ① 这台机器上会**长期存在**一个「被移动 + 老路径 symlink」的 home;② 他要付 1 小时,很大程度是为了找回被 `--fix` 删掉的 86 条注册 —— 而这部分**可能不需要重建**(见 §3.R4) |
| 3. 哪个 home | per-project `$MCPP_HOME`,在 checkout 内;`/home/<u>/Projects/<proj>/.mcpp` → `/home/<u>/Projects/<group>/<proj>/.mcpp`,同一文件系统,plain `mv` | 场景是纯路径前缀替换,没有跨设备、没有权限变化 | 这正是**最干净的探测条件**:新根可从 exe 路径推出(`config.cpp:501`),旧根写在版本库每一条 path 里,两者只差一个前缀 |
| 4. 哪个二进制 | registry 内自带的 `<registry>/bin/xlings`,2026.8.30.2,`XLINGS_HOME`/`XLINGS_BIN` 都不设 | 审计数据来自 8.30.2 —— 与「`lib/` 从未被扫描」完全自洽,`51 stayed at 51` 的更正站得住 | **崩溃那次是另一个配置**:`XLINGS_HOME` 指向全局 home + 沙箱不可写。即 SIGABRT 与「home 被移动」**无因果关系**,是同一份报告里的两个独立缺陷 —— #585 按「只读 home」做的差分验证,打的是对的靶子 |

### 一条需要在回复里纠正的口径

报告者说「a diagnostic that names the condition is all I would ask for」。按 §2 的实测,**只给诊断是不够的**:诊断不会阻止 `--fix` 删链接。要么门控,要么诊断出现时直接拒绝执行破坏性步骤。

---

## 2. 实测:2026.9.4.1 在被移动的 home 上删掉可恢复的库链接

### 复现(合成 home,20 行,不碰真实 home)

脚本:`$SCRATCHPAD/reloc_repro.sh`。构造一个最小 home:一个 `kind=lib` 的条目 `libfoo@1.0`,payload 在 `data/xpkgs/xim-x-foo/1.0/libfoo.so`,农场链接 `subos/default/lib/libfoo.so` → 该绝对路径;然后 `mv old new`,用 2026.9.4.1 跑 `self doctor --fix`。

```
### control: home NOT moved
  [after ] link PRESENT -> .../ctrl/data/xpkgs/xim-x-foo/1.0/libfoo.so     ← 不动,正确

### moved home (mv old -> new, same filesystem)
  [before] link PRESENT -> .../old/data/xpkgs/xim-x-foo/1.0/libfoo.so      ← 悬空
  exit=0
  [after ] link GONE                                                        ← 被删除
  [after ] DB path: .../old/data/xpkgs/xim-x-foo/1.0                        ← 版本库仍指老根
```

`--fix` 的输出:

```
    · dangling link removed  @xlings/subos/default/lib/libfoo.so
  ▸ status                   OK — workspace, shims, and payloads are all consistent
  ▸ healed                   1
```

**删掉的那一刻,`<new>/data/xpkgs/xim-x-foo/1.0/libfoo.so` 是存在的。**

### 代码链路(为什么必然如此)

1. `doctor.cpp:2082` — 2026.9.4.1 把 `lib`/`lib64` 加进了 sysroot 扫描根。移动后农场里每一条链接都悬空,全部成为 `SysrootDangling`。
2. `doctor.cpp:2471` — 修复分支先问版本库「这条链接的源在哪」:`sysroot_link_source_`(`doctor.cpp:2258`)。
3. `bindings.cpp:499` — `library_placement` 返回 `.source = data.path / sourceName`,**`data.path` 是原样读的绝对路径(老根)**,没有过 `expand_path`。
4. `doctor.cpp:2280` — `sysroot_link_source_` 末尾 `if (!exists(sources.front())) return {};` → 老根不存在 → 返回空。
5. `doctor.cpp:2494` — 空源 = 「没人能说这条链接属于哪」→ 走删除分支 `fs::remove` + `prune_empty_asset_dirs`。

第 4 步是要害:**存在性检查是在老根上做的,没人问过「同样的相对路径在新根下是否存在」**。而这段代码自己写着规则:

> *"Deletion is what you do when nothing can say where the link belongs, not when you have not asked."* (`doctor.cpp:2464`)

被移动的 home 恰恰**说得出来**(相对路径不变),只是没人在新根下问。这是同一条规则的违反,不是新规则。

### 后果链

- **删了就回不来。** `FindingKind` 里只有 `SysrootDangling`(悬空),**没有「农场里少了一条应有的链接」这一类**(`doctor.cppm:96-230` 全枚举核对过)。链接一旦删除,doctor 的任何后续运行都看不到缺失;只有重装该包(config hook → `place_asset`)才会重新放置。
- **报告者的 workaround 现在是承重结构。** 老路径上的 symlink 在,链接就不悬空,连 finding 都不会产生。他一旦「清理掉那条 symlink 再跑一次 --fix」,农场就没了 —— 而那正是一次看起来最像收尾的操作。
- **有 symlink 时确实安全(已实测)**:同一合成 home,`mv` 后在老路径建 symlink 指向新路径 → 链接解析成功 → **既不产生 finding 也不被删除**,`--fix` 输出里没有 `dangling link removed`。所以「在重建完成前不要移除那条 symlink」这条建议是成立的,不是推测。
- **量级参考(他的 home)**:大 home `subos/default/lib/` 下 51 条,小 home 46 条,会在第一次 `--fix` 中被删除。8.30.2 时代那 153 条 `usr/include` 已经删过了。
- **退化方向明确**:8.30.2 上,`--fix` 之后一条 symlink 就能让「dangling 归零、musl 与 glibc 双目标真实构建通过」(报告者实测)。9.4.1 上,同样的 symlink 无法恢复已被删除的链接。

### 附带缺陷:删了资产仍然 `status OK` / exit 0

`doctor.cpp:3683-3708`:「有损」这档只挂在 `repair.pruned > 0`(注册剪枝)上。删除 sysroot 资产计入 `healed`,不计入损失,所以上面那次运行**删了 sysroot 里的文件,然后打印 `OK — workspace, shims, and payloads are all consistent` 并 exit 0**。

这与 #585 刚刚立下的口径直接冲突:*「a run that dropped registrations must say so and must not exit 0(退出码保持)」*。删文件的破坏性不低于掉注册,却没有进同一档。这是「修复自身重新引入同一缺陷类」的又一例:三档判决(clean / lossy / broken)建好了,但只接了一个损失来源。

---

## 3. 还欠什么(按「能不能防止丢数据」排序)

| # | 内容 | 位置 | 说明 |
|---|---|---|---|
| **R1** | **门控悬空链接删除**:删之前,把链接目标里 `…/data/xpkgs/<pkg>/<ver>/<rest>` 的后缀拼到当前 `homeDir` 下;存在 → 这是搬迁不是死链,**重新指向新根**(不是保留、不是删除) | `doctor.cpp:2464-2500` | 唯一能阻止上面那次数据丢失的改动。顺带把搬迁的农场**修好了**,而不只是不弄坏 |
| **R2** | **门控注册剪枝**:同一判据,payload 在新根下存在 → 拒绝 deregister,并说明原因 | `prune_dead_registrations_`,`doctor.cpp:3049` | 报告者原文的最低诉求。注意剪枝只作用于 `kind=program`(`doctor.cpp:1325` 处 `effective_kind != "program"` 直接 continue),所以他那 86 条全是 program 条目 |
| **R3** | **HomeRelocated 诊断**:命名条件「this home appears to have moved from X to Y」,并作为 R1/R2 的共同前提呈现给用户 | 新 `FindingKind` | 信号是免费且普遍的:真实 home 里 **2908/2908** 条带 path 的版本条目是绝对路径(0 条用占位符),前缀一律是 `<root>/data/xpkgs`;新根由 exe 自身推出(`config.cpp:501`)。判据 = 「前缀 ≠ 当前 homeDir 且相对路径在当前 homeDir 下存在」 |
| **R4** | **给报告者的恢复路径**(见下) | — | 可能免掉他那 1 小时 |
| **R5** | 版本库改存 `${XLINGS_HOME}` 占位符 | `installer.cpp` 注册侧 | **能从根上避免那 86 条 deregistration**(路径永远解析到当前根,`BrokenPayload` 根本不成立)。但:① 需要审计 **70 处**直接读 `path` 而不过 `expand_path` 的地方 —— `library_placement`(`bindings.cpp:499`)就是其中一处,证明这个风险是真的;② 对老 home 无效;③ **绝不能对外说成「home 可搬迁了」**:ELF `PT_INTERP`、linker script、`.xlings-resolution.json` 一个都没动 |
| **R6** | 报告者提到的 6 条 `xvm-binding-target-missing`,在路径恢复后**依然存在** —— 按他自己的观察,与移动无关 | 未认领 | 一个在「一切都能解析」的 home 上稳定报错的 binding 状态,值得单独查/单独开 issue,别混在 #583 里 |

### R4:那 86 条注册可能不需要全量重建

`installer.cpp:3092` 的 config hook(注册就发生在这里)**不受 `payloadInstalled` 影响** —— install hook 会因为 payload 已在盘上被跳过(`installer.cpp:2750`),但 config hook 照常执行。因此 `xlings install <name>@<version>` 在「payload 还在、注册被剪掉」的 home 上,理论上会**只重新注册、不重新下载/编译**。

**状态:只读了代码,未实测。** 在告诉报告者之前必须实测(见 §5),否则就是让人在他唯一的现场上试一条没验证过的指令。

---

## 4. 回复要点(建议)

1. 谢四个回答;确认 (1) 一次性 → relocation 机制维持 won't-do。
2. **主动纠正自己上一条的结论**:「diagnostic 就够了」在 2026.9.4.1 上不成立。给出 §2 的实测:被移动的 home 上 `--fix` 会删掉 `<subos>/lib` 下的链接,而 payload 就在新根下;9.4.1 之前不会,因为那时 `lib/` 没被扫描。**建议他在完成重建之前,不要移除老路径上的那条 symlink,也不要在没有 symlink 的状态下跑 `--fix`**(有 symlink 时不删,已实测)。 这是这条回复里唯一有时效性的内容。
3. 说明将要做的:R1 + R2 + R3(门控 + 命名条件),以及 `status OK` 那一档要把资产删除也算进损失。
4. 关于 (2) 的 1 小时:给出 R4 的恢复路径 —— **实测之后再给**。
5. 关于 (3):指出 `$MCPP_HOME` 长在 checkout 里,所以「移动 checkout」对人群而言不是一次性的;这不改变 won't-do,但它是把 R1/R2 从「体贴」升级为「必须」的理由。
6. R6 单独开 issue,不在 #583 里继续。

---

## 5. 下结论前还需要测的两件事

(E3「有 symlink 时确实安全」已测,见 §2 末尾。)

| 实验 | 怎么测 | 什么结果会推翻现有判断 |
|---|---|---|
| **E1** 真实 home 上的删除量级 | `.agents/tools/slice-real-home.sh` 切片 → `mv` → 9.4.1 `--fix` → 数 `subos/*/lib*` 下链接的前后差 | 若真实 home 上因为其它 finding 提前中断而没走到删除分支,§2 的量级要下调(合成 home 已证明分支可达,但真实 home 的路径更长) |
| **E2** R4 的重新注册 | 隔离 home 装一个小包 → 手工从版本库删掉它的条目(保留 payload)→ `xlings install <name>@<ver>` → 看是否重新注册且未重新下载 | 若 config hook 因为计划里节点被 gate 掉而没跑到,R4 不成立,不能告诉报告者 |

---

## 附:证据清单

- 实测脚本与日志:`$SCRATCHPAD/relocbench/{ctrl,moved,moved2}.log`、`reloc_repro.sh`
- 真实 home 路径统计:`~/.xlings/.xlings.json` → 2908 条带 path 的版本条目,2908 条绝对路径,0 条 `${XLINGS_HOME}`
- 代码:`doctor.cpp:2082 / 2258 / 2464-2500 / 3049 / 3683-3708`、`bindings.cpp:482-504`、`config.cpp:501`、`installer.cpp:2750 / 3092`、`doctor.cppm:96-230`
