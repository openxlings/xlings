# 项目安装的 shim 泄漏进全局 —— 根因、约束推导与设计方案

> 状态:**已实现**(2026.9.3.1,分支 `fix/shim-routing-vs-state`)。基线 HEAD `7c35579`
> (发布版 2026.9.2.1)。实施中被实测推翻/修正的判断见 §3(推翻 6-9),逐条标注而不是悄悄改掉。
> 所有数字都在真实 home(`/home/speak/.xlings`)上**只读**实测,命令写在各节的「实测」里。

**Goal:** 让「一个名字归 xlings 路由」和「某个 scope 激活了这个名字的某个版本」成为两件
分开表达的事,从而消灭项目安装在全局 bin 里留下的、不可见也不可回收的悬空名字。

**Non-goal:** 让项目安装在全局**完全不留任何文件**。§4/§5 论证这在 cmd.exe 上不可达,
且支持 cmd.exe 的八个同类工具无一做到。

---

## 1. 现象

用户报告:项目目录有 `.xlings.json`,全局没装工具 `xxx` 但项目 workspace 里有,
`xlings install` 之后全局空间被污染,项目外用会报「找不到」或没有用激活的版本。

### 1.1 机制(代码事实)

PATH 上只有一个工具目录 —— `src/core/xself/profile_resources.cppm:87`:

```sh
XLINGS_BIN="$XLINGS_HOME/subos/${XLINGS_ACTIVE_SUBOS:-current}/bin"
```

项目 subos 的 bin(`<proj>/.xlings/subos/{_,<name>}/bin`,`config.cpp:348`)**从来不在
PATH 上**。所以有一段代码专门把项目装的 shim 复制进全局 bin —— `src/core/common.cpp:11`,
注释写得很直白:

```cpp
// Mirror a shim to global subos bin when in project context.
// Project subos bin is not in PATH; global subos/current/bin is.
// Only creates if missing — never overwrites existing global shims.
void mirror_shim_to_global_bin(const fs::path& xlings_bin, const std::string& shim_name) {
    if (!Config::has_project_config()) return;
    ...
}
```

三个调用点:`xim/installer.cpp:2009`(install)、`xvm/commands.cpp:827`(use)、
`xim/libxpkg/types/script.cpp:74`(script 包)。写入 `~/.xlings/subos/<globalActiveSubos>/bin/`。

而 shim 的解析是随 cwd 变的 —— `xvm/shim.cpp:409` 走 `Config::effective_workspace()`,
项目内读项目 pin,项目外读全局 workspace。于是同一个文件要回答两个不同的问题。

### 1.2 实测:真实 home 上的泄漏

```bash
# 84 个 subos;default/current 的 bin 里有 122 个文件,其中 23 个全局从没装过
python3 - <<'EOF'
import json,os
p=os.path.expanduser("~/.xlings/subos/default")
j=json.load(open(os.path.join(p,".xlings.json")))
ws=j.get("workspace",{}); act=ws.get("active",ws); inst=ws.get("installed",{})
files=sorted(os.listdir(os.path.join(p,"bin")))
orph=[f for f in files if f not in act and f!="xlings"]
print(len(orph), orph)
print([f for f in orph if f not in inst])   # 连 installed[] 都没有
EOF
```

结果:

```
23 个:brew cl lib libcxx-headers libxkbcommon link musl-cross-make
      qemu-aarch64-static qemu-arm qemu-riscv qemu-system-{aarch64,arm,riscv32,riscv64}
      qemu-user-aarch64 rc rustup slang slangc slangd slangi xvm xvm-shim
其中在 installed[] 里也没有的:全部 23 个
```

全是指向 entry binary 的 symlink:

```
$ ls -la ~/.xlings/subos/default/bin/slang
lrwxrwxrwx ... /home/speak/.xlings/subos/default/bin/slang -> ../../../bin/xlings
```

项目外运行的真实行为:

```
$ cd /tmp && slang --version
[error] slang is not installed in this subos (default)
          install it here   xlings install slang
$ echo $?
1
```

那句 remedy 是错的 —— 用户从没要过 slang,是某个项目要的;而 `xlings install slang`
在全局未必是个有效包名(slang 来自一个 release group)。

---

## 2. 根因:一个目录承担两个含义

`~/.xlings/subos/<n>/bin/` 里一个文件的存在,今天可能来自两个完全不同的原因:

| 含义 | 谁写 | 谁读 | 作用域 |
|---|---|---|---|
| **路由** 这个名字归 xlings 接管 | `mirror_shim_to_global_bin`(**project scope** 的决定) | PATH / shell profile | 全局(PATH 是全局的) |
| **状态** 这个 subos 激活了它 | installer / `use`(**global scope** 的决定) | `shim.cpp:409` dispatch、`doctor.cpp:838` Check 2 | scope 化 |

shim 文件本身是 entry binary 的一个 symlink/hardlink,**不携带版本、不携带来源** ——
它天然只能表达「路由」。是读者硬把它读成「状态」。后果是双向的:

1. **路由产生的文件被状态规则解析** → `xvm.not_in_subos`,项目外必然报错。
2. **路由产生的文件被状态过滤器排除** → 永远不回收。`doctor.cpp:853`:

```cpp
const auto* vi = xvm::get_vinfo(st.db, base);
if (!vi || !xvm::has_program_kind(st.db, base)) continue;   // ← 这里跳过了它们
```

mirror 把版本注册进的是**项目**的 versions DB(`Config::versions_mut()` 在
project scope 下返回 `projectVersions_`,`config.cpp:987`),全局 DB 从没听说过这些名字,
于是被当成「不是我们的文件」跳过。

`doctor.cpp:1145` 的 Check 2.6 **已经**知道项目 binDir 是特殊的:

```cpp
// Scoped to bin dirs physically inside the home -- a project subos binDir
// lives in the project tree, where shims intentionally anchor to the global home.
```

但 Check 2(orphan shim)没有这个认识。同一个 doctor 里两条规则对同一件事的理解不一致。

