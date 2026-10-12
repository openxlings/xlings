# Luban 首次体验、卸载语义与大 home 性能：问题分析与设计方案

日期：2026-10-12　版本基线：2026.10.12.1　目标版本：按发布当天的日期——10-12 当天发布即 `2026.10.12.2`（单 PR，xlings 一个、xim-pkgindex 视需要一个）

**review 决定（第一轮）**：1. 批量清理只用 `--older-than`；2. 程序依赖且仍有可满足版本时直接允许并提示；3. `needs_rebind` 按 scope 运行时重装一份；4. 重跑 `luban new <name>` 补全；5. 视图内主题解析不到时静默回落；6. 版本按日期，`2026.10.12.x`。

来源：按《Luban OS 体验路线》使用 2026.10.12.1 的完整记录（`xlings self update`、`xlings update mcpp`、
`xlings remove`、`luban new`、`luban enter` 后 `mcpp new/run`、`xlings install nvim`）。本文逐条给出现象、证据、根因和修复，
并给出 CI 怎样把每一条固定下来。

---

## 0. 问题总表

| # | 现象 | 性质 | 根因（已确认 / 待复现） |
|---|---|---|---|
| P1 | `self update`、`update mcpp`、`remove` 很卡 | 性能 bug | 已确认一部分并已在 12.1 修复（投影整体重算）；真实使用中的大 home 上仍有残余，需按阶段计时定位 |
| P2 | 卸载 mcpp 的某个版本被"huxerui 依赖"拒绝 | 语义 bug | 反向依赖检查只比**包名**，不比版本，也不区分库绑定和程序依赖 |
| P3 | 需要"删除某版本之前的所有版本" | 新能力 | — |
| P4 | 旧 home 上 `luban new` 报 11 个失败 | 迁移 bug | root scope 要求旧客户端从未写过的证据（`.xlings-resolution.json`），并且旧记录指向的 glibc 不在这个 scope 中 |
| P5 | `luban new` 输出混乱 | 体验 | 三段子命令的输出拼接，没有统一的计划和进度视图 |
| P6 | 环境内 `mcpp run` 失败 | bug | 在 Luban root 内给另一个 home 执行 `xlings self init` 失败（待复现） |
| P7 | 环境内每条 xlings 命令都警告主题 | 体验 bug | home 的主题文件在环境的视图里看不到；警告每次都打印 |
| P8 | 环境内 `xlings install nvim` 让整个 `luban enter` 被挂起 | bug | 会话内的进程在后台进程组里改终端，被内核 SIGTTOU 停住 |
| P9 | 进入环境后是 `~ #`，看不出在哪 | 体验 | 没有默认 shell 和提示符的约定 |
| P10 | Luban 文档缺一条"拿来即用"的上手路径；README 没有 Luban | 文档 | — |

## 1. P1：大 home 上的命令耗时

### 1.1 已确认并修复（2026.10.12.1）

`use`、安装时的注册、`remove` 三处都通过**重算整个 SubOS 的投影**来求变化：遍历每个激活包的文件树，并重放全部链接。
修复后由 `materialize::prepare_projection_change` 统一处理，只物化移动了的名字。

用真实 home 的版本库（3,986 个目标）合成的大 SubOS（2,606 个激活目标、2,759 个 sysroot 链接），2026.10.12.1 实测：

| 命令 | 耗时 |
|---|---|
| `install mcpp@<旧版本>`（含下载） | 4.7 s |
| `use mcpp <v>` | 0.87 s |
| `remove -y mcpp@<旧版本>` | 0.79 s |
| `install mcpp --use` | 1.0 s |
| `list` | 0.61 s |

### 1.2 残余：真实使用中的大 home 与合成 home 的差别

合成 home 已经不慢，但真实使用中的大 home 上 `remove` 仍然卡。两者的差别正是下一步要量的东西：

- 有 rootfs SubOS（`luban-test-1`）：它的每次工作区变化都是一代；需要确认 default 的变化是否也会触发它的代（pinning 刷新路径）；
- 有 18 个 knownProjects：路由表要重建它们；
- `self update` 会在**每个** SubOS 和项目中重新指向过期的 shim；
- `self doctor --fix` 在合成大 home 上超过 5 分钟没有结束；
- 现象"确认 `y` 之后长时间没有输出"——需要知道这段时间耗在哪个阶段。

