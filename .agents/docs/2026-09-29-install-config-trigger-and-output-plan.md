# install 的 config 触发、输出层与 elfpatch 跳过规则 —— 方案

2026-09-29。来源：libxpkg#43、xlings#632、xlings#629，以及维护者提的两条：
`self update` 不要下载进度条、TUI 进度条后补空行；`install` 默认不重跑
config，用 `--reconfig` 触发。

第一轮 review 已定：

| 点 | 决定 |
|---|---|
| config 触发 | 三种情况分开处理（§2.2），不是"一律不触发" |
| 配方改了 config 怎么生效 | **(a)** 必须 bump `revision`；"是否最新"只有 revision 一个回答者 |
| #632 §3 `installed()` | xlings 不改；index 给 qt 系列 bump `revision` |
| A2 / self update | 同意按流类型处理；`self update` 整个不画进度条 |
| #632 §2 模糊匹配 | 本轮不改 |
| B2 shim 表 | 要深度分析，确认没有问题且架构合理 → §3 |

---

## 0. 范围

**做：** libxpkg#43 ①②③；#629；#632 §1、§4；两条维护者需求。

**不做：**
- libxpkg#43 ④（`resolved_dep` 带 loader）：①② 做完后 chatgpt 可以回到自动 patch，配方不再需要接管。
- #632 §2：维护者决定本轮不动。现状本来就是"可以自动匹配、但安装前要确认"，只有 `-y` 会跳过确认。
- #632 §3：理由见 §2.10。
- `subos.env{reason=}`：要改 spec，延后。
- 各 index 的配方改动：qt 系列 revision、chatgpt 接管代码的删除、构建脚本。

---

## 1. 输出层

### A1 进度条后补空行
`src/cli.cpp` 的 `render_download_event_`：画完一个流的最后一帧后，恢复光标，再输出一个 `\n`。只在终端上做。非终端走的是逐行输出，不需要。

### A2 index 流不画动态帧
`index:*` 流在任何输出目标上都走逐行分支：开始一行、结束一行。不画会刷新的帧，也就不需要补空行。
- 改 **E2E-123 P2**：update 那一半现在要求 pty 上至少画一帧，要改成 0 帧、2 行。这是有意的规格变更。

### A2′ self update 不画进度条（用已有开关，不新增）
**发现：** 下载渲染器在问 `palette::cursor_rewrite_allowed()`，它不看 UI 模式。而 `ui::capabilities_of` 已经给出了 `cursorRewrite = (mode == Tui) && stdoutIsTerminal`。同一个问题有两个回答者，结果是 `--ui-mode cli` 在终端上照样画进度条，和 spec §7（"cli：纯文本"）矛盾。

**改法：**
1. 渲染器改问 `ui::current_capabilities().cursorRewrite`，interface 模式原有的提前返回保留。要改的调用点有两处：`src/cli.cpp:127` 和 `src/ui/progress.cpp:231`。
2. `self update` 调用两个子进程时带上 `--ui-mode cli`：`xlings --ui-mode cli update` 和 `xlings --ui-mode cli install xlings@latest -y --use`。

**副作用：** 子进程那两段输出变成纯文本、没有颜色。
**影响面：** 显式配置了 `uiMode=cli` 的用户，在终端上会从进度条变成逐行输出，这正是 spec 的本意。默认的 `auto` 在终端上解析为 tui，不受影响。

### A3 #629：构建脚本输出改走 libxpkg 的 sink
**否决 xlings 侧 fd 截获的方案，理由：**
- `platform::StdoutCapture` 明确写着不可重入：一个进程最多一个实例，嵌套的那个析构时会把 fd 1 交回外层的管道。interface 模式在外层已经开了一个。
- 截获期间，CLI 自己的 log 和进度帧也写 fd 1，会被一起吞进管道，渲染器得改成写保存下来的 fd。
- 还要多一个读线程，Windows 上的行为也不一样。

**核查到的事实：** 三个子索引的 `pkgindex-build.lua` 都是先看有没有 `cprintf`（libxpkg 的沙箱没有这个函数），再用 `io.write("\r[i/n] ns::name\027[K")`，最后才退到 `print`。

