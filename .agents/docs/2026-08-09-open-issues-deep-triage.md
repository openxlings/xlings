# 全量 open issue 触诊(44 条)与收敛处置

> 2026-08-09 · 基线:**v2026.8.9.2**(origin/main @ f203b6b,本地树与其零 diff)
> 方法:44 条 open issue 按子系统分 8 域并行调查(读全文+全部评论 → 在当前树核实代码路径 → 分类);
> 全部 25 个关闭决定的承重证据(commit / file:line / CI run / 线上探测 / 外部仓库事实)在主线**逐条二次复核**后才执行。
> 处置(以 speak-agent 账号执行):关闭 25 条(13 completed + 12 not planned,其中 3 条为合并)、
> 在 3 个合并目标上留合并说明、新开汇总帖 **#517** 承接关闭说明与残留清单。

---

## 0. 结果一页

| | 数量 | 明细 |
|---|---|---|
| 关闭·已修(completed) | 13 | #45 #48 #55 #58 #102 #130 #155 #345 #350 #408 #456 #459 #476 |
| 关闭·过时/取代(not planned) | 9 | #16 #21 #29 #37 #38 #54 #96 #104 #123 |
| 关闭·合并(not planned) | 3 | #491→#500、#503→#506、#419→#423 |
| 保留·存量缺陷 | 15 | #513 #514 #464 #376 #493 #417 #425 #500 #506 #423 #442 #458 #433 #427 #153 |
| 保留·功能请求 | 3 | #206 #467 #501 |
| 保留·信息帖 | 1 | #2(建议刷新链接到 openxlings 口径) |
| 新开 | 1 | #517 汇总帖 |