这是 memory 里 `one-question-many-answerers` 的又一例,也是
`reporter-repairer-predicate-drift` 的近亲。

### 2.1 状态其实已经隔离了

值得明确:除 shim 外,项目安装**不写任何全局状态**。
- sysroot header:`installer.cpp:1439` → `p.subosDir / "usr" / "include"`
- file assets:`installer.cpp:1913` → `Config::paths().subosDir / file.destination`
- workspace / versions:`config.cpp:1243`(`save_versions`)、`config.cpp:1330`(`save_workspace`)都按 scope 分流
- 唯一全局的是 payload store `data/xpkgs`(`installer.cpp:816`)—— 内容寻址的共享仓库,是设计不是污染

**所以要修的只有 shim 这一处。**

---

## 3. 过程中被实测推翻的判断

按 repo 惯例逐条标注,不悄悄改掉。

**推翻 1:「doctor 会把这些判成 orphan,`--fix` 会删掉,于是项目里的工具跟着消失」——错。**
实测 `cd /tmp && xlings self doctor` 一个都没报。原因是 §2 的 `has_program_kind` 过滤。
真实情况**更糟**:不是被误删,是**永久泄漏、无人可见、无人回收**。

**推翻 2:「一个名字要被任意子进程按 PATH 找到,必须是 PATH 上某目录里的真实文件」——说得太满。**
PATH 条目可以是相对路径,那样 PATH 静态不变而解析随 cwd 走。实测(§4.2)证明它在项目根
目录下对 `make` / `execvp` / `dash` 全部有效。这是第四类机制,我漏了。但它只在项目**根**
生效,且引入 PATH 劫持面,所以不能单独成方案。

**推翻 3:「不用 cd hook 是因为跨平台,而 xlings 只发三套 shell profile,所以成本不高」——
前提不成立。** 用户指出 Windows 上真实存在 cmd.exe 用户。cmd.exe 没有任何 cd hook,这条
约束是硬的,§5 的外部调研从八个工具的实践确认了它。

**推翻 4:「新建 `~/.xlings/shims/`,PATH 变常量」——自相矛盾,已撤回。**
`subos use` 的默认模式是**每个 shell 一份**(`subos.cpp:1739`,profile 从
`XLINGS_ACTIVE_SUBOS` 算 `XLINGS_BIN`)。常量 PATH 意味着一张全局路由表,而两个终端可以
同时处在不同 subos —— 表里只放全局 active subos 的名字,另一个终端的工具就没有文件;
放全 subos 并集,就回到我自己否掉的 273。**常量 PATH 买不到任何东西**:今天的 PATH 已经
按 subos 变且工作正常。新目录同时作废,见 §6.1。

**推翻 5:「rehash 删文件前必须确认」——过度补偿,已撤回。**
实测那 23 个泄漏名字在**整个 home 的 84 个 subos 里**的 active 情况:22 个在任何地方都没有
active version(今天敲它们只能得到错误),只有 `rustup` 在 `gfxbuild` 里 active。确认框的
前提是「用户有真实的选择」,而删掉它们前后能做的事完全一样(都是报错)。改为**删,并打印
清单,不问**(§6.4)。

**推翻 6:「desired 集合要探针 payload,只放 dispatch 能服务的名字」——错,已撤回。**
被 `test_xvm_bindings` 的 production-path 子进程抓到。载荷坏掉、或布局不在
`resolve_executable` 的探测范围内的包会**静默离开表**,用户拿到的是 shell 的
`command not found`——或者更糟,透传到宿主机的同名程序——而不是 dispatch 那条带路径和
重装命令的 `executable 'X' not found`。载荷损坏是运行时事实,dispatch 报得很好,
`self doctor` 也已经在查。#452 的虚拟根由 registration 的 `anyRealUncontested` 在
**激活**这一层挡掉,轮不到表来问;而且实测那 23 个泄漏名字**全部**被 `active` 一条排除,
探针从来就不是抓住它们的东西。

**推翻 7:探针本身还错了两处**(同一个测试抓到)。它按 `VInfo::filename` 查可执行文件,
而 dispatch 用的是 **target 名**(`shim.cpp:682`);它还要求 **alias 包**在载荷里有二进制,
而 alias 包根本没有——dispatch 走 alias 命令,完全不经过 `resolve_executable`。

**推翻 8:「reserved 名字(`xlings`)双向保护」——只该保护删,不该挡加。**
两个方向的理由不同:`ensure_subos_shims` 会在一个从没装过该包的 home subos 里放
`xlings`,workspace 里没有任何东西为它背书,所以不挡删就会把「所有 shim 都指向的那个
文件」判成陈旧删掉;但 `xlings` 也是个真包,装进某个 subos 就该给它 shim。项目 subos
两者都不给——它的 workspace 里没有 `xlings`,它的 bin 也不在 PATH 上。

**推翻 9:`xlings_binary_in_home` 少了 bootstrap 回退。**
`self init` 之前二进制直接在 `<home>/xlings`,被删掉的三个写者站点都带这个回退,我漏了 ——
结果是「装了、active、一个文件都没有」。`self_doctor_anchor_shim_test.sh` 第一条断言就炸。
补进 `xlings_binary_in_home` 而不是另造一个局部答案。

---

## 4. 约束推导

### 4.1 command-not-found 兜底盖不住主场

设想:不放文件,靠 shell 的「命令找不到」钩子兜底。实测(`bash`,`/bin/sh` → dash):

```
command_not_found_handle 命中情况
  bash 直接调用        ✓ HANDLER-HIT
  bash -c 子进程       ✓ HANDLER-HIT
  sh -c 子进程 (dash)  ✗ not found
  make                 ✗ No such file or directory
  execvp (python)      ✗ FileNotFoundError
```

xlings 的主场是给项目提供**编译工具链**,消费者是 `make` / `npm` / 构建系统,它们不经过
交互 shell。所以这条路被证伪。

### 4.2 相对 PATH 条目