**改法：**
- **libxpkg**：`build_index` 增加一个可选的输出 sink。给了 sink 时，构建沙箱里的 `io.write`/`print` 都写进 sink，`io.flush` 变成空操作；不给时行为和现在一样。
- **xlings**（`src/core/xim/index.cpp:197`）：
  - 传入 sink。按 `\n` 和 `\r` 切行；识别 `[i/n] msg` 的函数用 `parse_bracketed_step_`，把它从 `interface.cpp` 挪到一个共享模块，只保留这一个实现。
  - 进度步骤交给一个进程级的 observer。interface 把它设置成发出和现在完全一样的 `ProgressEvent{phase="index_rebuild", message="rebuilding index cache i/n: msg"}`，协议不变。CLI 不关心步骤。
  - 构建结束时 `log::info("[index] built {} ({} files)")`，每个索引一行，终端和非终端一样。只在确实跑了构建脚本时才输出（`run_pkgindex_build` 返回 true），没有构建脚本的主索引不输出。
  - 不认识的行原样转给 log。
  - interface 会话级的 StdoutCapture 保留，继续兜住其他代码写到 stdout 的内容。
- **验收**沿用 #629 原文：`xlings update > file 2>&1` 在全新 home 下不出现 `\r` 和 `\033[` 字节，每个重建的索引只出现一次。

---

## 2. config 触发（#632 §1 + `--reconfig`）

### 2.1 事实
- `commands.cpp` 在 `pending_count()==0` 时打印 "is already installed"，**然后照样调用 `execute()`**。
- installer 对每个 payload 已经在的节点都会重跑 config hook。
- `resolver.cpp:156` 的 `alreadyInstalled` 只表示 payload 在不在 store 里（home 级的事实），不回答"有没有绑定到当前 subos"。

### 2.2 三种情况

| 节点状态 | 默认 | `--reconfig` |
|---|---|---|
| payload 不在或已过期（没装、异平台、不完整、revision 不对） | install + config（不变） | 同左 |
| payload 在，但**当前 scope** 还没配置过 | 只跑 config，即映射（不变） | 同左 |
| payload 在，且当前 scope 已按当前 revision 配置过 | **跳过** | 重跑 |

### 2.3 判定：`configured_here(node)`
问的问题是："这个节点有没有按**当前配方 revision**，在**本次命令写入的那个 scope** 里配置过"。scope 指的是 `activeSubos` / `workspace_config_path()`，不是全局 subos。按 AGENTS.md 的要求，代码里要写一行说明问的是哪个问题。

两个条件都满足才算已配置：
1. 当前 scope 的记录 `configured["<ns>:<name>@<version>"].revision == node.revision`；
2. ledger 里所有归属这个 payload 的条目（按 payload 坐标匹配，和 `count_ledger_registrations` 用同一个判断），都被当前 scope 的 `installed[]` 认领。
   - 比较时容忍版本 key 带不带命名空间，规则和 `filter_to_subos_installed_` 一样。
   - payload 没有任何注册条目时，这一条自动成立。
   - 不读 stamp 的 `registered` 字段：老 stamp 里没有这个字段。

**豁免和不适用：**
- `kind=Build` 的节点从来不跑 config，视为已配置。否则 plan 里只要有 build dep，提前返回就永远触发不了。
- `pkgType==3`（Config 类型）不写 stamp，也不参与这条判定，行为不变。

**planner 和 installer 用同一个函数。** 这和 `payload_revision_verdict` 的做法一样：planner 需要它来决定是否提前返回和显示什么，installer 在每个节点上用它。

### 2.4 为什么要新增一条记录，而不能只从现有记录推导
只用条件 2（认领）有两个漏洞：
- **跨 subos 的 revision bump 传不过去。** payload 是整个 home 共享的。在 subos A 里 bump revision 后重装，B 里的认领都还在，于是被判成"已配置"，新版 config（新的 env 声明、新的 sysroot 链接）永远到不了 B。在 B1 之前，B 里任何一次触及这个包的 install 都会重跑 config，所以这是 B1 本身引入的倒退，决定 (a) 单靠自己也补不上。
- **`registered == 0` 的包拿不到任何证据**，只能永远重跑。例如 `claude-llm` 的 config 每次都会要 API key，还发一次验证请求。