终态实测:open = 20(19 保留 + #517),编号集合与上表一致。

---

## 1. 已修关闭(13)——判据

每条都指到**当前树里真正解决它的代码**,不以 commit message 为准。

| # | issue | 修复证据 |
|---|---|---|
| #45 | semver 模块请求(2024-12) | `src/core/semver.cppm`(#289 引入;2913a09 = v2026.8.9.2 通用化为 N 段+字母段+单比较子);`resolver.cppm:47,60` 生产消费;`tests/unit/test_semver.cpp` |
| #48 | OS/架构对安装的影响(2024-12) | 配方 `xpm.{linux,macosx,windows}` + per-arch 资源表;`compatibility.cppm`(ArchEvidence 模型);`resolver.cppm:155-164` 与 `installer.cppm:2298-2307` 双处 fail-closed;OS 版本差异由自包含 payload + subos runtime 绑定(C1,054be37)消化 |
| #55 | 结构统一提案(2024-12,英文) | MC++ 重写(4adfd9c)交付了提案分层:src/cli、src/runtime、src/core/{config,xim,xvm,subos};`remove` vs `self uninstall` 歧义已解(spec.cppm:53,78) |
| #58 | 指定依赖安装目录(2024-12) | 单根模型:`config.cppm:638` `dataDir = homeDir/"data"`;整根经 XLINGS_HOME 重定位;维护者当年宣布的方案形态即现状 |
| #102 | fix 自动修复命令(2025-06) | `xlings self doctor --fix`(spec.cppm:84;doctor.cppm detect→repair→re-detect 收敛;e2e doctor_fix_convergence);mongodb 式外部 runbook 未采用→记 #517 残留 |
| #130 | 安装路径自定义(2026-02) | `config.cppm:553-563` XLINGS_HOME 最高优先;`install.cppm:203-219`「显式目标即权威(含首装)」(0c362cf,v2026.8.3.1);quick_install 零 sudo;e2e fresh_xlings_home 双平台 |
| #155 | CentOS7 装 gcc 找不到命令(2026-02) | 根因是当时索引 linux gcc 委托 musl-gcc 只注册 musl- 前缀;xim-pkgindex#56(4e6d7634,实测确touch gcc.lua)换真实 glibc 预构建 + programs={gcc,g++,c++};fresh-install CI CentOS 7 cell 持续绿(run 31277647290) |
| #345 | .xlings.json 写失败(2026-06) | 半初始化 home(pacman 直装、未跑 self install)→ subos/default/ 不存在;0c362cf 在 `config.cppm:1379-1384` 写前 create_directories,注释即引用同型 #471;v2026.8.3.1 发布 |
| #350 | Windows TUI「编码」乱码(2026-07) | 非编码:是 OSC-11 背景色应答未消费(`0a/2424^[\` = rgb 片段);3147426(PR #373,0.4.65)改为 DSR fence 读完再恢复终端(unix.cppm:131「This fixes #368」);原生 Windows 不发查询(windows.cppm:317 注释);场景实为 WSL |
| #408 | sysroot 多版本设计贴(2026-07) | 阶段 0/1 全落地(PR #410 库随 release 切换、use 物化头文件、files 资产类、锚点 Notice);余项=closure 规则 C(2026-08-09-ecosystem-closure-design.md),存量数据面归 #458 |
| #456 | list 漏已装 mcpp(2026-07) | 根因推翻 issue 两个猜测:cmd_list 走 catalog.search 的 latest 倾向 match;0c362cf 新增 `inventory.cppm`(workspace installed[]+versions DB+payload 戳记),commands.cppm:936,1102 消费;断言未削弱,CI 红→绿边界恰在 0c362cf |
| #459 | 体验优化1(2026-07-31) | 两条均 91922e3(PR #463,v2026.7.31.3):`pin_target_to_active`(resolver.cppm:52,95,236)+ `cmd_use_by_name` 多候选直接委托列表、exit 0;计划文档 2026-07-31-install-use-semantics-plan.md:6 显式引用本 issue |
| #476 | index 无法取旧 snapshot(2026-08) | 47fb5c4(PR #477,v2026.8.4.1):choose_snapshot 自动路由 + E_INDEX_NO_COMPATIBLE_SNAPSHOT 硬失败 + `index list/use` + 持久 pin + self update 走 XLINGS_INDEX_PIN=newest 防死锁;今日实测线上指针:4 索引 × 8 条 history + client_latest=2026.8.9.2 |

## 2. 过时/取代关闭(9)

| # | 判据 |
|---|---|
| #16 #21 #96 | d2x 教学模块(run 命令 / LLM 提示 / 编辑器)随 4adfd9c(2026-02 MC++ 重写)整体移出;后继 d2learn/d2x 已有 editor.cppm/assistant.cppm(实测在);编辑器本身在 xlings 是普通包 |
| #54 | 架构走了提案的反方向:自持安装 + xvm shim + SubOS;pm 封装层已删(src 零命中);thread 内维护者已表态「xvm shouldnt process system's package-manager directly」「系统 PM 不支持多版本(core reason)」 |
| #104 | PR #105 的宿主 PATH 探测(code@system)未随重写保留;现行自包含口径下「下载自己的副本」是设计行为;恢复路径(code.lua installed() 钩子)记 #517 残留 |
| #37 | 底层痛点(清单/干净卸载/更新)已由 self uninstall/update + 单根解决;AUR 在树内但 PKGBUILD 停 0.4.14、release 无自动 bump;渠道扩展记 #517 残留 |
| #29 | 包名层模糊纠正已存在(commands.cppm:335-360);命令层 did-you-mean 记 #517 残留(cli.cppm:916-928 编辑距离即可) |
| #123 | 空正文 TODO;标签整理任务本身记 #517 残留(d2x/xchecker 标签已失效,subos/index/agent 无标签) |
| #38 | 0.0.1 稳定版语义被日期版本取代;条目或已异形落地或随拆分移出;交叉引用(addto: #38)关闭后仍可达 |

## 3. 合并(3)

- **#491 → #500**(同一缺陷,双语双报):裸 install 物化 subos 内容但不写 home 注册表;`subos list` 合成 default(subos.cppm:85-88)vs `subos use` 只信注册表(:749-762)。#500 含创建路径视角,作 canonical。
- **#503 → #506**(同一根因):D4(77b43b3)的 no-op 谓词问「**target** 有没有版本」,正确问题是「**本包** 有没有注册过版本」。两个上报场景里 target 都持有*别的包*批次注册的版本(mingw-w64 注册 gcc@15.1.0;gcc 注册 cpp),谓词均不触发,v2026.8.9.2 上仍复现。#503 第二面(无依赖 config 包静默跳过 uninstall 钩子)已并入 #506 说明。
- **#419 → #423**:#419 特有部分(legacy 拷贝、hook 内删除失效、stamp)已随声明式资产迁移(xim-pkgindex#431)退役;存活症状(卸载后悬空头文件链接)与 #423 完全重合,且影响面扩大(删一次 glibc ≈ 130 个悬空链接)。

## 4. 保留的 15 个存量缺陷——根因与修复落点

### 安装器/接口层

**#513 install hook 失败只剩空 E_INTERNAL**(high)
四层皆吞:executor 对 `return false` 不产生错误文本且从不填 `HookResult.output`(libxpkg xpkg-executor.cppm:757-768);installer 只 `log::error`+空 message(installer.cppm:2751-2758);`log::error` 在 interface 模式被 TUI 闸吞、`log::set_file` 全仓零调用;Lua 侧 io.write 直写 stdout 污染 NDJSON。→ executor 捕获 hook 输出 + 空错误给默认文案 + 输出并入 status/LogEvent;需 libxpkg 发版 + bump pin。**它同时吞掉 #514 的诊断,是本域可调试性的放大器,建议先修。**

**#514 install_dir() 对仅存于 mcpp shared registry cache 的依赖返回 nil**(high)
三条解析路全断:mcpp 级依赖不进 xlings plan(resolved_deps 无记录)、cp 种入 cache 无 xvm 记录、scan 三处均为项目本地路径(pkginfo.lua:131-160)。→ 方向 B(更合设计):mcpp 复用 cache 时写 resolver record,走单一答案源;或环境变量注入额外搜索根。跨仓 libxpkg+mcpp。

**#464 plan_install 缺 targets → 空计划 exit 0**(high)
schema 声明了 required 但 execute 只 `contains` 判断(capabilities.cppm:87,92-102);interface 分发层无集中校验(interface.cppm:271);#374 兜底只对非零退出生效。→ 在 `cap->execute` 前按 `spec().inputSchema.required` 集中校验,缺失发 ErrorEvent{InvalidInput}+非零;test_interface_protocol.cpp 加断言。典型 silent-success。

**#376 解压失败缺语义码/hint**(PARTIAL)
消息已能上线(cf9b60d 的 Failed→ErrorEvent),但 `ExtractError.kind` 不参与上报(installer.cppm:2578-2594),无 InvalidInputArchive→E_INVALID_INPUT / LocalWriteFailure→E_DISK_FULL 映射(枚举两码俱在)。→ kind→ErrorCode 映射 + 分 kind hint。

### 索引/下载/更新

**#493 --index-repo 覆盖主索引「接受但不生效且零输出」**(high)
`config` 只校验 name:url 格式;下次 update 时 local 源跳过 artifact fetch,但 `repo.cppm:615` 因磁盘上 stale 的 `.xlings-index-version` marker 把条目当「artifact 托管勿动」静默 continue,本地接管永不发生。→ 当场拒绝(cli.cppm:526 对 name==xim 且非官方远端)或让接管真发生(615 谓词补 mainIsOfficialRemote + 清 marker)。消灭第三态。

**#417 CN 镜像 HEAD 恒 401,freshness 探测永久死亡**(high;今日实测 gitcode HEAD 401 / GET-Range 302→206 依旧)
`query_remote_meta` 无条件 HEAD、无降级(tinyhttps.cppm:451-485);探测与 sidecar 补写两个调用点都死。→ 401/403/405 降级 GET + `Range: bytes=0-0`,总长从 Content-Range 解析;修一处两点受益。

**#425 新版提示永不触发**(PARTIAL)
#477 已把 `client_latest` 随每次 update 免费带回并写好比较+文案(indexfetch.cppm:692-696,746-754),但唯一调用点在 `!choice->isNewest` 降级分支——线上 requires 全空,分支现实中不触发。→ 把比较提出来,在主索引 fetch 成功后 sync 层无条件打一次;成本一行、零额外网络。

### subos / 移除语义

**#500 裸 install 造半 subos(并 #491)**(high)
物化(installer.cppm:1979,2055-2070)与注册(init.cppm:266-291,仅 self install/init、subos new、migrate 可达)分离;list 合成 vs use 注册表两个答案源。→ 首次物化前补登记,或两读者共享谓词;fresh e2e 补 `subos use default` 断言(现恰停在临界点前)。

**#506 remove 谓词 target-vs-package 错位(并 #503)**(medium-high)
详见 §3。→ `installer.cppm:3153-3163`:「target 现存版本全部属于其他 provider」等同「无版本」,走 no-op+hook 分支(bindingGroup->provider 可查,registration.cppm:971-972);验收 = 删 windows-test.ps1:332-366 委托容忍(posix 侧 #577 已删,Windows 侧从未对 gcc changed-set 跑过)。连带覆盖 config 包钩子静默跳过。

**#423 files 资产完整卸载不清盘(并 #419)**(high)
完整卸载只清 lib(:737)/program(:788)两类;detach 路径 `file_placement` 按包名 key 而 files 节点注册为 `<pkg>.files.<n>`,恒空(installer.cppm:1503-1506)。`use` 切换路径已会逐成员处理 files(xvm/commands.cppm:477-483)——数据模型够用,卸载侧没接线。→ 在 :3343-3353 旁加 kind=="files" 清理(遍历 removalResult->removed,dbBeforeRemoval 的 fileDst);测试用 `-xtype l` 计数,勿用 `-e`。临时缓解:doctor --fix SysrootDangling。

**#442 detach 只摘具名 target,同 release 成员滞留**(high)
`detach_current_subos_` 单 (target,version) 签名,两个调用点都只传 detachTarget;`removalContext.members` 就在手边(:3176)却只用于校正版本。滞留成员导致另一 subos 最后一次完整移除误删 payload,留下指向已删载荷的 workspace 残条。→ stillReferenced 分支遍历 members 逐个 detach;e2e fixture 补双 target(仿 patchelf+elfpatch)。

**#458 doctor 看不见 payload 内烘焙的 subos 路径**(high)
doctor 三类检查(xvm 注册值、ELF 头、getent)都触不到 payload 内文本(gcc specs);全 src 无读 specs 代码。→ recipe 声明 `payload_config_files` + doctor 对声明文件做 subos 路径子串扫描;长线并入 closure 规则 C(该 issue 即规则 C 的存量数据面)。

### 质量基建 / 平台

**#433 无 sanitizer 构建**(high)
11 个 workflow、全部构建配置零 `-fsanitize`(实测 grep);现防线只有 db.cppm:463-465 的 deleted 右值重载(只挡一种写法)。→ Linux job:glibc 工具链(musl+static 与 ASan 不兼容)+ `-fsanitize=address,undefined` 跑 `mcpp test`;前置确认 mcpp 旗标注入;可仅 push-to-main。

**#427 e2e 手工注册清单**(PARTIAL,实测 103 个测试 / 88 被引用 / 孤儿 15)
孤儿 18→15 全靠 issue 当天预告的 #426;深层缺陷未动且复发实证:E2E-65/66 自 2026.8.8.2 起漏注册一个发布周期,0b06b07 才补。→ run_all.sh 改 glob+显式 SKIP,或加清单守卫(差集非空即 FAIL);汇总行加 SKIPPED 计数。

**#153 macOS 缺 install_name_tool 时 fail-open**(high)
`_patch_macho` 缺工具 warn 后 return,安装照报成功(elfpatch.lua:592-594);CI 绿只因 runner 预装 CLT;零工作量时也告警(检查先于 targets 判空)。→ 工具检查移到 targets 判空后;有实际工作时缺工具应失败(对齐 C2「宁可拒绝不可假成功」);安装路径/doctor 预检 `xcode-select -p`。

## 5. 保留的功能请求与信息帖

- **#206 内建「系统 subos」**:default 恰是安装落点(与请求相反);空 subos 的 PATH 重建已保证 ≈ 宿主环境,缺内建保留名 + per-subos `locked` 字段 + cmd_install 入口检查——现在做比当年顺。
- **#467 aria2 可插拔下载器**:下载路径硬连 tinyhttps,无抽象/分片/续传(downloader.cppm:610-613 自注 future);若做,注意无 sha256 包(47/118)经外部下载器的完整性链设计。
- **#501 rootfs 隔离**:sandbox 现按设计 ro-bind 宿主 /usr;closure 主线(形态 X)正是物理前提,空-host bwrap 已作为索引侧自检工具跑通;落点 = sandbox.cppm bind 清单加「空 host」模式,以 closure_check 通过为准入。
- **#2 信息汇总帖**:保留;正文链接仍是 d2learn 旧口径,建议刷新。

## 6. 横切观察(修上面这些时值得知道的形态)

1. **target-vs-package 谓词错位**是本轮最有含金量的单点发现:#506/#503 两个「不同」的 bug 是同一行谓词的两张脸;修谓词一处,两案皆平。
2. **「一个问题,多个回答者」仍在产生新案例**:#500/#491(list 合成 vs use 注册表)与历史上的 doctor/repairer 漂移同构;收敛答案源比修表象便宜。
3. **interface 模式日志无出口**(log.cppm TUI 闸 + set_file 零调用)是一条放大链:#513 吞掉了 #514 特意加的诊断——修 #513 前,任何「加条日志」型修复在 mcpp 路径下都不可见。
4. **silent-success 家族又添三员**:#464(缺字段=无事可做)、#503 残留(钩子静默跳过)、#153(缺工具照报成功)。与 C2「宁可拒绝」的口径统一起来是一条主线。
5. **声明式迁移会搬运缺陷的数据面**:#419 的 legacy 机制退役了,但症状原封不动地变成了 #423 的案例,且规模×130。迁移完成 ≠ 症状消失。
6. **手工注册清单必然复发**:#427 在 issue 提出后的两周内就复发了一次(E2E-65/66);没有守卫的清单不会靠自觉变完整。
7. **重写会丢行为**:#104 的宿主探测在 Lua→C++ 时无声消失,一年后才被注意到。被删除的「已修复」需要清单化对账。

## 7. 建议优先序

- **P1(低成本高杠杆)**:#513(解放整个安装域的可调试性)→ #506 谓词(一行修两案)→ #423(数据模型已就位,只差接线)→ #464(集中校验一处)。
- **P2**:#500(补登记)、#417(GET 降级)、#442(members 遍历)、#425(提示提出降级分支)、#427(清单守卫)。
- **P3(需设计/跨仓)**:#514(mcpp 协同)、#458(recipe 声明 + 规则 C)、#493(口径裁决)、#433(mcpp 旗标)、#153(strict 化)。

## 8. 验证方法与可信度

- 8 个并行调查域的结论,**25 个关闭决定全部经主线二次复核**:commit 存在性(git log)、代码行现状(sed/grep 当前树)、CI run conclusion(REST API)、线上行为(gitcode HEAD/GET、xim-index 指针)、外部仓库事实(xim-pkgindex 4e6d7634 确实 touch gcc.lua;d2learn/d2x 确有 editor/assistant.cppm)。
- 两处 agent 结论被复核修正口径:#130 的「quick_install 忽略 XLINGS_HOME」为 0.4.6x 旧行为(0c362cf 后失效,相应 memory 已过时);#155 引用的索引 commit 4e6d7634 信息与实测吻合后才写入公开留言。
- 保留缺陷均给出 file:line 级根因;其中 #427/#433 为当日实测计数(103/88/15;11 workflow 零 sanitizer)。
- gh 操作注记:挂过 classic project card 的老 issue(如 #54)会让 `gh issue view` 死于 GraphQL projectCards 弃用错误,全部处置改走 REST(`gh api repos/.../issues/...`)。