### 1.3 设计

1. **按阶段计时（`XLINGS_TRACE=timing`）**。每条命令按阶段打印耗时：解析计划、下载、每个节点的 hook、注册发布、投影、路由表、root 代、保存。
   - 用户报告时附上一行就能定位；
   - 在已有的 `observe` 模块上加一个 channel，不加新机制。
2. **长时间无输出就是 bug**。所有持有状态锁超过 2 s 的阶段都要显示进度，例如 `rebuilding routes for 18 projects…`；不再出现锁内的长时间静默。
3. **CI 性能预算**：
   - 在 testkit 中加一个**大 home 生成器**：版本库约 4,000 个目标；1 个 SubOS 有约 2,500 个激活目标和资产链接；加 1 个 rootfs SubOS 和 10 个 knownProjects。payload 用合成的头文件树，不依赖网络。
   - 以下操作各有预算，超出即失败：

     | 命令 | 预算 |
     |---|---|
     | `use` | 2 s |
     | `install --use`（已在 store） | 3 s |
     | `remove`（未使用的版本） | 2 s |
     | `self update` 的激活步骤 | 5 s |
     | `self doctor`（只读） | 10 s |

   - 计入 `PERF-*` 需求，失败时报告 `XLINGS_TRACE=timing` 的输出。
4. 由第 1、3 条量出来的残余热点逐个修，修法仍是"只做这次变化涉及的部分"。重点是 pinning 刷新和 rootfs 代，`doctor --fix` 单独评估。
5. 不在本轮：把版本库按目标拆开（约 1 s 的固定解析和写回开销）。只在第 3 条的预算证明它是瓶颈时再做。

## 2. P2：反向依赖检查要看版本、看绑定方式

### 2.1 现状

`direct_dependents_of` 对当前 SubOS 的每个已安装包读取 `.xlings-resolution.json`，只要某个依赖的 `name` 等于目标包名，就拒绝卸载。

问题：
- 没有比较**版本**：卸载 mcpp 2026.10.11.1 时，即使 huxerui 实际用的是另一个版本，或另一个版本仍可满足它，也会被拒绝；
- 没有区分**库绑定**和**程序依赖**：huxerui 只是在 PATH 上找 `mcpp`。

### 2.2 设计：只有"删了真的会坏"才拒绝

对依赖方 D 记录下来的每个依赖项，与要删的 payload P（名字 + 版本，即一个具体目录）比较：

| 情况 | 结果 |
|---|---|
| D 的记录指向 P，且有 `libdirs`（RPATH / INTERP 闭包在 P 内） | **拒绝**：D 的二进制直接链接着 P |
| D 的记录指向 P，但只有程序依赖（无 `libdirs`），且仍有另一个已安装版本满足 D 声明的约束 | **允许**，并提示：`huxerui uses mcpp from PATH; 2026.10.8.1 remains` |
| D 的记录指向 P，只有程序依赖，且删完后没有满足约束的版本 | **拒绝**，并给出能满足它的版本 |
| D 的记录指向另一个版本 | 与这次删除无关，**允许** |
| D 没有记录（旧客户端装的） | 按声明的约束判断：仍有满足约束的已安装版本就允许 |

- 程序依赖还有一种隐性的链接：依赖方在自己的载荷里写死了被删载荷的绝对路径（包装脚本、`*.cfg`）。判定"只是程序依赖"之前，扫描依赖方载荷中 `bin/`、`etc/` 下的文本文件，看是否出现 P 的路径；出现即按"链接"处理。只扫这两个目录，代价有界；
- `--force` 保留为逃生出口；
- `remove_blocked` 事件增加 `reason`（`linked` / `unsatisfied`），让 agent 也能区分。

### 2.3 测试

扩展单元测试 `test_payload_remove`，四种情况各一个用例；e2e：同一个包装两个版本，依赖方只用程序 → 删旧版本成功；依赖方链接库 → 删除被拒绝。

## 3. P3：批量清理旧版本