**记录的约定：**
- **位置：** 当前 scope 的 `.xlings.json` 里新增顶层键 `configured`，由 `save_workspace` 的读-改-写流程一起写。它是 subos 自己拥有的事实，放在 subos 自己的文件里，不越过 home 和 subos 的状态分界。
- **写：** 只有 installer 一处写，在 config 成功、re-stamp 之后。
- **删：** 当前 scope 的 `Installer::uninstall`（detach 和 delete 两条路径都要删），以及 `apply_subos_env_ops_` 里的 superseded 分支。
- **没有记录**就判"未配置"，跑 config，然后写上记录。这是安全的方向，老 home 在第一次 install 时自然完成迁移。
- 删除 subos 时记录随文件一起消失。`subos new --from` 会把记录连同 sysroot、`bin/`、workspace 一起复制过去，它们仍然一致。

### 2.5 全部已配置时提前返回
按顺序：
1. 打印 `<pkg>@<v> is already installed (--reconfig to re-run its configuration)`。
2. `record_report()`。`already_present` 在进入 `execute()` 之前就已经预置，所以 `install_revision_test` R2 不受影响。
3. `activate_requested_targets()`。`SubosRuntimeMissing` 的修复靠的就是这一步。
4. **调用一次 `sync_shim_tables()`**，保持 AGENTS.md 写明的不变量："任何全局 scope 的 install/use/remove 都会重建整张表"。
5. 不调用 `execute()`。

### 2.6 混合的 plan
installer 对已配置的节点直接跳过 config、snapshot、re-stamp 和 closure 扫描，然后发 `Done{payloadReused, "already configured"}`。

### 2.7 `--reconfig`
- CLI：`install --reconfig`。interface：`install_packages` 增加字段 `reconfig: bool`，只增不改，协议从 1.3 升到 1.4。
- 作用范围是**整个 plan**，也就是精确恢复今天的行为，作为逃生口。

### 2.8 决定 (a) 的落地
配方只要改了 config 的效果，就必须 bump `revision`。这条要写进配方作者文档（xim-pkgindex 的贡献指南，加上 libxpkg 的文档）。
- 代价：payload 要重装。
- 好处：各个 subos 会在下一次 install 触及这个包时各自补上 config（§2.4 的记录就是为此）。

**配方规则：config 不能把 scope 相关的数据写进共享 payload。** 如果写了，两个 scope 本来就会互相覆盖；有了 B1，切换 scope 也不会再改写回去。已核查：
- `gcc`：specs 里写的是 payload 直达路径，并且自带 stamp，重跑 config 时不会重写 specs。
- `expat`、`fontconfig`、`fribidi`：写的是 subos 的 sysroot，属于每个 scope 自己的效果，没有问题。

### 2.9 doctor 打印的补救命令是否仍然有效

| 位置 | 场景 | B1 之后 |
|---|---|---|
| `doctor.cpp:444` | env 的 provider payload 缺失 | 会重装，有效 |
| `doctor.cpp:605` / `:655` | 声明的 runtime 没有激活 | 靠激活修复（§2.5 第 3 步），有效 |
| `doctor.cpp:676` | `SubosRuntimeUnknown`，提示 "xlings install glibc，之后 runtime 会被记录" | 依赖 config 重跑。**改成打印 `xlings install glibc --reconfig`** |
| `doctor.cpp:1881` | payload 不完整 | 会重装，有效 |
| `doctor.cpp:2106`、`repair.cpp:157` | remove + install | remove 会删掉记录，有效 |

`self update` 在已是最新版时：子进程的 install 会提前返回，但父进程随后执行的 `xlings use xlings latest` 里，`cmd_use` 没有"已激活就提前返回"的路径，一定会走到 `sync_shim_tables()`（`xvm/commands.cpp:844`）。所以 AGENTS.md 写的"self update 顺带修复 shim 表"仍然成立，不需要 `--reconfig`。

### 2.10 #632 §3：xlings 不改
调用 `installed()` 会给"payload 是否最新"加上第二个回答者。另外，有的配方里 `installed()` 的意思是"宿主机上有没有"，比如 `cpp.lua` 跑 `gcc --version`、`github-gh.lua` 跑 `gh --version`，它返回 false 会导致每次都重装。
**index 那边的动作：** 给 `qt-base`、`qt`、`qt-addons`、`qt5` bump `revision`。