```
PATH=".xlings/subos/_/bin:$PATH"   # 静态,永不改动
  cwd = 项目根           ✓ 直接敲 ✓ make ✓ execvp ✓ dash sh -c
  cwd = 项目子目录 src/  ✗ 127
  cwd = 项目外           ✗ 127        ← 隔离天然成立
```

有效,但两个硬伤:**只在项目根生效**(在 `src/`、`build/` 里就没了);**PATH 劫持** ——
任何目录下放一个 `.xlings/subos/_/bin/ls`,cd 进去敲 `ls` 就中招,和 `.` 在 PATH 里同一类
问题。可作为受限场景的补强,不能是主方案。

### 4.3 准确的约束表述

一个名字要被**任意子进程**按 PATH 找到,必须存在一个 PATH 条目,**在子进程解析的那一刻**
指向含该文件的目录。这样的条目只有四种:

1. 全局绝对路径目录 → 内容全局唯一 → 有全局文件
2. 项目绝对路径目录 → 需要有人在 cd 时改 PATH → 需要 shell hook
3. 相对路径条目 → 仅根目录 + 劫持风险(§4.2)
4. symlink 路径 → 目标全局唯一 → 同 1

**cmd.exe 没有任何 cd hook**(无 `PROMPT_COMMAND` 等价物;`AutoRun` 只在 cmd 启动时跑;
无 command-not-found 钩子),所以路线 2 在 cmd.exe 上不存在。⇒ **要在 cmd.exe 上做到
「cd 进项目直接敲名字」,全局目录里必须有那个文件。**

---

## 5. 外部调研

八个同类工具,一手来源。

| 工具 | PATH 上的东西 | 文件带版本? | 是路由表? | 要 shell hook? | cmd.exe | 回收机制 |
|---|---|---|---|---|---|---|
| **proto** | `~/.proto/shims` + `~/.proto/bin` | shims 否 / bin 是 | shims 是 | 否 | ✅ 原生 `.exe` | `regen` 整目录删了重建 |
| **aqua** | `$AQUA_ROOT/bin` | 否 | 是(全量) | 否 | ✅ 硬链接到 `aqua-proxy` | 无;仅显式 `aqua rm -m l` |
| **rustup** | `~/.cargo/bin` | 否 | 是,**编译期写死 13 个名字** | 否 | ✅ symlink→hardlink 回退 | 不需要(表是常量) |
| **pyenv** | `~/.pyenv/shims` | 否 | 是(按已装版本生成) | 否 | ❌(见 pyenv-win) | `rehash`:注册-再清扫 |
| **pyenv-win** | `...\shims` | 否 | 是 | **根本没有 `pyenv init`** | ✅ `.bat` shim | 整目录清空重建 |
| **nvm-win v1** | 一个 symlink | — | 否(断言激活态) | — | ⚠️ 要管理员 | 无 |
| **nvm-win v2** | shims(Zig) | 否 | 是 | 否 | ✅ 不要管理员 | `nvm doctor --autofix` |
| **chocolatey** | `...\chocolatey\bin` | **是**(路径编进 PE) | 否 | 否 | ✅ 每 shim 现编一个 exe | **无,是 open bug** |
| **hermit** | 项目内 `./bin` | **是** | 否 | 否(stub 自举) | ❌ **完全不支持 Windows** | 不需要 |

### 结论 1:支持 cmd.exe 的工具,无一例外都是「一个全局目录 + 按名字建文件」

唯一躲开的 hermit 把 Windows 整个放弃了(README 标题 "uniform tooling for Linux and Mac";
release 矩阵只有 linux/darwin × amd64/arm64)。这从外部证据确认了 §4.3。

### 结论 2:主流设计的核心就是「路由与状态分离」

proto 最露骨 —— 它两个目录都有,官方博客(moonrepo.dev/blog/proto-v0.20)列表说明:

| | Shims (`~/.proto/shims`) | Binaries (`~/.proto/bin`) |
|---|---|---|
| 是什么 | 跑 `proto run` 的转发器 | 指向具体版本的符号链接 |
| 跑哪个版本 | **运行时探测** | **最后安装并 pin 的那个** |

shim 文件是 `proto-shim` 二进制的逐字节拷贝,每个工具**一模一样**,靠自己的文件名认身份
(`crates/cli/src/main_shim.rs`:`let shim_name = exe_path.file_name()...`)。它的存在断言
「这个名字归 proto 管」,不断言任何版本被激活。proto 的 changelog 0.47.4 甚至把
「shim 文件在、版本没配」当作**正常状态**描述,不是缺陷。

**这正是 §2 的病根的反面。**

### 结论 3:三分之二的工具做了「透传给宿主机」,且都实现了自排除

- **proto** `find_global_executable` 遍历 PATH 时跳过 `bin_dir`、`shims_dir`、任何以
  `.proto/shims` 结尾的目录、以及任何含 `registry.json` 的目录(躲开别人的 proto store)。
  changelog 0.49.4 的理由:*"mitigate a situation where the `~/.proto/shims` file takes
  precedence on PATH but proto should not be used."*
- **aqua** `lookPath` 跳过任何解析到 `aqua-proxy` 的条目(POSIX)/ aqua bin 目录(Windows)。
- **pyenv** `pyenv-which` 用 `remove_from_path "${PYENV_ROOT}/shims"` 之后再找,execs 系统 python。
- **rustup** *假装*做了:把裸名字重新 exec,靠 `RUST_RECURSION_COUNT`(上限 20)兜底。只有
  `cargo` 和 `rust-analyzer` 有真透传,后者的做法是跳过任何 `is_same_file` 等于自己的 PATH 条目。

xlings 的 `shim.cpp:276-289` 遍历 PATH 时**没有自排除**,只靠 `XLINGS_SHIM_DEPTH`
(`shim.cpp:392`,上限 8+2)兜底 —— 和 rustup 的假透传同形态。要做透传,这是必须先补的。

### 结论 4:透传有一个已知的静默陷阱

pyenv 至今带着:pin 了一个没装的版本,而系统 PATH 上有同名命令时,**跑系统的那个且一声不吭**
(`pyenv-which` 里 `nonexistent_versions` 的警告只在 else 分支)。这正是 memory 里
`silent-success-pattern` 那一类。