### 3.1 命令（`remove` 的一个选项，不新增命令）

```bash
xlings remove mcpp --older-than 2026.10.1   # 删除早于 2026.10.1 的所有已安装版本
xlings remove mcpp --older-than 2026.10.1 --dry-run   # 只列出会删什么
```

- 比较用版本约束的通用文法（`xim-pkgindex` 规范 §7.3 的同一实现）：严格小于给定版本；不能解析的版本键不参与，并在列表中注明。
- 只作用于一个包。不提供 `--all`：不同包的版本号之间没有可比性，按包名逐个清理才有意义。
- 交互确认（`[y/N]`）。

### 3.2 规则

- **从不删除**：
  - 任一 SubOS 中正在激活的版本，以及任一已知项目（`knownProjects`）的工作区中声明的版本；
  - xlings 自己正在运行的那个版本（入口二进制所在的版本）；
  - 被某个 SubOS 钉住的版本；
  - 按 P2 规则删了会坏的版本（被库绑定）；
  - 被保留的代（retained generation）引用的版本：照现有规则进入 `retained.json`，不立即删。
- 先列出将删除的版本、各自大小和总计，并逐一说明跳过的版本及原因，然后要求确认（`[y/N]`）。agent 模式必须同时给 `-y`；无人可问时以退出码 2 结束，什么都不改。
- 只删 payload 引用（派生数据），不碰任何 SubOS 的 home（用户数据规则不变）。
- 每次删除写入 `logs/destructive.ndjson`。
- interface：`remove_package` 增加 `older_than`（以及 `dry_run`），返回 `remove_plan`（含跳过的版本及原因）。

### 3.3 测试

e2e：装 4 个版本（1.0、1.1、1.2、2.0），激活 1.1、另一个 SubOS 激活 1.0，执行 `--older-than 2.0` → 只删 1.2，并说明跳过了 1.0、1.1 及原因；`--dry-run` 什么都不改；agent 模式不带 `-y` → 退出码 2。

## 4. P4：旧 home 上 `luban new` 失败（迁移）

### 4.1 证据

```
xim-x-patchelf/0.18.0/.xlings-resolution.json: missing checked runtime resolution; run owner `xlings install --reconfig` to migrate
xim-x-glibc/2.44: recorded runtime dependency is not opted into this root scope
```

- patchelf、linux-headers、make、ninja 是旧客户端装的：payload 在，但没有 root scope 要求的"已校验的运行时解析记录"；
- zlib 的记录指向 glibc `2.44`（旧 payload），而这个 root 的运行时是 `2.44.3`；
- 一个节点（patchelf）失败，依赖它的节点连锁失败，共 11 个。

### 4.2 根因

- 计划器把这些 payload 判定为"已在 store、当前、不用做事"；
- root scope 的发布器却要求旧客户端从未写过的证据，并且不接受指向这个 scope 之外运行时的记录；
- 两者对"这个 payload 能不能用在这个 scope"给出了两个不同的答案。错误信息还把修复（`--reconfig`）推给用户，而 `luban new` 本可以自己完成。

### 4.3 设计：计划阶段就把迁移做掉，只有一个回答者

1. 计划器为每个节点问发布器用的**同一个函数**：`evidence::usable_in_scope(payload, scope)`，返回四种结果之一：
   - `ok`；
   - `needs_record`：缺记录，能在本地重新解析；
   - `needs_rebind`：记录指向的运行时不在这个 scope；
   - `unusable`。
2. `needs_record`：在本节点的 config 前重新解析依赖并写入记录。不下载，不改 payload 的字节，只是迁移。计划中显示为 `migrate: record`。
3. `needs_rebind`：在这个 scope 里重新解析到 scope 的运行时（root 的 glibc 2.44.3），并**按 scope 运行时重装**这个 payload（决定 3）。
   - 约束：store 按 `<包>/<版本>` 存放，同一版本只能有一份 payload，不能真的"并存两份"。所以重装是**原地重装**（复用已有的 stale 目录交换机制：先下载并重定位到新位置，成功后原子替换，失败则回滚）；
   - 原地重装之前，先列出引用这个 payload 的所有 scope（SubOS、项目、root）和它们声明的运行时：
     - 都与新运行时一致，或者没有声明运行时 → 原地重装，计划中显示为 `reinstall: runtime 2.44 → 2.44.3`；
     - 有 scope 仍然声明旧运行时 → 不重装，报一个说清两边的错误：哪个 scope 用旧运行时，可用 `xlings subos runtime` 把它移到新运行时。这是真实的冲突，不能替用户选择；
   - 记录中的情况属于第一种：default 声明的已经是 `glibc@2.44.3`，旧的 zlib 对它也是过期的。