### 2.11 B4：特权 env 警告只在新增或变化时发
警告在 `installer.cpp:1836`，那里已经持有 manifest 文档。在 `set_env_section` 之前读出这个 binding 现有的 section，只对其中没有的 (var, op, value) 三元组发警告。`tests/unit/test_subos_manifest.cpp` 需要补一个"原样重录不告警"的用例。

### 2.12 B3：默认模式下的进度
- `ProgressEvent` 已经存在，interface 已经把它作为 `{"kind":"progress"}` 发出去，所以 interface 这边零改动。
- **CLI：每个跑过 hook 的节点输出一行**，新装的和映射的都算，已配置被跳过的不输出。
- 不做就地刷新的状态行，原因：
  - log 行虽然都在 console 锁下（`log.cpp` 的 `emit_line_`），但 Lua hook 的 `print` 直接写 C 的 stdout，绕过这把锁，状态行一定会被踩；
  - 这和 A1 要解决的"贴在一起"是同一类问题。
- **格式约束：** 不能匹配 E2E-123 里里程碑行的正则 `^    [↓✓✗] `，否则会被计进下载行数。

---

## 3. B2：shim 表同步 —— 深度分析

### 3.1 原方案"推迟到 plan 结束"：否决
- hook 能看到当前 scope 的 `bin/`：`installer.cpp:3358-3365` 把 `ctx.bin_dir`（当前 scope 的 subos `bin/`，见 `installer.cpp:842`）加到 PATH 前面，而且**始终没有恢复**。所以从第一个新装节点开始，后面所有 hook 的 PATH 里都有它。
- 有五个配方依赖"同一个 plan 里先装的依赖的 shim 已经存在"：

| 配方 | hook | 用到的依赖 | 怎么调用 |
|---|---|---|---|
| `gcc.lua:550` | config | `xim:gcc-specs-config` | **直接用 shim 的路径** `system.bindir()/gcc-specs-config`（:439 的注释写明了这一点） |
| `musl-gcc.lua:114,135` | install | `xim:patchelf@0.18.0` | `patchelf …`，靠 PATH |
| `media-crawler.lua:54,67` | install | `xim:uv` | `uv …`，靠 PATH |
| `mcpp-vscode-clangd.lua:224` | config | `xim:mcpp` | `mcpp build`，靠 PATH |
| `claude-llm.lua:242` | config | `xim:claude` | `claude -p …`，靠 PATH |

- 推迟之后，全新安装 gcc 时 config 会失败（"gcc specs rewrite did not take effect"）。在一台干净机器上，musl-gcc 找不到 patchelf 会跳过重定位，只留一条 warning，得到的工具链是坏的。
- 附带说明：elfpatch 自己找 patchelf 是先查 payload（`pkginfo.tool_payload_dir`，libxpkg ≥0.0.51，xlings 用的是 0.0.58），shim 只是后备，所以它本身不依赖 per-node 同步。
- **结论：每个节点同步一次必须保留。** 另外 `installer.cpp:2188` 的注释写着 "once the install has finished"，与实现不符，要改掉。

### 3.2 实测：每次同步的时间花在哪
- **方法：** 在 scratch 下临时建一个 home，只复制真实 home 的状态文件：3.6 MB 的 `.xlings.json`（3965 个 target、5575 个条目）、`subos/default` 的 `.xlings.json` 和 `bin/`。payload 路径仍然是绝对路径，只读。在上面跑已发布的 2026.9.28.2：`xlings -v use adb <另一个已装版本>`，也就是一次切换加一次同步，同时用 strace 计时。
- **为什么不在真实 home 的切片上跑 install：** config hook 会原地改写 payload（gcc 的 specs、stamp、快照），切片的硬链接会把这些改动写穿到真实 home。本机 AppArmor 禁止无特权 userns，xim 池里的 bwrap 也不是 setuid，overlay 隔离不可用。

| 阶段（strace 下；不开 strace 时整条命令 0.50s，约为 0.46 倍） | strace 时间 | 折算成真实时间 |
|---|---|---|
| `sync_shim_tables` 合计 | 0.345s | ≈0.16s |
| ├ `known_projects()`：**为了取一个键，把 3.6 MB 的 home 配置整份重新解析** | 0.174s | ≈0.08s |
| ├ 读各个 project 的状态文件（mcpp 那个 1.2 MB） | 0.126s | ≈0.06s |
| └ 计算差异并写入 scope 的表 | 0.045s | ≈0.02s |