⇒ 透传必须严格限定:**只在当前 scope 对这个名字毫无主张时才透传;scope pin 了版本却没装,
必须报错,绝不透传。**

### 结论 5:路由表从来不靠「审计目录」回收,全是「按真相重建」

- proto:`regen` → `fs::remove_dir_all(&store.shims_dir)` 然后按已配置的工具重建
- pyenv:`remove_stale_shims()` 注册-再清扫,注册集就是路由集
- pyenv-win:`Sub Rehash` 先 `For Each file In ...Files: file.Delete True` 再重建
- rustup:表是编译期常量,压根没有 rehash,stale 结构上不可能
- aqua:无 reaper,只有显式 `aqua rm -m l`
- **chocolatey:唯一试图审计的,orphan shim 至今 open**(choco#3204「Add ability to verify
  shims and remove orphaned shims」、choco#3206「Automatically created shims should be
  removed during upgrade」)

⇒ **xlings 的 orphan-shim 规则(doctor Check 2)应该整条删掉,换成一个 rehash。这是净减法。**

### 结论 6:Windows shim 机制的选择,xlings 已经站对了

- **aqua** 明确写了为什么 Windows 上不能用 symlink:*"Non-administrators can't create symbolic
  links by default on Windows; PowerShell doesn't use the final target of a symbolic link when
  starting a process"* → 改用 hardlink。并且在 v2.30.0 **抛弃了 `.bat`/`.sh` 脚本**:
  *"bat scripts can't handle signals properly"*、*"tools can't be executed on Nushell"*。
- **rustup** symlink 优先、hardlink 回退,并记录了 Windows 文件锁问题:*"we have `rustup.exe`
  open and `cargo.exe` is a hard link to that file, we can't remove `cargo.exe`"*。