4. 错误信息中不再出现"让用户去跑 `--reconfig`"。能自动做的都在计划里做掉；只有 `unusable` 才报错，并说清原因。
5. 连锁失败：依赖方的失败合并为一行 `9 packages not installed because patchelf failed (see above)`。

### 4.4 CI：旧 home 场景（host matrix 新场景 `old-home`）

1. 装上一个已发布的 xlings（例如 2026.10.9.x），在 default 中装 patchelf、make、ninja、zlib、linux-headers 和旧的 glibc；
2. `xlings self update` 到待发布的候选版本；
3. `luban new dev core`：必须零错误、零失败节点；
4. `luban run dev -- mcpp new hello && cd hello && mcpp run`（覆盖 P6）；
5. `luban run dev -- nvim --version`。

在所有 Linux 腿上运行。新增需求：`UPGRADE-OLD-HOME-LUBAN`。

## 5. P5：`luban new` 的输出

### 5.1 现状的问题（按使用记录）

- edition 链 core ← tiny ← nano 打印成三段 `[1/1] installed subos:…`，像三个不相关的安装；
- 计划表中出现 `reinstall: recipe revision 4, installed r…`：截断了，而且对用户没有意义；
- 下载进度刷新了四次，中间夹着 `[mirror] … served by …`；
- 每个节点打印一行 `root generation N`，最后还有 `85 name conflict(s)`，没有解释；
- 失败按发生顺序穿插在成功之间，看不出根因；
- 最后给出一条 300 字符的 `xlings install …` 命令。

### 5.2 设计：一条命令，一个计划，一屏结果

```
luban new dev  (Luban Core 2026.10.11.2 · core ← tiny ← nano · x86_64)

  host       ready (bwrap, user namespaces)
  packages   21  (12 to download 186 MB · 3 updated · 6 present)
  ↓ ████████████████░░░░  63%  8.0 MB/s

  ✓ dev is ready — luban enter dev
    root generation 1 · 21 packages · policy open
```

失败时：

```
  ✗ dev is made but not complete
    patchelf failed: <一句原因>
    9 packages need it (gcc, binutils, zlib, …)
    luban new dev     finishes it   ·   luban rm dev   takes it away
```

- edition 链只显示一行；`reinstall` 的原因只在 `-v` 下显示，默认合并为 `N updated`；
- 镜像来源、每一代的编号、名字冲突只在 `-v` 或 `luban status dev` 中显示；默认只在结束时给一次总结；
- 名字冲突保留在 `status` 中，并附上解释（哪个包的哪个名字覆盖了哪个）；
- **重新执行同一条 `luban new dev` 就会补全未完成的环境**：幂等，按 `instance.json` 中已记录的 edition 继续。不再给出拼接出来的长命令；
- 实现：luban 的 `new` 用 `xim::cmd_install_step` 的结构化事件（已有计划、进度、节点事件），由 luban 自己渲染，不再转发 xlings 的逐行日志。`--json` / agent 模式输出同一份事件。

### 5.3 CI

非 tty 下 `luban new` 输出的黄金测试，在 fixture index 上运行：只断言稳定的行（标题、packages、ready、失败块的结构），不断言进度。失败场景用一个必然失败的 fixture 包验证合并后的失败块。

## 6. P6：环境内 `mcpp run` 失败

### 6.1 证据

```
Bundled xlings 2026.10.12.1 (from PATH: /usr/bin/xlings)
warning: `xlings self init` failed for sandbox at '/root/.mcpp/registry'
error: sandbox not initialized (missing subos/default/.xlings.json)
```