- **只在 project scope 下才有：** 每次同步都会调 `register_known_project`，把 3.6 MB 读进来、解析、再整份写回，只为了把 `lastSeen` 更新到当前秒。而 `lastSeen` **没有任何代码读**，全仓库 grep 只有写入的地方。
- **纠正 issue 的归因：** issue 说每个节点 0.8–1.3s，"大部分花在为每个 project 重算 shim 表"。实际上表本身的计算只占约 0.02s。同步里真正的大头是重复读写 home 配置。issue 自己的时间戳也对得上：`[shim-table]` 那几行加起来不到 0.09s。
- **每个节点剩下的开销：** `save_versions` 本身也是整份读入、解析、dump 再 fsync 这 3.6 MB，外加 hook 本身。本轮不改，见 §3.5。

### 3.3 新的 B2：同步次数不变，让每次同步变便宜
- **(a) `Config::known_projects()` 不再每次都整份重新解析。**
  - 列表来自对权威文件的最后一次解析：`reload_state_` 在拿到锁之后本来就要解析，全局 scope 下 `save_versions` 每个节点本来也要解析，顺手记下列表，不增加解析次数。
  - 返回列表前比较文件的 (size, mtime)；和上次解析时不一样就重新解析。
  - `register_known_project` 写入时同时更新内存里的列表。
- **(b) `register_known_project` 只在 key 不存在，或者当天还没写过 `lastSeen` 时才写。** 判断存在与否用 (a) 的列表，不另外解析；真要写的时候才做读-改-写。
- **(c) 不做：给 project 贡献加缓存。** 收益约 0.06s/节点；代价是多一份按 mtime 判新旧的缓存，在 mtime 精度粗的文件系统上可能过期，而且当前 project 每个节点都会被写一次。不值得多一个回答者。
- **再补一条代码注释，把 PATH 的约定写明：** 后续 hook 可以通过 PATH（以及 `system.bindir()`）看到同一个 plan 里先装的依赖的 shim。五个配方依赖它，它不再只是一个副作用。

### 3.4 为什么这套架构是合理的
- **写者还是只有一个，仍然是 `sync_shim_tables`。** 它的输入值没变（同样的 workspace、DB、project 集合），只是少了重复 I/O。没有推迟，也没有部分同步，不引入"表暂时落后"的状态。
- **(a) 的新鲜度与今天等价。**
  - 所有 4 个调用方（`install_`、`cmd_use`、`self init`、remove）都持有 state lock；其中 `self init` 拿锁后不 reload。knownProjects 的唯一写者也在锁内。(a) 的正确性不依赖 reload：每次返回列表前都会比较文件的 (size, mtime)。
  - hook 里起的子 xlings 会通过锁重入登记新 project，那样会改变文件大小。项目的增删必然改变 size，所以 (size, mtime) 能发现这类变化，不会用一份旧列表去重建表、把别的 project 的名字删掉。这正是 AGENTS.md 说的"用错误的输入重建"那种风险。
- **中途崩溃时的行为不变**，因为仍然是每个节点同步一次。

### 3.5 预期、测量方法、后续
- **预期：** 全局 scope 下每次同步约从 0.16s 降到 0.08s；project scope 下每个节点再省掉一次 3.6 MB 的读加写。
- **测量：** 实现后用 §3.2 同一个临时 home 的做法重测，再用 fixture 闭包做一次冷装计时。
- **后续，不在本轮：**
  - `save_versions` 每个节点都整份重写 3.6 MB；可以改成"DB 有变化才写"，但要先确认注册时能判断出 DB 没变。
  - 更根本的是版本 DB 的存储形态。
  - B1 已经消除了"全部已配置"这种情况下的全部开销。

---

## 4. libxpkg#43

已在真实文件上核实，都是在副本上操作：
- chatgpt 自带的 `rg`、`codex`、`codex-code-mode-host`、`node_repl` 都是 static-pie：`--print-interpreter` 和 `--print-needed` 输出都为空。对 `rg` 的副本执行 `patchelf --set-rpath /x` 后再运行，exit 139，问题复现。
- glibc 的 `ld-linux-x86-64.so.2` 同样没有 INTERP、没有 NEEDED。所以规则 1 顺带让自动 patch 不再改写 loader，之前就记录过改写 loader 会导致 exit 139。
- `libc.so.6` 有 NEEDED，照旧会被 patch。