- **nvm-windows v1** 用 `mklink /D` → 要管理员 → **v2 因此抛弃了 symlink 模型**。
- **chocolatey** 每个 shim 现编一个 unsigned .NET exe:约 5s/shim、破坏 AppLocker、
  kill shim 时子进程变孤儿(home#134 自 2016、home#113 自 2018 至今 open)。**反面教材。**

xlings 的 `create_shim`(symlink > hardlink > copy,`xself/init.cppm:35`)+ 多调用入口二进制
= rustup 的 chimera + aqua 的选择。**这部分不需要动。**

---

## 6. 设计方案

### 6.1 不新建目录,原地重定义 `subos/<n>/bin`

**关键认识:状态从来就不需要文件。** `workspace.active`(subos 的 `.xlings.json`)才是状态。
`bin/` 里的文件**全部是路由** —— shim 是 entry binary 的一个 symlink/hardlink,名字就是它
携带的全部信息。doctor 的 Check 1(每个 active program 有没有 shim)和 Check 2(每个 shim
有没有 active version)是在手工维护一个**派生物**和真相的同步,而这正是一次重建能做完的事。

所以:

```
~/.xlings/subos/<n>/bin/            路由表(定义变了,路径没变,PATH 没变)
                                    内容 = 本 subos 的 active program ∪ 已知项目声明的命令名
                                    文件存在 ≠ 任何断言;由一个写者按真相重建
~/.xlings/subos/<n>/.xlings.json    状态,唯一真相(已经是了)
~/.xlings/.xlings.json              新增 knownProjects(§6.3)
```

代价对比,这是选它的理由:

| | 新建 `shims/` + 常量 PATH | 原地重定义 `bin/` |
|---|---|---|
| PATH 改动 | 有 | **无** |
| profile 版本(`profile_resources.cppm` kVersion=11) | 要升 | **不动** |
| `linux_release.sh` / `macos_release.sh` / CI / 文档 / self-contained 布局 | 全改 | **不动** |
| 旧 shell 会话 | 失效窗口 | **无** |
| 多 subos 并发 | **破坏**(见 §3 推翻 4) | 正确(PATH 本来就按 subos 变) |
| 路由表规模 | 被迫取并集 273 | 每个 subos 各自 99+ |

留下的风险是「目录还叫 `bin`,未来读者可能再次从中推导状态」。用注释 + 一条测试锁住语义
(§6.6),不靠改名。

### 6.2 路由表的内容与写者

**期望集合**(对 subos `N`):

```
desired(N) = { shim_filename(name)
               | name ∈ N.workspace.active, effective_kind(name) == "program" }
           ∪ { shim_filename(cmd)
               | cmd ∈ ⋃ knownProjects[*].commands }
           ∪ BUILTIN                       // xlings + SHIM_NAMES_OPTIONAL,永不删
```

项目命令名进**每一个** subos 的表,而不是只进全局 active 的那个:项目工具的解析走 cwd
(`resolve_subos_scope_`,`config.cpp:457`),从哪个 subos 里 `cd` 进项目都应该能用。实测
规模是 +23,可忽略。

**维护者是写者,不是修理工。** `install` / `use` / `remove` / 项目安装在自己的事务里
增删对应条目 —— 它们本来就在做这件事(`installer.cpp:2009` 等),只是现在有了一个共同的
定义。重建只在**漂移**时需要:项目目录被删、状态文件被手改、安装被中断、老客户端写的家。

### 6.3 `knownProjects`

放 `~/.xlings/.xlings.json`(与 `versions` / `activeSubos` 同级),不另立文件。
**只登记项目根路径,不缓存命令名:**

```json
"knownProjects": {
  "/home/speak/work/api": { "lastSeen": "2026-09-03T10:12:00Z" }
}
```

- **写**:project scope 的 `install` 成功后,用绝对路径为键登记(或刷新 `lastSeen`)。
- **读**:重建时,对每个键读 `<key>/.xlings/.xlings.json`,从**该项目自己的状态**
  (`versions` + `workspace.active`,取 program kind)现算命令名。
- **为什么不缓存命令名**:缓存会过期。项目的 `workspace` 只写包名,而一个包注册哪些命令名
  只有装完才知道(来自 versions DB),所以缓存必须在 install 时写死;之后项目改了依赖、
  或某个包被 `remove`,缓存就和现实脱节,而下一次重建会照着过期的缓存把已经没有的名字
  重新建出来 —— 正好是本文件要消灭的那类 bug。读项目自己的状态文件则永远是当前的。
  代价是每次重建多读 N 个小文件,相对 doctor 的 0.63s 可忽略。
- **回收**:`<key>/.xlings/.xlings.json` 不存在或不可解析 → 丢弃该项目(并在报告里
  说明),它的命令名随之退出所有 subos 的表。这就是「项目被删了」的自动回收路径,
  用户不需要做任何事。不可解析的项目**跳过而不是让整次重建失败**,与
  `profile.cpp` 的 `load_subos_snapshots` 同一政策。

### 6.4 重建是 `doctor --fix` 的一条修复,不是新命令

从「为什么要用它」倒推:

- 装完 / 切完之后要跑一次? → 那是写者的 bug,不是用户的工作。
- asdf 的 `reshim` 之所以必须存在,是因为 `pip install` / `npm i -g` 会**绕过 asdf**
  往版本目录里丢可执行文件,asdf 无从得知。xlings 没有这个洞:recipe 通过
  `xvm.add_version` 声明自己注册什么,没有「背着 xlings 装了个 program」的路径。
- proto 的 `regen` 定位就是修理(*"remove unexpected or broken shims"*),不是流程步骤。

⇒ **它是修复,而 xlings 的修复入口就是 `doctor --fix`。** 不新增命令、不新增用户概念,
而且正好落进现成的 scan → finding → repair 结构。

**doctor 的两条 shim 规则合并成一条。** Check 1(`doctor.cpp` 每个 active 有没有 shim)
和 Check 2(`doctor.cpp:838-882` 每个 shim 有没有 active)删除,换成:

```
FindingKind::ShimTableDrift    per subos,一条
  detail: desired 与 actual 的差集(要加的 / 要删的),按名字列出
  repair: 应用差集
```

**应用差集,不是清空重建。** proto / pyenv-win 都是整目录删了重来;那会留下一个 PATH 上
什么都没有的窗口,而且在 Windows 上一个被占用的文件就让整次重建失败。我们知道期望集合,
直接算差集只动该动的文件。

**删,并打印清单,不问**(§3 推翻 5):

```
$ xlings self doctor --fix
  ✓ shim table (default)   rebuilt
    • removed 23 entries — none resolved to an installed version in this subos
        brew cl lib libcxx-headers libxkbcommon link musl-cross-make
        qemu-aarch64-static qemu-arm qemu-riscv qemu-system-* rc rustup
        slang slangc slangd slangi xvm xvm-shim
```

打印是必须的:memory 的 `silent-success-pattern` 要求「什么都没发生」和「成功了」不能长得
一样。静默清扫违反它,确认框则是过度补偿(那 22 个名字删前删后都只能报错,用户没有选择)。

### 6.5 透传(host fallback)

```
~/work/api/.xlings.json 声明 node@22        宿主机 /usr/bin/node 是 18

  cd ~/work/api && node -v   →  22            (不变)
  cd ~          && node -v   →  18            (今天是 rc=1 的错误)
```

**这是本设计要的核心效果。** 全局 bin 里那个 `node` 之所以存在,纯粹是因为某个项目需要它
在 PATH 上;它不该改变项目之外的任何行为。透传把行为**还原成「xlings 没在这里放过文件」**
—— 也就是把 side effect 抵消掉,而不是用一条更好的错误消息去描述它。

实现(`shim.cpp` dispatch,`version.empty()` 分支):

1. 沿 `PATH` 找同名可执行文件,**排除 xlings 自己的目录**:`<home>/bin`、
   `<home>/subos/*/bin`、任何项目的 `.xlings/subos/*/bin`。这是 `shim.cpp:276-289` 现在
   缺的那一块(proto 排除 `shims_dir`/`bin_dir`/含 `registry.json` 的目录;aqua 排除解析到
   `aqua-proxy` 的条目;pyenv 用 `remove_from_path`;rustup 用 `is_same_file`)。
   `XLINGS_SHIM_DEPTH`(`shim.cpp:392`)保留为兜底。
2. 找到 → `execvp` 它(Windows 见 §9);找不到 → 报错,并说清归属。
3. **`stderr` 是 TTY 时**打印一行:

   ```
   [xlings] node: no version in subos 'default'; running /usr/bin/node
   ```

   非交互(`make` / CI / 管道)不打印 —— 那里噪音的代价最大,而 PATH 语义本来就该透明。

**透传的精确触发条件 —— 现成的三态诊断正好就是这条边界。**
`shim.cpp:428-462` 已经把「没有 active version」分成两支:

| 分支 | 条件 | 含义 | 透传? |
|---|---|---|---|
| `not_in_subos` | `active` 无此名 **且** `installed[]` 无此名 | 本 scope 对它**毫无主张** | **是** |
| `xvm.no_active_version` | `active` 无此名,但 `installed[]` 有 | 用户把它 opt-in 进了这个 subos,只是没选版本 —— **这是主张** | 否 |
| `xvm.pinned_version_missing`(`shim.cpp:495`) | `active` 有此名,`match_version` 解析不到 | pin 了但没装 —— **最强的主张** | 否 |

即:**透传只在 `here.empty()` 那一支发生**,另外两支必须报错。后两支正是 pyenv 至今带着的
静默 bug 的形状(pin 了一个没装的版本、系统上恰好有同名命令,于是跑了系统的且一声不吭)。
边界不是我新划的,是代码里本来就有的三态 —— 这也是它可信的理由。

### 6.6 其余四项

0. **drift 按方向分级**(实施中补上):`toAdd` 非空 = **Error**(有程序 active 却跑不了,
   真故障);只有 `toRemove` = **Notice**(陈旧条目删前删后都解析到空,而且在老 home 上
   不是用户造成的)。这正是 `self_doctor_anchor_shim_test.sh` 早就钉住的判断——只是它
   原来钉在 per-name 的 `anchor shim` 上,现在钉在 per-subos 的 `shim table` 上。
1. **删 `mirror_shim_to_global_bin`**:`src/core/common.cpp` + `common.cppm` 整个文件消失。
   三个调用点(`xim/installer.cpp:2009`、`xvm/commands.cpp:827`、
   `xim/libxpkg/types/script.cpp:74`)改为走统一的表维护。另需清理
   `src/core.cppm:12` 的 `export import` 和 `xim/libxpkg/types/subos.cpp:6` 的
   **无用 import**(实测该文件从不使用 `common::`)。
2. **诊断说清归属**:`not_in_subos` 的 remedy 从 `xlings install slang`(用户从没要过 slang)
   改为按来源分支 —— 项目提供的说明是哪个项目、全局装用 `xlings install -g <pkg>`。
3. **`tools` / `pkgCount` 的错标**:`subos.cpp:70-82` 和 `1453-1462` 数的是 bin 文件数,
   却在面板上叫 `tools`(`:1584`)、在 JSON 里叫 `pkgCount`(`:229`)。它从来不是包数
   (llvm 一个 release 注册 40 个 program 就算 40)。实测 `default` 今天报 **121**,而真正
   active 的 program 只有 **99** —— 多出的 22 正是要删的死名字。改名为 **`commands`**
   (它数的确实是命令名);真正的包数可由 `profile.cpp` 现成的 `load_subos_snapshots` 从
   release 层面算,列为后续。
4. **锁定语义的测试**:一条测试断言「`bin/` 里存在文件 ⇏ 该名字有 active version」,
   并断言重建后 `bin/` 恰好等于 desired 集合。这是防止未来读者再次从目录推导状态的闸门。

---

## 7. 自我 review

### 7.1 稳定性

| # | 风险 | 处理 |
|---|---|---|
| S1 | **Windows 文件被占用**。删一个正在运行的 shim 会失败;shim 是 `xlings.exe` 的硬链接,rustup 明确记录过这个坑(*"we have `rustup.exe` open and `cargo.exe` is a hard link to that file, we can't remove `cargo.exe`"*)。 | **新增**这一侧已经有现成原语:`create_shim` 走 `platform::displace_locked_file`(Windows 上把占用者改名挪开,POSIX 上就是 unlink),issue #473 的产物。**删除**这一侧今天是裸 `fs::remove`(`doctor.cpp:2191`),必须改用同一个原语;仍失败则**计数并报告**,不吞。`ensure_subos_shims` 返回失败计数正是为此,复用同一口径。 |
| S8 | **误删用户自己的文件**。有人往 `subos/<n>/bin` 放了一个真的 `node` 二进制,差集判它「多余」就删掉,是数据损失。 | 只删**确实是我们的 shim** 的条目,判据用 `std::filesystem::equivalent(candidate, entryBinary)` —— 它对 symlink(跟随)和 hardlink(文件标识相同)都成立,**一次调用两个平台都对**。不满足的文件**报告为异物但不动**。⚠️ 不要照搬 `compact/xself.cpp:29-38` 的现有判据:它先做 `fs::is_symlink`,而 Windows 上 shim 是硬链接不是 symlink,该判据在 Windows 上恒为 false —— 也就是说**那条 legacy alias 清理在 Windows 上从来没生效过**(见 §9 附注)。 |
| S2 | **并发**。两个 xlings 同时重建同一张表。 | 重建持有 home 的 state lock(`xvm/lock.cppm`),与 workspace 写同一把锁 —— 表和真相必须在同一个事务里。 |
| S3 | **中途失败留下半张表**。 | 差集是幂等的:重跑一次收敛。不做「先删后建」,任何时刻表都是 desired 的子集或超集,不会是空。 |
| S4 | **误删入口二进制**。 | `BUILTIN`(`SHIM_NAMES_BASE` + `SHIM_NAMES_OPTIONAL`,`xself/init.cpp:16`)永远在 desired 里,`is_builtin_shim` 已有。 |
| S5 | **透传递归**。shim 沿 PATH 找到自己。 | 排除本 home 的 `bin` 与所有 `subos/*/bin`,以及项目 `.xlings/subos/*/bin`。用**路径前缀**判断而非 `is_same_file` —— Windows 上 shim 是硬链接,同一 inode 的判断在跨卷/复制场景不可靠,而我们知道自己的布局。`XLINGS_SHIM_DEPTH` 保留为兜底。 |
| S6 | **透传吃掉 entry binary**。`xlings`/`xim`/`xvm` 被透传到别的 xlings。 | multicall 分支在 dispatch 之前(`main.cpp:37-54`,`shim_dispatch` 在 `:94`),透传不经过这些名字。 |
| S7 | **共享名字所有权**。两个包注册同一个命令名。 | 表按名字键,一个条目;所有权在**注册时**裁决(现有 `programs` 独占口径,memory `shared-shim-ownership`),重建不重新裁决。 |

### 7.2 跨平台

| # | 点 | 结论 |
|---|---|---|
| X1 | **cmd.exe 无感** | 不变。PATH 不动、目录不动、文件还是 `xlings.exe` 的硬链接。这是整个方案的硬约束(§4.3 / §5 结论 1),而它零成本满足。 |
| X2 | **`.exe` 后缀** | desired 集合必须用平台后缀算,否则差集会把每个条目同时判成「缺失」和「多余」。`doctor.cpp:50` 的 `shim_filename_` / `shim_ext_` 现成。 |
| X3 | **透传的 PATHEXT** | `resolve_alias_program`(`shim.cpp:255-259`)已经在 Windows 上探测 `""/.exe/.bat/.cmd`,复用。 |
| X4 | **shim 创建方式** | 不变:`create_shim` = symlink > hardlink > copy(`xself/init.cppm:35`),与 rustup / aqua 的选择一致(aqua 明确解释过 Windows 上不能用 symlink)。 |
| X5 | **sandbox** | 无需改动:`sandbox.cpp:356` 把整个 `~/.xlings` 以同路径绑进去,布局变化自动可见。 |
| X6 | **owner-anchored dispatch** | 无需改动:`resolve_owner_home` 从 `<home>/subos/<n>/bin/<name>` 向上仍命中 `is_home_root`(`shim.cpp:33`)。 |
| X7 | **macOS** | 无特殊点。 |

### 7.3 用户体验

**变好的:**
- 项目外敲项目工具:从一条**误导的**错误(`xlings install slang` —— 用户从没要过 slang)
  变成**宿主机原本的行为**,或一条说清归属的错误。
- 那 23 个死名字被回收;新家根本不会长出来。
- `commands:` 数字第一次是准的(121 → 99+)。

**变化但不是退化:**
- `subos list` 的 `tools:` 改名 `commands:`,数字变小。这是从虚报变准,不是功能减少。

**唯一的行为切换点:** 项目外敲一个只有项目才有的名字,今天报错、改后跑宿主机的。
这**就是要的效果** —— 项目安装不该给全局留 side effect,而透传正是把 side effect 抵消。
TTY 下一行提示保证交互时用户看得见发生了什么。

**没有迁移动作:** 用户不需要跑任何命令。旧家的死名字在下一次 `doctor --fix` 时回收;
不跑也只是维持现状,不会更坏。

### 7.4 架构

**净减法:**

```
删:  src/core/common.{cpp,cppm}          整个模块
      3 个 mirror 调用点 + 1 个无用 import + core.cppm 的 export import
      doctor Check 1(active → shim)
      doctor Check 2 / FindingKind::OrphanShim
        (doctor.cpp:838-882, 2188-2197, 2824, 3041, 3273)
加:  1 个 desired-set 计算 + 差集应用(唯一写者)
      knownProjects 读写
      shim dispatch 的 PATH 自排除 + 透传分支
      1 条锁定语义的测试
```

**收敛的不变量:**
- 目录 `bin/` 从两个含义收敛到一个(路由)。
- shim 的写者从 3 个收敛到 1 个。
- 派生物与真相的同步,从「两条互相打架的审计规则」收敛到「一次差集」。
- 不新增命令、不新增目录、不新增用户概念。

**这一节里我唯一不满意的地方:** 目录名仍叫 `bin`,而它现在是路由表。语义只由注释和一条
测试守住。这是为了换取零 PATH 改动、零迁移窗口而付的价,记在这里以免将来有人以为是疏忽。

---

## 8. 曾经的未决问题 —— 全部已定案

**8.1 desired 集合的 `kind` 口径 —— 关闭,代码今天已经是对的。**
`installer.cpp:352-360` 按 kind 分流,只有 `program` 产生 `ProgramShim` 效果
(`lib` → `Library`,`files` → `FileAsset`);`cmd_use` 那侧同样有
`effective_kind_of(...) != "program" → continue`(`commands.cpp:822`)。
`libcxx-headers` / `libxkbcommon` / `musl-cross-make` 这类名字是 **2026.7.29.2 修复之前**
的历史残渣 —— `installer.cpp:1970` 的注释自陈:*"The sibling effects below already guard on
activation; **this one did not**."* 与「22/23 指向虚空」吻合。
lib/files 包**登记进 xvm 是设计**(它们也要版本管理),**有 shim 才是缺陷**,这两件事代码
已经分开。⇒ desired 用 `kind == "program"`,重建清掉它们即为正确结果。

**8.2 binding root / release anchor —— 不进表。**
切换的本质是切换 binding root 及其成员 list,所以 root 是**选择的单位**,不是**命令**:
没人 exec 它。守卫一条:把 anchor 移出表**不得改变 `cmd_use` 遍历成员的范围** ——
测试断言「切换后成员 shim 齐全,root 自己没有文件」。

**8.3 build-side 的三处 PATH 注入 —— 保留,加测试钉住。**
`installer.cpp:2835`(build_dep)、`:2965`(patchelf)、`compact/git.cppm:92`(git)要的是
「能跑到工具」,路由表照样满足。加一条测试锁住。

**8.4 真包数 —— 加。**

```
default   commands: 99   packages: 14
```

`commands` 数的是命令名,一个 release 可能注册几十个(llvm 一个包 ≈ 40 个名字),所以它
回答不了用户真正会问的「我装了什么」。真包数从 workspace 的 release 层面算
(`profile.cpp` 的 `load_subos_snapshots`),每个 subos 多读一个 JSON。

---

## 8.5 Follow-up(不在本次范围):`src/core/subos` → `modules/subos`

`modules/` 的成员是独立包,**不能依赖 root**。subos 今天依赖 root 的十个模块:
`core.config`(Config 单例)、`core.xim.commands`(直接调 `cmd_install`)、`core.xself`、
`core.xvm.shim`、`cli.spec`(一个库知道 CLI 的参数规格)、`core.{home_config,log,utils,
xim.compatibility}`、`runtime`。6161 行,6 个反向依赖。

**不与本次合并的理由是归因**:本次改的是 doctor / installer / shim-dispatch 的**语义**,
模块抽取改的是**构建图**;一起落地,构建一红就分不清是谁干的
(memory `attribute-ci-break`)。

前置链(按必须顺序):① `Config` 解耦或先出去 → ② 反转 `subos → xim::commands`
(subos 不该调安装器;调用方装完把结果交给它)→ ③ 砍 `subos → cli.spec` →
④ shim 原语下沉。

**本次为 ④ 铺路**:路由表的唯一写者放在 `xvm/shim_table`,不塞进 `xself` 也不塞进 `subos`,
它需要的 `create_shim` / `displace_locked_file` 一并集中,下次抽取可以整块搬走。

---
## 9. 附带发现:Windows 上 shim 走 `cmd /c`(独立缺陷,建议单独开 issue)

`src/core/xvm/shim.cpp:670-687`:

```cpp
#if defined(__linux__) || defined(__APPLE__)
    execvp(exe_path.c_str(), const_cast<char* const*>(new_argv.data()));
#else
    std::string cmd = platform::shell_quote(exe_path.string());
    for (int i = 1; i < argc; ++i) cmd += " " + platform::shell_quote(argv[i]);
    return platform::exec(cmd);          // → std::system() → cmd.exe /c "<字符串>"
#endif
```

`platform::exec` 是 `std::system`(`modules/platform/src/platform.cpp:166-185`)。

- **Unix 侧是 `execvp`,比 proto / aqua / chocolatey 都好** —— 进程自我替换,不多一层、
  信号直达、退出码与 argv 原样透传。
- **Windows 侧退化成 `cmd.exe /c`**,把 §5 记录的坑全踩了:参数被 cmd.exe 二次解析
  (chocolatey choco#1273 那一类,也是 aqua 抛弃 `.bat` 的原因);进程树多一层,Ctrl-C 和
  外部 kill 打在 shim 上而不是目标上(chocolatey home#134 / home#113,2016/2018 至今 open);
  退出码穿过 `cmd /c` 会被改写;每次调用多一个 cmd 进程。

修法(proto 的做法):直接 `CreateProcessW` 传原始命令行(不经 cmd.exe)+ 装一个空的
`SetConsoleCtrlHandler` 让 Ctrl-C 直达子进程 + 原样返回子进程退出码。注意
`platform.cpp:278` 现有的是 `CreateProcess**A**`,撞 `reference_windows_acp_path_narrowing`
记录过的 ACP 窄化问题。

---

## 10. 来源

- proto:[Workflows](https://moonrepo.dev/docs/proto/workflows) ·
  [v0.20 blog](https://moonrepo.dev/blog/proto-v0.20) ·
  [regen](https://moonrepo.dev/docs/proto/commands/regen) ·
  [main_shim.rs](https://github.com/moonrepo/proto/blob/master/crates/cli/src/main_shim.rs) ·
  [commands/run.rs](https://github.com/moonrepo/proto/blob/master/crates/cli/src/commands/run.rs) ·
  [shim/windows.rs](https://github.com/moonrepo/proto/blob/master/crates/shim/src/windows.rs)
- aqua:[Lazy Install](https://aquaproj.github.io/docs/reference/lazy-install/) ·
  [Windows Support](https://aquaproj.github.io/docs/reference/windows-support/) ·
  [installpackage/link.go](https://github.com/aquaproj/aqua/blob/main/pkg/installpackage/link.go) ·
  [which/lookpath.go](https://github.com/aquaproj/aqua/blob/main/pkg/controller/which/lookpath.go)
- rustup:[concepts/proxies.md](https://github.com/rust-lang/rustup/blob/master/doc/user-guide/src/concepts/proxies.md) ·
  [overrides.md](https://github.com/rust-lang/rustup/blob/master/doc/user-guide/src/overrides.md) ·
  [src/lib.rs](https://github.com/rust-lang/rustup/blob/master/src/lib.rs) ·
  [src/toolchain.rs](https://github.com/rust-lang/rustup/blob/master/src/toolchain.rs)
- pyenv:[README](https://github.com/pyenv/pyenv/blob/master/README.md) ·
  [libexec/pyenv-rehash](https://github.com/pyenv/pyenv/blob/master/libexec/pyenv-rehash) ·
  [libexec/pyenv-which](https://github.com/pyenv/pyenv/blob/master/libexec/pyenv-which)
- pyenv-win:[pyenv-lib.vbs](https://github.com/pyenv-win/pyenv-win/blob/master/pyenv-win/libexec/libs/pyenv-lib.vbs) ·
  [bin/pyenv.bat](https://github.com/pyenv-win/pyenv-win/blob/master/pyenv-win/bin/pyenv.bat)
- nvm-windows:[v2 modes](https://docs.nvm-windows.com/features/modes) ·
  [v2 doctor](https://docs.nvm-windows.com/command/sync-doctor) · v1 `src/nvm.go`
- chocolatey:[features/shim](https://docs.chocolatey.org/en-us/features/shim/) ·
  [ShimGenerationService.cs](https://github.com/chocolatey/choco/blob/develop/src/chocolatey/infrastructure.app/services/ShimGenerationService.cs) ·
  [home#134](https://github.com/chocolatey/home/issues/134) ·
  [home#113](https://github.com/chocolatey/home/issues/113) ·
  [choco#3204](https://github.com/chocolatey/choco/issues/3204) ·
  [choco#3206](https://github.com/chocolatey/choco/issues/3206)
- hermit:[README](https://github.com/cashapp/hermit/blob/master/README.md) ·
  [env.go](https://github.com/cashapp/hermit/blob/master/env.go)

---

## 附注:legacy alias 清理在 Windows 上从未生效(自我 review 时发现)

`compact/xself.cpp:29-38`:

```cpp
bool is_legacy_alias_symlink_to_bootstrap(const fs::path& path,
                                          const fs::path& canonical_bootstrap) {
    if (!fs::is_symlink(path, ec)) return false;      // ← Windows 上恒 false
    auto target = fs::weakly_canonical(path, ec);
    return target == canonical_bootstrap;
}
```

Windows 上 shim 是 hardlink / copy(`create_shim` 的 `#if !defined(_WIN32)` 分支跳过
symlink),所以 `is_symlink` 恒为 false,`cleanup_legacy_alias_shims` 的四个调用点
(`self init`、`install xlings --use`、`use xlings <ver>`、`doctor --fix`)在 Windows 上
**全部空转** —— 0.4.8 之前留下的 `xim.exe` / `xvm.exe` 等 alias 至今还在 Windows 用户的
home 里,而 `report_deprecated_alias_if_match` 会让它们以 exit 2 失败。

这是一个 COMPAT 路径(计划在 0.6.0 删),优先级低,但正好是本文件 S8 要避免的同一个错误:
**用 symlink 判断「这是不是我们的文件」在 Windows 上不成立。** 改用
`std::filesystem::equivalent` 一并解决。建议随本次改动顺手修,或单独记一条。
