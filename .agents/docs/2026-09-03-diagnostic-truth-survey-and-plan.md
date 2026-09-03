# 诊断与守卫的「真值」排查 —— 从三条报错到一类问题

> 起点:用户贴出的 `self doctor` / `install` 真实输出,三个问题:
> ① remedy 写成 `xlings install xim-x-xxx@version`;② `--force` 不存在;③ 为什么报错。
> 分支 `fix/musl-loader-family-misread`,基线 `ba51901`(发布版 2026.9.3.1)。
> 状态:**调研 + 方案,待 review**。§6 的 P0 已在分支上完成并通过 51 套单测与 3 个受影响 e2e;
> P1 起未动手。

## 0. 一句话

这次排查到的问题都是一个形状:**守卫或诊断回答的是一个比真实问题更弱(或不同)的
代理问题,然后把答案当作真实问题的答案打印或执行**。

| 真实问题 | 实际回答的代理问题 | 后果 |
|---|---|---|
| 这个 core 文件属于哪个 libc 家族 | 解析符号链接之后的**文件名**像什么 | musl 的 loader 是 `libc.so` 的链接 → Unknown → 与 glibc 比较 → 假 split → **拒绝安装** |
| 加载器实际会从哪个载荷取 libc | RUNPATH 里**有没有任何**目录含别的载荷的 core 文件 | 真实 home 152 条 error,**0 条为真**;同一谓词是安装期硬拒绝 |
| 用户该敲什么命令 | 拿手头的字符串(store 目录名 / 程序名 / 想象中的 flag)拼一条 | 复制即失败;338 个程序名里 218 个不是包名 |
| 这次 `self update` 有没有换到索引构建 | 激活键里**有没有冒号** | 2026.9.2.1 起键按身份拼写,index 安装也可带 `xim:` → 假「nothing was upgraded」(#579) |

memory 里早有这个形状的名字:[[proxy predicate, discarded half]]、[[gate the message on behaviour]]。
本次不同之处在于**同一个代理谓词同时驱动报告与拒绝**:doctor 的噪声与 install 的误拒是一个函数。

## 1. 用户的三个问题,逐条

### ① `xlings install xim-x-bun@1.3.11` —— 拼的是 store 目录名

`doctor.cpp:1779` 用 `root.target`/`root.version` 拼 remedy,而整库扫描时 `root.target` 是
`data/xpkgs/` 下的**目录名**(`xim-x-bun`),不是包坐标。实测:

```
$ xlings install xim-x-bun@1.3.11        → "not found in the synced index"
$ xlings install xim:bun@1.3.11          → 解析成功
```

修法已在仓里:`xvm::coordinate_from_payload_path` 就是为「从 store 路径反推可安装坐标」写的。
新 remedy:`xlings remove xim:d2x@0.1.3 -y && xlings install xim:d2x@0.1.3 -y`。
为什么是 remove+install:`install` 对已注册且载荷在盘的包打印 already installed 什么也不做,
「重装」在 xlings 里不是一个 flag 而是两步。坐标解析失败时 remedy 留空 ——
`Finding::remedy` 的契约允许空,不允许错。

### ② `--force` —— `install` 没有这个选项

`cli.cpp:1589-1626`:`install` 只有 `-g/--global`、`-u/--use`,加全局 `-y`。`--force` 是
`remove` 的(含义是「有依赖者也删」)。实测 `xlings install zz@1.0 --force` →
`Error: unknown option: --force`,rc=1。

同一字符串在 **两处**:`doctor.cpp:1779`(已修)和 `xvm/commands.cpp:521`
(`runtime activation refused … hint: reinstall it with xlings install {} --force`,已修为
`xlings install {}` —— 该分支的前提就是「已注册但载荷缺失」,这正是安装器会重跑 install hook
的状态,裸 install 就是重装)。`grep 'install .* --force' src` 现为空。

### ③ 为什么报错 —— musl 的 loader 是 libc 的符号链接

```
xim-x-musl/1.2.5/lib/ld-musl-x86_64.so.1 -> libc.so
```

`core_runtime_sources_` 只保留 `weakly_canonical` 之后的路径;`core_family_of_path_("libc.so")`
既不匹配 glibc 模式也不匹配 musl 模式 → `Unknown`;而家族守卫的规则是「任一侧 Unknown 仍比较」
(为了不放过不认识的载荷)。于是每一个 RUNPATH 触及含 musl 的 subos lib farm 的 glibc 二进制
都被判 split,`installer.cpp:3017` 的硬守卫**拒绝安装**——用户看到的
`xlings install xim:bun@1.3.11` 在 node@24.20.0 上失败就是这条。

修:`struct CoreSource { path source; CoreFamily family; }`,家族按**目录条目名**判定,
解析路径只用作身份与报错。实测真实 home:doctor 178 → 152;`--scope node@24.20.0` 1 → 0。
单测 `SameSource.AMuslLoaderSymlinkedToLibcIsStillMusl` 用真实形状(符号链接)建 fixture ——
原来两条 musl 测试 `touch` 的是普通文件,fixture 长得不像它代表的东西,所以从没抓到。

## 2. 顺着查:剩下的 152 条也全是假的

真实 home,分支二进制,`self doctor --deep --all`(4.4 s,470 个载荷):

```
error 155 = loader/libc split 152 + broken payload 2 + binding state 1
summary:  broken payloads 152        exit 1
```

152 条**同一形状**:

```
interpreter -> xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
RUNPATH     -> …/xim-x-glibc/2.39/lib64 : … : ~/.xlings/subos/default/lib
core file   -> xim-x-glibc/2.44/lib/ld-linux-x86-64.so.2     ← farm 今天指向 2.44
```

涉及 18 个包(gcc 71、binutils 29、llvm 17、ncurses 7、cmake 5、bun 5、slang 4、node 3…)。
直接运行其中 10 个(gcc 11.5.0、d2x、cmake 4.0.2、node ×7)**全部 rc=0**。

按加载器真实语义重算 —— 对每个 core soname(`ld-linux-*`、`libc.so.6`、`libm.so.6`、
`libpthread.so.0` 等 8 个)沿 RUNPATH **顺序取首个命中目录**,与 interp 载荷比较:

```
flagged binaries 151   ordered per-soname mismatches 0
```

### 根因:谓词是「顺序盲」的,而且有单测钉着

`elf_same_source.cppm::check`(2026-08-10,`65002df` 引入)明写:

> Every proven core-runtime entry must agree: a later directory may supply the loader or
> another core component even when libc.so.6 was found earlier, so one same-source entry
> cannot wash out another mismatch.

单测 `SameSource.AnyDifferentCoreRuntimeEntryViolates`:RUNPATH `[2.44(同 interp), 2.39]` →
必须 violated。这个理由只在**首个目录不完整**时成立(首目录有 `libc.so.6` 没 `libm.so.6`,
`libm` 才会从后面的目录来)。`xim-x-glibc/<v>/lib64` 永远是完整的,所以真实 home 上这条规则
的全部输出都是假阳性。fixture 每个目录只 `touch` 一个 `libc.so.6`,区分不出「完整首目录」与
「残缺首目录」,于是把一个更强的策略当成了加载器语义。

### 三重后果

1. **doctor**:这台机器上 98% 的 error 是噪声;真 finding(2 条 broken payload、1 条 binding
   state)淹在 152 条里;summary 与 exit code 都被假阳性决定。
2. **install**:同一函数是 `installer.cpp:3017` 的硬拒绝。何时误拒:subos 激活的 glibc ≠
   farm 实际指向的 glibc(漂移,见 #529),或 recipe 钉了另一版 glibc,或 musl(已修)。
3. **remedy**:让用户对 18 个包 / 152 个二进制做 remove+install —— 修一个不存在的问题,
   而重装后 INTERP 变 2.44,行为反而改变。

### 152 条背后的真信息,doctor 没说

`default` 声明并激活 glibc **2.44**(workspace 仅 `['2.44']`),但 152 个二进制的 INTERP 指向
**2.39**(2026-05 安装时的 runtime;2.39 仍注册在另外 21 个 subos)。它们能跑,是因为自己的
RUNPATH 先于 farm。这是 #529 的「声明 vs 实际」漂移的逐二进制版本 —— D6 只比 farm 与声明,
不看每个二进制的 INTERP。如果有一天 2.39 被从最后一个 subos 移除,这 152 个二进制会以
`ENOENT` 死掉且报错指向二进制本身([[ENOENT names the binary]])。**该报的是这条,而不是 split。**

## 3. 安装守卫的契约漂移

`xim/commands.cpp:865` 的注释是契约:

> installer.execute itself only returns unexpected on cancel or plan-level errors.
> Per-package failures … accumulate in failedCount.

Rule-B 守卫(`installer.cpp:3017-3027`)对**单包**失败 `return std::unexpected(...)`:

| 契约要求 | 守卫实际 |
|---|---|
| 单包失败 → `onStatus(Failed)`,其余节点继续 | 整个 plan 中止(bun 因 node 死) |
| 失败以 `ErrorEvent{recoverable=true}` 逐包上报 | `ErrorCode::Internal`,`recoverable=false` |
| `install_summary` 事件 | 不发 |
| install/config hook 失败写 payload failure marker(#541 ①) | 不写 marker;载荷留在盘上 |

重试行为我先假设是 fail-open(目录非空 → 视为已装 → 跳过守卫),**量过是错的**:
`payloadInstalled` 由 versions DB 注册决定,被拒节点未注册 → 重试重新走 install hook 与守卫,
确定性再拒。留在盘上的是一个无 marker 的「有载荷、无账」目录。

## 4. 打印出来的命令:一类,不是两处

盘点 `src/` 里所有拼出 `xlings …` 的字符串(~60 处),不可运行的风险分四型:

| 型 | 例 | 站点 | 状态 |
|---|---|---|---|
| A 不存在的 flag | `install … --force` | doctor.cpp:1779、xvm/commands.cpp:521 | **已修** |
| B store 目录名当坐标 | `install xim-x-bun@1.3.11` | doctor.cpp:1779 | **已修** |
| C 程序名当包名 | `install g++` / `install -g slang` | errors.cpp:331/342、shim.cpp:730、xvm/commands.cpp:970、xim/commands.cpp:2026 | 未修 |
| D remedy 字段夹散文 | `xlings self doctor --fix   (adopt …; or migrate with …)` | doctor.cpp:462/548/593/617 | 未修 |

C 型量化:真实 home versions DB 338 个 `program` 条目,**218 个不是索引包名**
(`g++`、`ar`、`as`、`c++`、`7z`、`aarch64-linux-musl-*` …)。对这些名字,
`xlings install <name>` 必然 not found。`errors.cpp:320` 的注释已经承认了 `slang` 这一例,
但只修了「项目提供」那一支,`install -g {target}` 与 `install it here` 两支照旧。

D 型:`diag::Diagnostic` 早已把 `label`(散文)与 `command`(可执行)分开
(`diag.cppm:63-64`),`doctor::Finding::remedy` 只有一个字符串,散文只能塞进命令里。

## 5. 相邻的同类 open issue(一起排,不重开)

- **#579** `self update` 假报「nothing was upgraded」+ 假 DOWNGRADED:`update_landed_on_index_build`
  以「键里有冒号」判定 provider;2026.9.2.1 起键按身份拼写。真实 home 8 个 active 值带 `xim:`。
  另 `entry_binary.cpp:91` 比较前未 strip namespace。
- **#578** `remove <name>` 经目录解析到索引 latest 而非已装版本;fresh home 里 `remove` 先要索引
  (实测 `remove zz@1.0 -y` → `package index not available`)。
- **#529** install 路径不调 `check_runtime_activation`(今天仍只有 `use` 与 doctor 两个调用点);
  §2 的 152 条正是它的产物。
- **#531** 「存在但不支持本平台」被报成 not found。
- **Windows 派发** `shim.cpp:890-897`:非 POSIX 走 `platform::exec` → `cmd.exe /c`
  (2026.9.3.1 notes §8 已记,未开 issue)。
- **撤回**:先前记的「GitCode 索引指针 404」今天未复现 —— `xim-index-pointers.json` 两端 200
  且内容一致;`xim-index-adeba41.tar.gz` 在 `vadeba41`、`latest` 两个 tag 下 GitHub/GitCode 均 200。

## 6. 方案(按收益/风险排序;P0 已完成)

### P0 · 本分支已完成 —— 用户的三个问题

- [x] 家族按条目名分类(`CoreSource`);单测用真实形状(符号链接)
- [x] remedy 走 `coordinate_from_payload_path`,不可解析则留空
- [x] 两处 `install --force` 改为可运行命令
- [x] e2e S13:所有打印的 remedy 不含 `--force`、不含 `<ns>-x-<pkg>@`;零 remedy 时自报 NOT EXERCISED
- [x] 51 套单测、`self_doctor_test` / `loader_libc_same_source_test` / `shim_table_routing_test` 通过

**验收已量:** 真实 home 178 → 152;node@24.20.0 1 → 0;`xim:d2x` 可解析而 `xim-x-d2x` 不可。

### P1 · 同源检查改为加载器语义(收益最大,改动最小)

`check()` 改为**逐 core soname、按 RUNPATH 顺序首命中**:

```
for soname in core_set:                   # ld-linux-*/ld-musl-*, libc.so.6, (可扩 libm/libpthread/…)
    winner = first dir in RUNPATH order that contains soname
    if winner.payload != interp.payload → violated (names soname, dir, both payloads)
```

- 保留原理由里**真实**的那一半:首目录残缺时,后续目录才对缺的 soname 计入。
- `AnyDifferentCoreRuntimeEntryViolates` 拆成两条:完整首目录 + 后续异源 → **pass**;
  残缺首目录 + 后续异源补齐 → **violated**。`FirstCoreRuntimeEntryDeterminesResult` 不变。
- `core_set` 是否扩到 libm/libpthread:glibc 2.34+ 已合并进 libc.so.6,但旧 glibc 分开;扩了更严,
  且不会引入假阳性(仍按首命中)。建议扩。
- **差分验收**:真实 home slice 上 152 → 0(已用脚本预演,见 §2);E2E-62 仍过;
  安装守卫在 farm 漂移下不再拒绝(e2e:farm 指 2.44、recipe 钉 2.39 且 RUNPATH 自带 2.39 → 安装成功)。
- 不影响的:`HostLoaderPayloadCore` 反向规则不动;家族守卫不动。

### P2 · 安装守卫回到契约

- Rule-B 拒绝改为**逐节点失败**:`onStatus(Failed)` + 加入 `refusedNodes` + 写 failure marker;
  其余节点继续;`install_summary` 照发;退出码由 `failedCount` 决定。
- 报错文案里的「report it with the two paths above」保留,但补一句「其它包已继续安装」。
- 差分验收:e2e 造一个被拒节点 + 一个正常节点的 plan,断言正常节点装上、退出码 1、
  `install_summary.failed == 1`。旧二进制上正常节点装不上。

### P3 · remedy 契约:可执行,或空

- `Finding::remedy` → `{ std::string command; std::string note; }`,与 `diag::Diagnostic` 对齐;
  渲染层 command 单独一行,note 跟在后面。D 型四处迁移。
- C 型:程序名 → 包坐标经 `xvm::owner`(已有 `install_command()`)解析后再拼;解析不到则
  给 `xlings list <name>` / `xlings use <name> --all` 这种**一定能跑**的探查命令,不给 install。
- 门:S13 扩为「每条 remedy 的 command 经 `spec::validate_manual_argv` 干解析通过」;
  CI grep 禁止 `install [^"]*--force`。

### P4 · 把 152 条背后的真信息说出来

新 finding `InterpRuntimeDrift`(Notice):「subos `<n>` 声明 glibc 2.44;N 个二进制(M 个包)的
INTERP 指向 2.39,它们靠自身 RUNPATH 运行;2.39 若从最后一个 subos 移除,这些二进制将无法启动」。
`--fix` 不动它(改 INTERP 只能重装);remedy 给包清单。与 #529 一并处理 —— 那边是「声明该不该
随安装变」,这边是「变了之后怎么让人看见」。

### P5 · #579

`update_landed_on_index_build` 改为比较**身份**(strip namespace 后与本次安装记录的版本比),
或直接读安装刚返回的坐标;`entry_binary.cpp:91` 比较前 strip namespace;最终「nothing was
upgraded」在 `use` 之后**重读状态**再判。已在 issue 里列了三条,不重复。

### P6 · 已有 issue,按期排

#578、#531、Windows `std::system` 派发(`CreateProcessW` + 空 `SetConsoleCtrlHandler`)。

**版本与打包建议:** P0+P1+P2 一个 PR(2026.9.3.2 或 2026.9.4.1):三者都是「守卫说了假话」,
且 P1 是当下真实 home 上最大的可见缺陷;P3/P4 第二个 PR;P5/P6 各自随 issue。
均**不跨仓库**:无新 libxpkg 字段、无索引格式变化、无 recipe API 变化。

## 7. 不变量(测试要锁的,不是版本号)

1. **一个 remedy 要么能原样运行,要么为空。** 门:S13 + 干解析。
2. **同源判定 = 加载器会做的选择。** 门:完整首目录/残缺首目录两条单测 + 真实 home slice 152→0。
3. **守卫拒绝一个节点,不拒绝一个 plan。** 门:P2 e2e。
4. **fixture 长得像它代表的东西。** musl 的 loader 是链接;glibc 的 lib64 是完整的。
   两次假阳性都是 fixture 失真放过去的。
5. **报告与拒绝共用一个谓词时,先用报告量,再让它拒绝。** Rule-B 2026-08-10 引入时直接是硬拒绝;
   `closure_check` 的 rule D/A/E 反而是 WARN-ONLY 先收集(C2.5)。顺序反了。

## 8. 本次被测量推翻的我自己的判断

- 「守卫在重试时 fail-open」 → 错。`payloadInstalled` 由注册决定;重试重新评估。
- 「GitCode 索引指针 404,CN 客户端靠跨区回退」 → 今天两端都 200 且一致;撤回。
- 「`--force` 让 install 以 exit 2 退出」 → rc=1,`Error: unknown option: --force`;注释已改。
- 「152 条 split 里总有真的」 → 逐二进制运行 + 逐 soname 首命中重算:0。

## 9. 附:本次的度量方法(可复跑)

```bash
# 真实 home,只读
target/<fp>/bin/xlings self doctor --deep --all > doctor.txt
grep -c '→ run' doctor.txt                                  # remedy 数
grep -oE 'xlings remove [^ ]+' doctor.txt | sort | uniq -c   # 涉及的包
# 对每个被标记的二进制:patchelf --print-interpreter / --print-rpath,
# 逐 core soname 沿 RUNPATH 首命中 → 与 interp 载荷比较(脚本见会话 scratchpad)
# 程序名 vs 包名:versions DB 中 type==program 的键 ∩ data/xim-pkgindex/pkgs/*/<name>.lua
```