改法：
1. **没有 `PT_INTERP` 且没有 `DT_NEEDED` 的文件跳过**，计入新增的 `skipped` 计数。
   - 位置：`_patch_elf_executables`、`_patch_elf_libraries`、fallback scan 三处。
   - 只对没有 INTERP 的文件额外跑一次 `--print-needed`，可执行文件不增加进程。
2. **异构的 ELF 跳过。**
   - `EI_CLASS` 和 `e_machine` 与 **loader 文件本身的**头部比较。没有 loader（只设 rpath）时，和宿主比较。
   - 现有的 `_is_elf_for_host` 定义了却从没被调用，一并换掉。
   - 只作用于自动路径和 set 路径。配方显式点名的 `M.set_rpath` / `M.set_interpreter` 不受影响。
3. **`set{scan, skip}` 生效。** 目前全仓库没有使用者、也没有文档，现在定义：
   - 都是相对 `install_dir` 的路径列表；
   - `scan` 替代"扫描整个目录树"；
   - `skip` 按路径前缀排除，一个目录项会排除它下面的所有内容；
   - v1 不支持通配符。
   - 写进 libxpkg 的文档。
4. **A3 需要的 sink API 也放进这个版本。** 一起发 **libxpkg 0.0.59**，xlings 随后升级依赖。

---

## 5. 交付与测试

| PR | 内容 | 依赖 |
|---|---|---|
| PR-L（libxpkg 0.0.59） | §4 的 1–3 + 构建沙箱的 sink | — |
| PR-1（xlings，输出层） | A1、A2、A2′、A3 | A3 依赖 PR-L |
| PR-2（xlings，config） | B1（§2.3–2.9）、B2（§3.3）、B4、B3、doctor `:676` 的补救命令 | — |

**新增或修改的测试：**
- **B1（e2e）：**
  - fixture 的 config 往一个计数文件里追加一行。
  - 覆盖这些场景：重复 install 不增加计数并提示 `--reconfig`；`--reconfig` 会重跑；第二个 subos 会跑（E2E-107 已有）；在 A 里 bump revision 后，B 里下一次 install 会重跑；remove 之后再 install 会重跑；没有记录的老 home 第一次 install 会跑并写入记录；interface 的 `reconfig` 字段；提前返回时仍然报告 `already_present`、仍然执行激活。
- **B2：**
  - 单元测试：`known_projects` 在文件没变时不重新解析，size 变了才重新解析。
  - e2e：project scope 下连续两次 install 只写一次 knownProjects；还要一个"同一个 plan 里，后面节点的 hook 能跑前面节点的 shim"的回归用例，fixture 仿照 gcc → gcc-specs-config 的形态。
- **A 系列：**
  - E2E-123：P2 的 update 那一半改成 0 帧 + 2 行；新增"最后一帧之后是一个空行"的断言。
  - #629 的验收标准；`self update` 的子进程不输出 `\033[`。
- **B4：** 单元测试，原样重录不告警、有变化才告警。

---

## 6. 自审：这一轮改掉了什么

1. B2 的"推迟到 plan 结束"被否决：有五个依赖方，gcc → gcc-specs-config 是直接用 shim 路径调用的。
2. B2 的成本归因纠正：大头不是 shim 表计算，而是重复读写 home 配置。
3. B2 (c)（给 project 贡献加缓存）不做：收益小，而且多一个回答者。
4. B2 (a) 从"启动时的快照"改成"跟随权威文件的 (size, mtime)"，否则锁重入的子进程登记的新 project 会被漏掉。
5. B1 从"纯推导"改成"推导 + 每个 scope 一条记录"，否则跨 subos 的 revision bump 传不过去，`registered==0` 的包也永远要重跑。
6. B1 提前返回时仍然要做 `record_report`、激活和一次同步，否则会破坏 R2、`SubosRuntimeMissing` 的修复，以及 AGENTS.md 写明的不变量。
7. B1 要豁免 `kind=Build` 的节点，否则提前返回永远触发不了。
8. doctor `:676` 的补救命令要改成 `--reconfig`，否则打印出来的命令执行后什么也不做。
9. `self update` 不需要 `--reconfig`：`cmd_use` 一定会同步，已核实 `:844`。
10. A3 从 fd 截获改成 libxpkg sink：`StdoutCapture` 不可重入，而且 CLI 自己的输出会被一起截走。
11. A2′ 发现下载渲染器是"要不要刷新光标"的第二个回答者；改成问 capabilities 之后，self update 可以复用 `--ui-mode cli`。
12. B3 从就地刷新的状态行改成每个节点一行：hook 的 `print` 绕过 console 锁。行的格式要避开 E2E-123 的正则。
13. B1 成立的前提是"config 不把 scope 相关数据写进共享 payload"；已核查 gcc 和三个写 sysroot 的配方，这条前提也写成了配方规则。
14. B1 的条件 2 不再读 stamp 的 `registered` 字段（老 stamp 里没有），改为只扫 ledger，没有注册条目时自动成立。
15. §3.4 的措辞改正：`self init` 拿锁后并不 reload。(a) 的正确性靠的是 (size, mtime) 检查，不依赖 reload。