mcpp 用环境内的 `/usr/bin/xlings` 为自己的沙箱 home（`/root/.mcpp/registry`）执行 `xlings self init`，在 Luban root 中失败。

### 6.2 待复现，候选根因

1. root 中的 xlings 读取 `/etc/xlings/root.json`，把自己绑定到机器的系统 home，对另一个 home 的 `self init` 拒绝或写错位置；
2. `self init` 试图把新 home 的 `bin/xlings` 链接到"入口二进制"，而 root 中的入口是机器的 `/usr/bin/xlings`（只读的代）；
3. `/root` 在 root 中不可写，或者被投影成只读。

### 6.3 设计

- 原则：**一台 Luban 机器内，任何非机器的 home（mcpp 的沙箱、项目的 home）都必须能被 `self init` 和使用**。机器 home 是 `root.json` 指向的那一个；只有对它的修改需要遵守 root 的规则。
- 先用 4.4 第 4 步在 CI 中复现，再按实际根因修复。修复点可能在 xlings（多半如此）或 mcpp。
- mcpp 侧同时改进：`self init` 失败时把 xlings 的错误原文转出来，而不是只打印 `failed`。

## 7. P7：环境内的主题警告

### 7.1 现象

环境内每条 `xlings` 命令都先打印 6 行"主题文件不存在"的警告。`solarized` 不是 xlings 自带的主题，是用户自己放的文件，而它在环境的视图里看不到。

### 7.2 设计

1. **同一个原因的警告只说一次**：主题加载失败按"配置值 + 原因"记入 `hintsSeen`。之后静默回落到默认主题；配置变化后重新判断。符合诊断规范："降级要说出来"，但只说一次。
2. **视图内静默回落**（决定 5）：在 SubOS 视图或 root 内（`XLINGS_SUBOS_MODE` 已设置，或 `/etc/xlings/root.json` 存在），主题解析不到时静默使用默认主题，不警告——环境不是用户配置主题的地方；在宿主上仍按第 1 条警告一次。

## 8. P8：环境内的安装让会话被挂起

### 8.1 现象

`luban enter` 后执行 `xlings install nvim`，宿主 shell 报 `Job 1, 'luban enter luban-test-1' has stopped`。

### 8.2 根因（与 AGENTS.md 已记录的 SIGTTOU 规则一致）

会话内有一个进程不在前台进程组，却去设置终端模式（安装界面的光标、宽度探测，或者 hook 子进程），被内核用 SIGTTOU / SIGTTIN 停住，导致 `luban enter` 整个被挂起。

### 8.3 设计

1. 所有改动终端模式的代码（TUI、终端查询、行内进度）先检查 `tcgetpgrp(fd) == getpgrp()`；不在前台就退回纯文本、不查询终端。集中在 `platform` 的终端模块中一处判断。
2. 入口：`luban enter` / `subos enter` 交给会话的 shell 一个前台进程组，并在 relay 中正确转发作业控制信号。核对 `relay_terminal` 的进程组处理。
3. CI：在 pty（`script -qec`）中执行 `luban enter dev`，在里面装一个小包，断言会话没有被停住、安装完成、退出码为 0。新增需求 `SES-TTY-JOBCTL`。

## 9. P9：进入环境后的 shell 和提示符

- edition 声明自己的 shell：core 和 agent-workspace 用 `bash`（core 里也有 fish），tiny 和 nano 用 `sh`。`luban enter` 使用它；用户可用 `luban config dev shell fish` 覆盖。
- 提示符：luban 在环境的 `/etc/profile.d/luban.sh`（以及 fish 的 `conf.d`）写入
  `PS1='[luban:dev] \w \$ '`，busybox sh 通过 `ENV` 读取。
- 进入时打印一行欢迎信息（可关闭）：`Luban Core 2026.10.11.2 · luban status · exit to leave`。
- 测试：在 pty 中 `luban enter dev`，断言提示符含 `[luban:dev]`，`$SHELL` 是 edition 声明的 shell。

## 10. P10：文档