## 7. 仍有的风险
- 手工删掉的 subos 效果（sysroot 链接、env section），不会再被一次普通的 `install` 修好。以后要用 doctor 或 `--reconfig`；之前这条修复路径本来也没有写进文档。
- 154 个 e2e 里可能还有依赖"重复 install 会重跑 config"的用例，grep 覆盖不全，要以 CI 全量运行为准。
- A2′ 对显式设置了 `uiMode=cli` 的用户是一个可见的行为变化，要写进 release note。
- 降级后又升级的情况：先降到旧版客户端，用它 remove 了一个 `registered==0` 的包（旧版不会删记录），再升回新版 install 它，这时 config 会被误跳过。注册过条目的包由条件 2 兜住。可以用 `--reconfig` 恢复。

---

## 8. 实施记录（2026-09-29）

| 仓库 | PR | 内容 |
|---|---|---|
| openxlings/libxpkg | #44 → 0.0.59 | §4 的规则 1–3；`build_index(repo, ns, BuildOutput)` |
| mcpplibs/mcpp-index | #485 | xpkg 0.0.59（三个平台块，GitHub 与 GitCode 字节一致） |
| openxlings/xim-pkgindex | #903 | qt-base / qt 的 linux 条目 `revision = 1`；贡献指南 §5.4 |
| openxlings/xlings | #633（2026.9.29.1） | A1–A3、B1–B4、doctor `:676`；另有 mcpp 2026.9.28.3 版本钉和一处 libc++ 修复 |
| openxlings/xim-pkgindex | #904 | xlings `latest` → 2026.9.29.1 |

与计划的差异，以及实现时才确定的细节：

- **B1 的记录**写在 scope 文件的顶层 `configured`，不写进 `workspace` 对象（旧客户端会把
  `workspace` 的每个键都当成 target 名）。内存里和 `installed[]` 一起加载，由
  `save_workspace` 写回。每个节点 config 成功后立即保存：进程中途被杀，没保存的部分下次重新配置。
- **B1 的删除点**：`uninstall` 的 detach 路径和删除路径；`apply_subos_env_ops_` 的 superseded
  分支，内存和它随后写回的文档两处都删。只删内存会被这次写回覆盖。
- **B2** 只做了 (a)（`knownProjects` 跟随文件的 size/mtime）和 (b)（`lastSeen` 每天最多写
  一次），(c) 不做。实现时又查到第五个依赖方：gcc 的 config 用路径直接调用
  `<bindir>/gcc-specs-config`。
- **A2′** 删掉了 `palette::cursor_rewrite_allowed`，它的单元测试改测 `ui::capabilities_of`，
  并补了 `--ui-mode cli` 的情况。
- **A3** 用一个进程级 observer 把 index 构建的步骤交给 interface，事件的写法与原来完全相同。
  脚本的其他行：CLI 下走 `log::println`，interface 下仍以 `[stray stdout]` 进 stderr。
- **B3** 按计划做成每个节点一行（`  [i/n] installed|configured <coord>`），外加 `configure`
  进度事件；interface 协议升到 1.4。
- 旁路发现（不在本轮范围内）：一个 Lua 语法错误的配方会从 catalog 里静默消失，报的是
  "not found … name is wrong or not published yet"。