1. 新增 `docs/luban/01-quick-start.md`（及中文版）：内容就是这次的体验路线——环境 → 开发 → 回滚 → agent 工作区 → 升级 → 镜像与 `luban try` → 写 U 盘（可选）→ 清理。每步写明"你应该看到什么"。现有的 `01-getting-started` 并入它。
2. README（中英）新增一个很短的 Luban 小节，用最少的命令给人"哇"的感觉：

   ```bash
   luban new box tiny        # a whole Linux userland, rootless, in seconds
   luban try box             # boot it as a machine (qemu, UEFI)
   luban export box box.iso  # a bootable ISO — BIOS and UEFI, x86_64 and aarch64
   ```

   附一句定位和一个链接。不放截图。
3. `test_docs.py` 已经覆盖这两处的命令校验和中英对照。

## 11. 遗留项（上一轮登记 + 本轮发现）

| 项 | 本轮是否处理 |
|---|---|
| `qemu-arm` / `qemu-x86` 的成员没有绑定到 release，`self doctor` 误报为损坏的 payload | 处理（index：绑定到包的 release，revision 加 1） |
| 全新 home 上"声明了 glibc 运行时但未安装"的警告 | 处理：声明了但没有任何包需要时是一条 Notice，不是 Warning |
| `self doctor --fix` 在大 home 上超过 5 分钟 | 按 1.3 的计时找出热点；本轮至少要有进度显示 |
| 版本库按目标拆分 | 不处理（见 1.3 第 5 条） |
| 11 个配方注释中的旧文档路径 | 不处理（随配方下次修改） |
| actions 升级 | 不处理（已决定延后） |

## 12. 任务与依赖

| ID | 任务 | 依赖 | CI |
|---|---|---|---|
| A1 | `XLINGS_TRACE=timing` 按阶段计时；持锁超过 2 s 的阶段显示进度 | — | 单元 |
| A2 | 大 home 生成器 + 性能预算 e2e | A1 | linux 腿 |
| A3 | 按 A2 量出的热点修复（pinning、rootfs 代、self update 的 shim 重指） | A2 | 同上 |
| B1 | 版本感知的反向依赖检查 | — | 单元 + e2e |
| B2 | `remove --older-than`（含 `--dry-run`） | B1 | e2e + interface |
| C1 | `usable_in_scope` 单一回答者；计划阶段迁移记录，按 scope 运行时重装 | — | e2e |
| C2 | host matrix `old-home` 场景 | C1 | host matrix |
| C3 | 复现并修复环境内的 `self init`（mcpp 沙箱） | C2 复现 | host matrix |
| D1 | `luban new` 统一渲染（计划、进度、结果、失败合并），重跑即补全 | C1 | 黄金测试 |
| D2 | 终端前台检查；enter 的进程组与作业控制 | — | pty e2e |
| D3 | edition 的 shell 与 `[luban:<name>]` 提示符 | — | pty e2e |
| D4 | 主题警告只说一次；视图内静默回落 | — | 单元 |
| E1 | `docs/luban/01-quick-start`（中英）+ README 的 Luban 小节 | D1、D3（输出示例要与实际一致） | test_docs |
| E2 | index：qemu-arm/qemu-x86 绑定；edition 声明 shell | D3 | index CI |

关键路径：C1 → C2 → C3（真实体验能否跑通）；A1 → A2 → A3（性能）。B、D、E 与它们并行。

## 13. review 决定（第一轮，已采纳）

| # | 问题 | 决定 |
|---|---|---|
| 1 | 批量清理的选项 | `--older-than V`（不提供 `--keep`、`--all`） |
| 2 | 程序依赖且仍有可满足版本 | 直接允许，并提示用户 |
| 3 | `needs_rebind` | 按 scope 运行时重装一份同版本 payload |
| 4 | 补全未完成的环境 | 重跑 `luban new <name>`，幂等；不新增命令 |
| 5 | 视图内主题 | 解析不到时静默回落，不投影 `config/themes` |
| 6 | 版本 | 按发布当天日期：`2026.10.12.x`（10-12 当天即 `2026.10.12.2`） |

## 14. 自我 review（第二轮，多视角）

| 视角 | 发现 | 处理 |
|---|---|---|
| 正确性（P4） | 决定 3 的"重装一份"按字面做不到：store 中同一版本只有一个目录 | 改为"引用它的 scope 都同意新运行时就原地重装，有 scope 仍用旧运行时就报真实冲突"（§4.3 第 3 条）。需要 review 确认这个细化 |
| 正确性（P2） | 程序依赖也可能把被删 payload 的绝对路径写进包装脚本 | 判定前扫描依赖方 `bin/`、`etc/` 下的文本文件（§2.2） |
| 安全（P3） | 项目工作区声明的版本、xlings 自己正在运行的版本，原方案没有列入保护 | 已加入"从不删除"（§3.2） |
| 简洁（P3） | 只保留 `--older-than` 加 `--dry-run`；不同包的版本不可比，不提供 `--all` | §3.1 |
| 兼容（interface） | `remove_blocked.reason`、`remove_package.older_than`、`remove_plan` 都是增补 | 协议升到 1.8（只增补）；同步更新 `docs/specs/interface-ndjson-v1.md` |
| 一致性（命令树） | `--older-than` 是现有命令上的选项，属于代价最小的一类 | 同步更新命令 spec、补全、`docs/generated/command-reference.md`、`docs/03-packages-and-versions.md`（中英），并补测试 |
| 跨平台（P8） | Windows 没有进程组 | `platform` 中的前台判断在 Windows 上恒为真；只在 POSIX 上检查 |
| 体验（P5） | 重跑 `luban new` 时，实例已存在但 edition 不同，或者用户改过包 | 实例记录的 edition 与请求一致才继续补全；不一致就拒绝并说明。补全只装 edition 缺的包，不动用户装或删过的 |
| 体验（P9） | busybox sh 只在设置了 `ENV` 时读启动文件；bash 只有作为 login shell 才读 `/etc/profile` | 会话设置 `ENV=/etc/profile.d/luban.sh`；bash 以 `--login` 启动；fish 用 `conf.d` |
| 可回归（CI） | `old-home` 场景要钉一个旧版本，而 `no_xlings_version_pin_check` 禁止在 fresh-install 中钉 xlings 版本 | 场景放在 host matrix（不是 fresh-install）；旧版本只作为升级起点，写明原因，和 `MCPP_OLD` 同类 |
| 风险（P6） | 根因还没复现；修复也可能落在 mcpp 仓库 | 先由 `old-home` 场景复现；若在 mcpp 侧，另开 mcpp PR，在 xlings PR 中记录对应的 mcpp 版本 |
| 文档（P10） | README 里"几秒内"不准：首次 `luban new box tiny` 要下载 | 改为"一条命令得到完整的 Linux 用户态，rootless" |
| 性能（A2） | 大 home 生成器不能让 CI 太慢 | 合成的 payload 只放空头文件（约 2,500 个小文件，几 MB），生成在 30 s 内 |

仍需 review 的一点：**§4.3 第 3 条对决定 3 的细化**——原地重装，加上在真实冲突时拒绝。

## 15. 实现结果（2026.10.12.2）

| # | 实际根因 / 做法 | 与设计的差异 | 测试 |
|---|---|---|---|
| P1 | 大 home 上 `remove` 一个未使用的版本 28.6 s：`uninstall` 开头的借用 payload 扫描每个条目都调用一次 `Config::versions()`（按值返回整个版本库），是平方级复制；安装时每注册一个程序也复制一次。改为扫描前取一次。28.6 s → 1.9 s（未优化构建）。剩余的每节点成本是版本库写回与回读校验（3.4 MB 时约 1.2 s，未优化），仍按 §1.3 第 5 条不在本轮 | `XLINGS_TRACE=timing` 用 `observe::Timed` 实现；超过 2 s 的阶段（路由重建、root 代）打印一行"正在做什么" | `HomePerf`（`test_home_perf.cpp`，静态性能通道，5,000 条注册、10 个项目，6 个预算）。`self update` 的激活步骤未纳入（夹具里没有 xlings 包） |
| P2 | 按设计 | 无记录的依赖方：被删 payload 有 `lib/` 就按"链接"拒绝（无法知道它链接的是哪个版本）；记录指向的版本已不存在时按"程序依赖"判断 | `RemoveVersions.OnlyARemovalThatBreaksADependentIsRefused` |
| P3 | 按设计；跨 SubOS：候选版本在每个安装了它的 SubOS 中各删一次 | 无 | `RemoveVersions.OlderThan…` |
| P4 | 按设计，在计划器做（`resolver.cpp: scope_evidence_`），只在目标 scope 是 root 时；`migrateRecord` 写记录，`rebindReason` 走已有的过期 payload 替换（原地重装）；冲突是计划错误 | 无记录时用 ELF 的 INTERP/RUNPATH 判断能否只写记录 | `SubosNewTemplate.AnOlderClientsPayloads…`、`…RebuiltOrTheConflictNamed`；host matrix `oldhome` |
| P5 | edition 一行；模板安装不打印逐节点行；每代一行降为 debug；依赖失败合并为一行（interface 增加 `install_skipped`）；`instance.json` 的 `making` 记录请求，重跑同一条命令补全 | 没有做 luban 侧的独立渲染器（xlings 的输出改好后不需要第二个渲染器） | `SubosNewTemplate.TheEditionIsOneLine…` |
| P6 | 沙箱内 `XLINGS_HOME=<自己的 home> xlings self init` 被当作只有宿主才能做的事拒绝（broker 路由不看 home）。改为：home 在沙箱内可写就在内部执行；只有会话的只读 home 走 broker。顺带：`--help` 不再转发给 broker（之前打印的是宿主 xlings 的帮助） | 修复在 xlings，mcpp 不用改 | `SubosHomeReadOnly.AHomeOfTheSandboxsOwnIsWrittenInside`；host matrix `oldhome` |
| P7 | 按设计 | 无 | `ThemeNotice` |
| P8 | 终端背景色查询（`query_terminal_is_light`）在后台进程组里 `tcsetattr`，SIGTTOU 停住整个组；会话里 broker 的子进程正是这样一个后台组，而它与 `luban enter` 同组。修复：查询前检查前台；UI 把"不在前台的终端"当作非终端；broker 的子进程有自己的会话 | 无 | `TerminalJobControl`（testkit 新增 `pty_background`；在 12.1 上失败）；host matrix `terminal` |
| P9 | root 里有 bash 就用 bash，否则 sh；提示符经 `PS1` 环境变量 `[luban:<name>]`；进入时的一行说明 edition 和 `luban status` | 不新增 `shell` 设置、不改 edition 清单（按是否有 bash 判断已覆盖 core 与 tiny/nano）；不写 `/etc/profile.d` | host matrix `terminal` |
| P10 | `docs/luban/01-quick-start.md`（替换 01-getting-started）与 README 的 Luban 小节 | 无 | `test_docs.py` |
| 遗留 | qemu-arm / qemu-x86 绑定到 release（index，revision 1）；全新 home 上未被需要的运行时声明是 Notice | 无 | index 静态测试；`subos_runtime_authority_test.sh` |

CI 上发现并修复的三点（第一轮 PR CI）：

- **前台判断只对控制终端成立**：SIGTTOU 只来自进程的控制终端；stdin 是一个非控制终端的 pty（`non_interactive_contract_test` N8）时 `tcgetpgrp` 失败，最初被当作"后台"，确认提示因此消失。改为：`tcgetpgrp` 失败的终端没有作业控制，按前台处理。
- **运行时 Notice 不提前返回**：Notice 分支一度提前 `return`，跳过了后面的声明与 sysroot 不一致检查（`subos_runtime_describe_test` S4）。
- **没有 bwrap 的 home 上做 root**：`oldhome` 场景在 u-fedora 上暴露：预检只警告"没有 bwrap"，edition 装到 glibc 时才因生成 loader 缓存需要 bwrap 而失败。改为预检时先装 `xim:bwrap`（T1，与 `luban try` 装模拟器同理）。
- 场景本身：旧客户端（2026.10.9.2）不能在同一计划里用刚装的 patchelf 重定位（12.1 已修），所以先单独装 patchelf；`luban enter` 的输入要在 shell 起来之后再输入，提前输入的字节会被启动时的终端颜色查询读走。
