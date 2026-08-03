# PR #472 架构化改造 —— 从"契约文本"到"可证伪的事实源"

**日期**: 2026-08-03
**类型**: 架构评估 + 优化方案 (plan)
**基线**: `origin/feat/multiplatform-ux-contracts` @ `636e0d4`（PR [#472](https://github.com/openxlings/xlings/pull/472)，Draft，73 文件 / +4308 −433）
**主干**: `3b3c055` = `2026.8.2.1`
**前序**: `.agents/docs/2026-08-03-multiplatform-user-experience-survey.md`（本 PR 自带的调研）
**涉及仓库**: `openxlings/xlings`（全部）；`openxlings/xim-pkgindex`（§2.1 必须联动）；`xlings-res`（无）
**相关 issue**: [#471](https://github.com/openxlings/xlings/issues/471)

---

## 0. 摘要与分批

PR #472 想解决的问题是真的，方向也对：**xlings 在四个发布目标上的行为没有被任何东西约束住**。
它交付的 candidate 冷家生命周期门禁、#471 修复、以及把 README 里"macOS/Windows 能隔离不可信代码"
这句错误的安全声明改掉 —— 这三件是本 PR 最有价值的产出，应当尽快单独合入。

但它在实现"约束"的时候，**把契约写成了第二份文本，然后让测试去读那份文本**。于是出现了一个
反复出现的形状：*一条契约被声明了，一条测试断言了这条声明存在，而没有任何东西验证被声明的事情
真的发生。* 本项目 memory 里管这个叫 silent-success；这次它同时出现在 CLI 规格、架构门禁、
安装脚本、发布门禁四个地方。

与此同时，PR 把三处"更严格"实现成了"更早地拒绝"，判据本身没变准，只是覆盖面变大了 ——
**其中 arch 门禁这一条，会让 Apple Silicon 上的 `xlings install go` 和 aarch64 Linux 上的
`xlings install git` 直接拒装。**

| 批次 | 主题 | 架构主线 | 独立价值 | 阻塞关系 |
|---|---|---|---|---|
| **A** | 直接合：#471 修复、candidate 门禁、文档诚实化、`xself` 错误事件化 | — | 已验证的净收益 | 无 |
| **P0-1** | 资产可用性判据下沉到 per-OS 解析（**否则 aarch64/arm64 大面积拒装**） | A3 | 恢复 macOS arm64 与 Linux aarch64 的可用性 | 与 pkgindex 扫描联动 |
| **P0-2** | 恢复 quick-install 的**执行式**端到端覆盖 | A1 | 最面向用户的路径当前覆盖为零 | 无 |
| **P0-3** | 交互 shell 与一次性命令拆成两种显式进程模型 | A4 | POSIX 交互会话的信号/作业控制正确性 | 无 |
| **P1-1** | CLI 事实源单向化 + **双向**执行式对账 | A1 | 消灭 spec↔parser 漂移（今天已有一处真漂移） | 无 |
| **P1-2** | 全局选项归属明确（`-y/--yes`） | A3 | agent/脚本可用性 | P1-1 |
| **P1-3** | `platform::Target` 值对象 + 能力注册表，消灭散落 `#if` | A2 | 跨平台一致性的结构保证 | 无 |
| **P1-4** | 输出策略三元组：颜色 / 光标重写 / 活进度 解耦 | A4 | `NO_COLOR` 用户的进度反馈 | 无 |
| **P2-1** | inventory 采集方向反转 + catalog 载入缓存 | A3 | `list`/`info` 性能与正确性 | 无 |
| **P2-2** | 发布链真实性：sidecar 格式、门禁去网络抖动 | A1 | 发布可靠性 | 无 |
| **P2-3** | 文档产品化：能力矩阵分层、生成物再生、agent 参考补全 | A1 | 用户对"支持"的理解不被误导 | P0-1 |

**落地建议**：A 立刻合；P0 三条各自独立 PR（互不阻塞）；P1 作为一个串；P2 排后。
**不建议**以当前形态整体合入 —— 主要不是因为缺陷数量，而是因为 P0-1 与 pkgindex 的联动被
PR 的 Boundaries 明确排除在外，而它是一个会立即生效的破坏性变更。

---

## 1. 三条架构主线

本 PR 的 20 余条发现里，绝大多数不是独立缺陷，而是三条主线的投影。**修主线比逐条修投影划算**，
因为投影会再长回来。

### A1 —— 契约有第二份副本，验证读的是副本

| 契约 | 第一份（事实） | 第二份（副本） | 验证读的是 |
|---|---|---|---|
| CLI 表面 | `cli.cppm` 的 cmdline builder + `subos.cppm`/`xself.cppm` 的手写 argv 循环 | `cli/spec.cppm` 的 `CommandSpec` | **副本**（help 由 spec 渲染，测试再断言 help 含 spec 的选项） |
| 资产可用性 | `pkg.xpm[os][version]` 里那条资源条目 | 包级 `pkg.archs` 并集 | **副本**（`check_target_compatibility` 只读 archs） |
| 安装脚本行为 | `quick_install.sh` 的执行结果 | 脚本的源码文本 | **副本**（`grep -F 'verify_sidecar'`） |
| 发布门禁 | `release.yml` 实际跑出来的 job 图 | YAML 的文本 | **副本**（正则 match job 名） |
| 沙箱 shell | `run_shell` 内部重解析的 `$SHELL` | `payload["shell"]` 事件字段 | 无人验证（两者已经不一致） |

**判据**：一条测试如果**在被测对象完全不工作时仍然通过**，它验证的是副本。
`test_generated_command_reference.py::assert_help` 的报错文案自称 `CommandSpec/parser help drift`，
但那条路径上根本没有 parser —— help 就是 spec 渲染的，它在拿 spec 和 spec 比。

**统一解法**：
1. **单向派生**：副本必须由事实源生成，而不是并行维护。（spec → parser，而不是 spec ∥ parser）
2. **可证伪对账**：当过渡期必须并存两份时，对账测试必须**执行**事实源，并且必须能指出一个
   今天就存在的真实分歧。一条新加的对账测试如果第一次跑就是绿的，说明它没在对账。
   §3.1 的 driver 的验收判据就是：**它必须在 `--shell powershell` 上失败。**

### A2 —— 平台差异处理在叶子，而不是边界

当前 `#if` 的分布：`platform.cppm`（4 处，且 Windows 认 `XLINGS_SHELL`、POSIX 只认 `SHELL`）、
`sandbox.cppm`（3 个平台分支 + 一个 `#if defined(__aarch64__)` 的能力硬编码）、
`compatibility.cppm::host_architecture()`（编译期常量却叫 host）、
`inventory.cppm`（又一处 `platformName`）、`installer.cppm`、`resolver.cppm`。

每一处都独立地回答"我在哪个平台上、这里能做什么"，于是每一处都可以独立地答错 —— 本 PR 就在
一个叫"multiplatform contracts"的改动里新引入了 3 处不一致（§4.1）。

**统一解法**：一个在进程启动时构造一次、向下传递的 `platform::Target` 值对象，
加一个把"能力"变成**运行期数据查询**而非编译期常量的注册表。见 §4.1。

### A3 —— "更严格"被实现成"更早地拒绝"，而不是"更准确地判断"

| 变更 | 判据变准了吗 | 覆盖面 | 结果 |
|---|---|---|---|
| arch 门禁去掉 `spec >= 2` 闸门 | ❌ 仍是包级 `archs` 并集 | V2 → 全部 | ~60 个 V1 配方误伤 |
| `validate_manual_argv` 取值白名单 | ❌ 硬编码 5 个 shell 名 | 新增 | `--shell powershell` 误伤 |
| `cursor_rewrite` 纳入 `NO_COLOR` | ❌ NO_COLOR 与光标控制无关 | 新增 | NO_COLOR 用户零进度反馈 |
| `reject_surplus` | 部分 | 新增 | `xlings self clean -y` 误伤 |

**统一解法**：fail-closed 需要**证据强度**做前置条件。把"我有多确定这个目标不被支持"显式建模：
强证据（条目本身携带 per-arch 信息）才允许拒绝，弱证据（只有包级并集）只能告警。
这条规则可以写成代码，且它**天然复现了旧 `spec >= 2` 闸门的意图**，但判据从"作者声明的
spec 版本号"（一个承诺）变成"这条资源条目有没有携带架构信息"（一个事实）。见 §2.1。

### A4 —— 语义变更搭便车

| 声明的动机 | 搭车的语义变更 | 有能失败的测试吗 |
|---|---|---|
| 统一 `--cmd` 的跨平台退出码 | POSIX 交互 shell 从 `exec` 替换改为 `fork`+`wait` | ❌ 新增测试全用 `--cmd` |
| CI 日志静态化 | `NO_COLOR` 用户在真终端上失去全部进度反馈 | ❌ |
| help 统一由 spec 渲染 | `xlings agent -h` 从纯文本技能总览变成 TUI 盒子 | ❌（测试只查有无 `\x1b`） |

**统一解法**：**每个语义变更必须有一条指名它的、能失败的测试**；做不到就拆开。
这不是流程口号 —— 上面三条如果各自有一条测试，写测试的过程本身就会暴露"我改的不止一件事"。

---

## 2. P0 批次

### 2.1 P0-1 —— 资产可用性判据下沉到 per-OS 解析

#### 现场

`installer.cppm:1997` / `resolver.cppm:149` 用 `check_target_compatibility` 替换了旧门禁，
旧门禁的 `specSupport.declared >= 2` 闸门连同解释它的注释一起被删除：

> V1 的 `archs` 从未被强制且普遍少声明（例：recipe 写 x86_64-only，实际通过 XLINGS_RES
> 解析出 aarch64 资产），强制会破坏本来能装的包。

对真实索引（`xim-pkgindex`，130 配方 / 99 个声明 `archs`）的核对：

```
spec="1" + archs={"x86_64"}：git go make cmake gcc binutils glibc zlib expat glib
                              vim fish dash busybox bwrap xvm mingw-w64 freetype …
带 macOS 支持但 archs 无 arm64（11 个）：go rustup npm pnpm nvm ollama mdbook xvm
                                          d2x busybox virtualbox-ubuntu
```

最干净的证据是 `go.lua`：

```lua
package = {
    spec = "1",
    archs = {"x86_64"},                                            -- 包级并集
    xpm = {
        linux  = { url_template = ".../go{version}.linux-amd64.tar.gz"  },
        macosx = { url_template = ".../go{version}.darwin-arm64.tar.gz" },  -- ← 实际资产
    },
}
```

`arch_matches` 的语义是"两边归一化后相等"，`x86_64` 与 `aarch64` 永不匹配。
所以本 PR 会让 **Apple Silicon 上的 `xlings install go` 拒装它本来就要下载的 `darwin-arm64` 包**。

放大因素：现在有**两处**门禁（resolver 计划期 + installer 执行期）。resolver 那处把错误
push 进 `plan.errors` 并 `return false`，意味着**一个传递依赖不兼容会让整个安装计划失败**，
而不是跳过该节点。

#### 根因（A3 + A1）

`archs` 是**包级**字段，`xpm` 的资产是**按 OS 分组**的。
`check_target_compatibility(pkg, os, arch)` 收下了 `os` 参数，却只拿它拼展示标签，
**从不参与匹配**（`compatibility.cppm`）。它读的是副本，不是事实。

#### 方案

判据下沉到"选定 OS 分支后的那一条资源条目"，并引入证据分级：

```cpp
// src/core/xim/compatibility.cppm
enum class ArchEvidence {
    Strong,   // 条目自身携带架构信息
    Weak,     // 条目只有单一资产，包级 archs 是唯一线索
    None,     // 包未声明 archs
};

struct TargetCompatibility {
    bool         supported { true };
    ArchEvidence evidence  { ArchEvidence::None };
    std::string  target;                  // "<os>-<arch>"
    std::vector<std::string> supportedTargets;
    std::string  advisory;                // Weak 且不匹配时的一次性告警文案
};

// entry = pkg.xpm[os][version]（已 ref 解引用）
// Strong 的判据（三者任一）：
//   1. entry 是 per-arch map（archs 子表 / sha256_by_arch 多键）
//   2. entry.url_template 含 ${arch} / {arch} 占位
//   3. entry.is_res 且 res 清单声明了多架构
TargetCompatibility check_target_compatibility(
    const mcpplibs::xpkg::Package& package,
    const mcpplibs::xpkg::Resource& entry,   // ← 新增：必须传具体条目
    std::string_view os,
    std::string_view arch);
```

决策表：

| evidence | 该 arch 在条目里存在 | 动作 |
|---|---|---|
| Strong | ✅ | 放行 |
| Strong | ❌ | **拒绝** `E_UNSUPPORTED_TARGET`，零下载、零 hook |
| Weak   | 包级 `archs` 命中 | 放行 |
| Weak   | 包级 `archs` 不命中 | **放行 + 一次性 advisory**，并记入 `self doctor` 可见位置 |
| None   | — | 放行 |

这条规则的三个性质：

1. **天然复现旧闸门的意图** —— V2 作者才写 per-arch 资产，所以 Strong ≈ 旧的 `spec >= 2`；
   但判据从"作者声明的版本号"（承诺）变成"条目携带了架构信息"（事实）。
2. **自动修好 `go.lua` on macOS arm64** —— `macosx` 分支是单一 `url_template`，Weak → 放行。
3. **给出无需再动客户端的升级路径** —— pkgindex 侧把条目补成 per-arch，evidence 自动升为
   Strong，门禁自动生效。

同时把 resolver 那处的 `return false` 改为**只标记节点**、不中断整个计划：一个可选依赖不兼容
不应该让主目标装不上。拒绝仍在 installer 的执行期生效（那里已有 `refusedNodes` 机制）。

`sandbox.cppm` 里的 `#if defined(__aarch64__)` 一并删除，改为 §4.1 的运行期能力查询。

#### 联动（必须同批）

- **xim-pkgindex**：对 99 个声明了 `archs` 的配方做一次扫描，凡是 `xpm` 里存在该 OS 分支
  但 `archs` 未覆盖对应架构的，补齐 `archs` 或升级为 per-arch 条目。11 个 macOS/arm64
  错配的优先。**这项不做，Weak 放行只是把误伤换成了长期告警噪音。**
- **README**：能力矩阵必须区分「发布产物矩阵」与「包生态覆盖」，见 §5.1。

#### 验收判据

- 新增 e2e：用真实索引的 `go.lua` 形状（macOS 分支 arm64 URL + 包级 x86_64）在 arm64 上
  **必须安装成功**；本 PR 现有的 `aarch64_compat_contract_test.sh` 的 `arm-incompatible`
  fixture 改成 per-arch 条目（Strong），断言仍然拒绝且零请求。
- 新增单测：`ArchEvidence` 的三条 Strong 判据各一例、Weak 一例、None 一例。
- 差分：在 aarch64 容器里对真实索引跑一遍 `resolve` 全表，输出被拒的包名清单，**必须为空**
  （或只剩已在 pkgindex 补齐的 Strong 项）。这条是防止再次误伤的唯一可信手段。

---

### 2.2 P0-2 —— 恢复 quick-install 的执行式端到端覆盖

#### 现场

`tests/scripts/test_quick_install.sh` 的全部实质内容：

```bash
bash -n "$SCRIPT"
grep -F 'linux-x86_64|linux-aarch64|macosx-arm64' "$SCRIPT" >/dev/null
grep -F '"${url}.sha256"'  "$SCRIPT" >/dev/null
grep -F 'verify_sidecar'   "$SCRIPT" >/dev/null
latest=$(printf '%s\n' 2026.8.3.9 2026.8.3.10 | sort -t. -k1,1n -k2,2n -k3,3n -k4,4n | tail -1)
[[ "$latest" == "2026.8.3.10" ]]     # 测的是 sort(1)，不是脚本
```

`.ps1` 版本是 6 个 `$script.Contains($needle)`。它替换掉的是：

- macOS CI `E2E-03` → 原 `bash tests/e2e/release_quick_install_test.sh`（真跑）
- Windows CI `E2E-03` → 原 `irm .../quick_install.ps1 | iex` + 校验安装结果 + `xlings search`

并且同时移除了 `continue-on-error: true`。**观感上变严，实质上唯一的真实覆盖归零。**

风险叠加：本 PR 给 quick_install 新增的 sidecar 校验是**硬失败**（拿不到 `<url>.sha256`
就换源，全部失败则退出 1）。项目记录里已经发生过"GitCode 侧只有 4 个 `.sha256`，4 个大包全缺"。
这条硬失败路径现在只由 `grep -F 'verify_sidecar'` 守着。

#### 方案

把"真跑"和"不依赖外网限流"这两个诉求解耦 —— 它们不冲突：

```bash
# tests/scripts/test_quick_install.sh  （重写）
# 1. 起本地 HTTP server，用【本次构建的产物】伪造一个 release 源
#    目录布局与真实 release 资产一致：<name>.tar.gz + <name>.tar.gz.sha256
# 2. 通过已有的源覆盖机制注入（新增 XLINGS_INSTALL_BASE_URL，默认走真实源）
# 3. 在隔离的 XLINGS_HOME/HOME 下真跑 quick_install.sh，断言：
#      - 装出来的 xlings 可执行且 --version 正确
#      - 篡改 sidecar → 必须失败且不落地任何二进制
#      - 删除 sidecar → 必须失败（而不是静默跳过校验）
#      - NO_COLOR=1 → stdout 无 \033
```

关键设计点：**新增的 `XLINGS_INSTALL_BASE_URL` 本身就是可测的事实源**，它让脚本在 CI 里
走的是和真实用户完全相同的代码路径（解析 → 下载 → 校验 → 解包 → self install），只是源换了。
`grep` 断言全部删除。

保留一条**真外网** smoke，但放在 nightly / release 后验证，且允许 `continue-on-error`
—— 那才是 `continue-on-error` 的正确用法（限流是环境问题，不是回归）。

`test_release_candidate_gate.py` 同理：它 grep `release.yml` 的 job 名和 smoke 脚本的字面量。
保留其中"防误删"的结构断言（有价值），但删掉 `if "--sandbox" not in smoke` 这类断言字面量
存在的部分 —— 它们只会在有人重构脚本时误报，从不在行为退化时报警。

#### 验收判据

- 篡改 sidecar 的用例**必须在当前实现下通过**（说明校验真的生效），且在把 `verify_sidecar`
  调用注释掉后**必须失败**（说明测试真的在测它）。这条双向检查是本项 P0 的核心验收。
- macOS 与 Windows CI 各有一次执行式 quick-install 运行。

---

### 2.3 P0-3 —— 交互 shell 与一次性命令拆成两种显式进程模型

#### 现场

`subos.cppm::use_spawn_shell` 的整个 POSIX 分支被替换为 `platform::run_shell(cmd, cmd.empty())`。
被删掉的注释写明了原设计意图：

> POSIX: exec(2) replaces the current process so xlings exits and the child shell takes over.
> `exit` from that shell returns directly to the parent shell with the original env intact.

新的 `platform.cppm:280` 是 `fork()` + `execl` + `waitpid`。改动动机（统一 `--cmd` 退出码）
合理，但它**顺带改掉了交互路径**（`cmd` 为空时）：

- xlings 在整个交互会话期间常驻为父进程
- 子 shell 未被放入独立进程组、未做 `tcsetpgrp`。Ctrl-C 发给前台进程组时 xlings 同样收到
  SIGINT（默认处置=终止），可能在子 shell 仍占着 tty 的情况下退出 → shell 变孤儿、终端
  出现两个读者
- Ctrl-Z / 作业控制 / shell 退出后的终端归属恢复，从"内核保证"降级为"依赖 bash 自己抢 pgrp"的竞态

本 PR 新增的全部测试（`smoke.sh`、`subos_cmd_contract_test.sh`、`aarch64_compat_contract_test.sh`）
**用的都是 `--cmd`**，交互路径零覆盖。这是 A4 的典型形状。

#### 方案

把"平台差异"限制在**唯一一条真实差异**上（Windows 没有 exec），而不是为了统一把 POSIX 降级：

```cpp
// src/platform.cppm
// 不返回（成功时）。POSIX 专用。调用方必须把它当作进程终点。
export [[noreturn]] void exec_replace_interactive_shell();

// fork/CreateProcess + wait，返回子进程退出码（信号 → 128+n）。全平台。
export int run_shell_command(std::string_view command);
```

派发规则写成一张表，而不是散在两个文件里的 `#if`：

| 场景 | POSIX | Windows |
|---|---|---|
| `subos use <n>`（交互） | `exec_replace_interactive_shell()` | `run_shell_command("")` + wait（无 exec 语义） |
| `subos use <n> --cmd <c>` | `run_shell_command(c)` | `run_shell_command(c)` |
| `subos use <n> --sandbox`（Linux） | 已经是 `execvp(bwrap)`，不变 | n/a |
| `subos use <n> --sandbox`（macOS/Windows） | 同上两行 | 同上 |

同时修掉两条搭车问题：

- **shell 来源统一**：`XLINGS_SHELL` > `$SHELL`（POSIX）/ `pwsh > powershell > cmd`（Windows），
  两平台同一优先级链。当前 Windows 认 `XLINGS_SHELL`、POSIX 只认 `SHELL`。
- **macOS sandbox 的 `payload["shell"]` 与实际执行必须是同一个值**：现在 `sandbox.cppm`
  算出 `$SHELL`（缺省 `/bin/zsh`）发进事件，`run_shell` 内部重新读 `$SHELL`（缺省 `/bin/sh`），
  `SHELL` 未设时事件在说谎。改为解析一次、既发事件也传给执行函数。

`shell_command_argv` 当前在 POSIX 上根本没被 `run_shell` 调用、Windows 的 cmd 分支也绕开它，
而它有专门单测 —— 让 `run_shell_command` 在所有分支上**都**经过它，单测才有意义。

#### 验收判据

- 新增 e2e（Linux/macOS）：`script`/`expect` 驱动一次真正的交互 `subos use`，断言
  (a) 子 shell 内 `ps -o pid,ppid,pgid` 显示它是前台进程组 leader 或 xlings 已退出；
  (b) 发送 SIGINT 后 shell 仍在、终端未出现双读者；(c) `exit` 后回到原始 shell 且退出码正确。
- 单测：`shell_command_argv` 的每个分支都被 `run_shell_command` 实际使用（可用一个可注入的
  spawn 回调做断言）。

---

## 3. P1 批次 —— CLI 事实源

### 3.1 P1-1 —— 单向化 + 双向执行式对账

#### 现场：今天就存在一处真漂移

`spec.cppm:176` 把 optional-value 的取值写死成白名单：

```cpp
(optionName == "--shell" && (value == "sh" || value == "bash"
    || value == "zsh" || value == "fish" || value == "pwsh"));
```

真实解析器 `subos.cppm:686` 接受 **`pwsh | powershell | ps1 | ps`**，且 `--shell` 后任意
非 `-` token 都收。于是：

```
$ xlings subos use dev --shell powershell
surplus positional argument for `xlings use`: powershell     ← 且指向了顶层的 xlings use
```

**恰好是 Windows 相关形态，在一个叫"multiplatform contracts"的 PR 里，被三层"防漂移"测试
全部漏掉。**

#### 方案

**终态（推荐）—— 单向派生**：`CommandSpec` 生成 parser。`subos.cppm` / `xself.cppm` 的手写
argv 循环替换为由 spec 驱动的通用解析器，产出 `ParsedArgs`；手写代码只保留"取值 → 语义"
（如 `--ttl` 必须是整数、`--gpu` 需要 `--sandbox`）这类**语义校验**，不再自己识别 flag。
这样 drift 在结构上不可能存在。

**过渡态（本 PR 可接受）—— 双向执行式对账**，两条都必须有：

```python
# tests/scripts/test_cli_spec_parity.py
# 正向：spec 声明的每个 (path, option) 必须被真实解析器接受
for path, option in walk(spec):
    argv = [XLINGS, *path, option, safe_value(option), *dummy_required(path)]
    out = run(argv, env=isolated_home())          # 只看诊断，不看业务结果
    assert "unknown option" not in out
    assert "surplus positional" not in out
    assert "unknown subcommand" not in out

# 反向：解析器源码里出现的每个字面量 flag 必须在 spec 里存在
for flag in scan_literals("src/core/subos.cppm", "src/core/xself.cppm", "src/cli.cppm"):
    assert spec_contains(flag), f"parser accepts undocumented flag: {flag}"
```

`safe_value(option)` 对 `--shell` 应当遍历**解析器实际支持的全集**，而不是 spec 的白名单
—— 否则又变成读副本。

**验收判据（关键）**：这条 driver **必须在合入前于 `--shell powershell` 上失败一次**。
一条新加的对账测试如果第一次跑就是绿的，说明它没在对账。

#### 顺带必修

- **取值白名单删除**。optional-value 的消费规则改为"下一个 token 非 `-` 开头且当前命令的
  位置参数已满 → 消费"，或干脆要求 `--shell=<kind>` 形式。硬编码取值集会让任何新 shell /
  新 sandbox backend 静默变成"多余位置参数"。
- **错误信息带完整路径**。`spec.cppm:152/194` 用的是 `command.name`（叶子名），
  于是 `xlings subos use` 报成 `xlings use`、`xlings self doctor` 报成 `xlings doctor`
  —— 后者根本不存在。递归时把 path 拼上即可，零成本。
- **`agent` 恢复早派发**。`cli.cppm:891` 把 `if (cmd == "agent")` 移到了 help 拦截之后，
  删掉的注释原文是 *"agent — dispatched early so it handles its own -h as plain text"*。
  现在 `xlings agent -h` 渲染 TUI 盒子且**不再列出任何技能**（`agent/agent.cppm:68` 的
  `print_overview` 被绕过），与本 PR 第 5 条目标自相矛盾。
- **`agent_reference()` 补全 root 全局选项**。它的 `if (command.name != "xlings")` 跳过了
  根节点，于是生成的 agent 命令参考里**没有 `--agent`、没有 `--yes`、没有 `-v/-q`** ——
  而被它替换掉的手写文本每个示例都带着它们。skill 的 RULES 段仍在要求"ALWAYS add --yes/--agent"，
  参考段却不再展示它们。加一个"全局选项"小节。

### 3.2 P1-2 —— 全局选项归属

`spec.cppm` root 声明了 `{"-y, --yes", "Skip confirmation prompts"}`，`xlings --help` 照实显示。
但 `cli.cppm` 的 fargv 剥离只处理 `-v/-q/--verbose/--quiet/--agent`，**`-y` 被保留**；
而 `validate_manual_argv` 只查 `command.options`、**不继承 root 全局选项**。

```
$ xlings subos list -y
unknown option for `xlings list`: -y        → exit 2      （改动前：静默忽略）
```

agent skill 的第 1 条规则就是 "ALWAYS add --yes"。照做的 agent 现在会在 `xlings subos list` 上翻车。

**方案**：把全局选项收敛成一个显式对象，在 argv 剥离阶段一并处理：

```cpp
struct GlobalOpts { bool yes, verbose, quiet, agent; };
// 剥离 -y/--yes 进 GlobalOpts::yes；subos/self/xim 从它读，不再各自扫 argv
```

`validate_manual_argv` 相应地把 `root().options` 并入 allowed 集合。
`xself.cppm` 的 `reject_surplus` 也从 GlobalOpts 之后的残余 argv 上判断，
这样 `xlings self clean -y` 不再误伤。

**验收**：§3.1 的正向 driver 覆盖 `(每个子命令) × (每个 root 全局选项)` 的笛卡尔积。

---

## 4. P1 批次 —— 平台边界

### 4.1 P1-3 —— `platform::Target` 值对象 + 能力注册表

#### 方案

```cpp
// src/platform/target.cppm
export struct Target {
    std::string os;    // linux | macosx | windows
    std::string arch;  // x86_64 | aarch64 | x86

    // 本进程的 ABI（编译期常量）。决定"该装哪个包" —— 进程就是这个 ABI。
    static const Target& build();
    // 内核报告的真实硬件（uname / GetNativeSystemInfo）。可与 build() 不同：
    // Rosetta 2、WOW64、box64。决定"要不要提示用户换个原生包"。
    static const Target& host();
};
```

**当前 `compatibility.cppm::host_architecture()` 是一串 `#if`，是 build target 却叫 host。**
在 Apple Silicon 上跑 x86_64 xlings 时，这个区分决定了是"按 x86_64 装（正确，Rosetta 能跑）"
还是"报错说你在 arm64 上"。现在的代码碰巧做对了，但因为命名和语义混淆，下一次改动很容易做错。

能力从编译期常量改为运行期查询：

```cpp
// 不是 sandbox.cppm 里的 #if defined(__aarch64__)
export enum class SandboxSupport { Native, NeedsBackendInstall, Unavailable };
export SandboxSupport sandbox_support(const Target&, const std::filesystem::path& home);
```

aarch64 上"没有自动后端"应当是**运行期结论**（探测不到系统 bwrap + 索引里该 target 无资产），
而不是编译期常量 —— 否则 pkgindex 补齐 aarch64 bwrap 之后，已发布的 aarch64 客户端仍然拒绝。

散落的 `#if` 收敛清单：`inventory.cppm::platformName`、`installer.cppm::detect_platform`、
`resolver.cppm` 的 platform 参数、`sandbox.cppm` 的三分支入口 —— 全部改为读 `Target::build()`。

### 4.2 P1-4 —— 输出策略三元组解耦

当前 `palette::cursor_rewrite_allowed = tty && !tui && !opted_out_()`，
而 `opted_out_()` 含 `getenv("NO_COLOR") != nullptr`。配合 `downloader.cppm` 新的非 rewrite
分支（**空转等待到下载结束，只渲染一帧**），结果是：

```
$ NO_COLOR=1 xlings install llvm      # 交互终端
（几分钟零反馈）
（最后蹦一帧）
```

改动前即使 `canRewrite == false`，循环仍每 200ms 追加一帧。**NO_COLOR 规范管的是颜色，
不是光标控制** —— 这是语义越界（A3）。

**方案**：三个正交的判据，各自只看该看的：

```cpp
colors_enabled  = tty       && !NO_COLOR && !TERM_dumb && !plain_forced
cursor_rewrite  = tty       && !tui_mode && !plain_forced          // ← 不看 NO_COLOR
live_progress   = cursor_rewrite ? Rewrite : Append                // ← 永远有反馈
```

`Append` 模式节流到 ~5s/帧：CI 日志不刷屏，用户也不至于面对几分钟的静默。
当前"一帧都不给"的实现只对 `--agent` 的机器消费者合适，不该套给所有非 TTY 场景。

附带修掉：`opted_out_()` 用 `getenv != nullptr`，意味着 `NO_COLOR=""` 也生效，与 NO_COLOR
规范（空值不应禁用）相反；以及非 rewrite 分支那个 20ms 一跳、什么都不渲染的空转循环
（50 次/秒纯浪费），改为条件变量。

---

## 5. P2 批次

### 5.1 P2-1 —— inventory 采集方向反转

`inventory.cppm::collect_inventory` 先 `catalog.search("", platform)`（内部 `build_matches_`
已对**每个配方**跑一次 `xpkg::load_package` → 起 Lua state 执行 .lua），**然后对每个 match
再 `catalog.load_package(match)` 跑第二次**，无缓存、无按 storeName 去重。

- `cmd_list`：改动前只对已安装的 match（通常 <10）load；现在对全索引 load 两遍
- `cmd_info`：改动前基本 O(1)；现在调 `collect_inventory(catalog, true)` —— 全索引双载
  + 遍历所有 subos 快照 + 遍历整个 xpkgs 存储树，**只为回答两个布尔值**

本机基线（130 配方的小索引）：`xlings list` 162ms、`xlings info gcc` 60ms。改动后 info
至少是 list 的量级再翻倍；官方索引更大、Windows 文件系统更慢时差距放大。

**方案 —— 方向反过来**：

```
records  = from(workspace.installed ∪ workspace.active ∪ payload_markers)   // O(已装)
metadata = catalog.lookup_many(distinct_store_names(records))               // O(已装) 次 load
```

并给 `PackageCatalog` 加进程内 `load_package` 缓存（key = rawName），因为 `build_matches_`
已经载过一遍。`cmd_info` 不调 `collect_inventory`，改调 `lookup_installed(canonicalName)`。

顺带修三条正确性问题：

- **`assemble_inventory` 硬用 `Config::global_data_dir()/"xpkgs"`**，忽略 `PackageMatch`
  自带的 `storeRoot`（project 作用域索引仓落在 `project_data_dir()`）→ project 作用域安装
  的包会被判成 `degraded: payload missing`。
- **`includePayloadMetadata` 直接绑给了 `allSubos`** —— "要不要显示 payload-only 包"和
  "要不要跨 subos"是两件事，且 payload-only 记录的 `suboses` 为空。拆成两个参数。
- **`suboses` / `inCurrentSubos` 采集了但从不渲染** —— `list --all` 仍是无归属的平铺列表。
  要么渲染（推荐：加一列），要么删掉字段。

以及 `cmd_info` 的三个相近标签（`package installed` / `selected installed` / 详情区的
`installed <verList>`）：被删掉的注释恰好解释了当初为什么只在"未安装"时才打那一行 ——
为了避免第三个标签争夺同一语义。建议保留 `selected installed`，把 `package installed`
折进详情区标题。

### 5.2 P2-2 —— 发布链真实性

- **sidecar 用裸文件名**。`release.yml` 的 `prepare-candidates` 用
  `sha256sum "$file" > sidecars/$name.sha256`，`$file` 是 `artifacts/xlings-linux-x86_64/…`，
  于是发布出去的 sidecar 第二列是**构建机内部路径**。旧代码 `(cd "$dir" && sha256sum "$name")`
  产出裸名。quick_install 的正则只取前 64 位十六进制所以不受影响，但任何用户或镜像工具跑
  `sha256sum -c xlings-*.tar.gz.sha256` 都会失败。改回 `cd` 或用 `--tag` 形式，
  并在 `prepare-candidates` 里加一步 `sha256sum -c` 自检。
- **发布门禁去掉网络抖动**。`smoke.sh` 在 Linux x86_64 上走 `*)` 分支执行
  `subos use --sandbox`，触发 `auto_install_backend_` 真的联网装 bwrap。这四个 candidate
  job 现在是 `create-release` 的 `needs`，等于把索引/镜像抖动接进了发布关键路径。
  改用预置 backend（或对该步骤单独加重试 + 明确的跳过条件）。

### 5.3 P2-3 —— 文档产品化

- **README 能力矩阵必须分层**。当前写「Linux aarch64 | Supported」，但在 P0-1 修好之前
  该平台上大部分包装不上；即使修好，包生态的覆盖也不等于发布产物的覆盖。拆成两张表：

  | | 发布产物 | 包生态覆盖 | SubOS 隔离级别 |
  |---|---|---|---|
  | Linux x86_64 | ✅ | 完整 | bwrap/proot 文件系统隔离 |
  | Linux aarch64 | ✅ | 部分（见 pkgindex arch 覆盖表） | bwrap/proot（后端需手动提供） |
  | macOS 14+ arm64 | ✅ | 部分 | 仅 HOME 重定向 |
  | Windows x86_64 | ✅ | 部分 | 仅 USERPROFILE 重定向 |

  "Supported" 一个词同时承担三件事，是本 PR 文档层面最大的误导来源。
- **`docs/generated/command-reference.md` 要有再生成路径**。当前 CI 只断言"已生成的和
  checked-in 的一致"，靠人工跑命令再提交。加一个 `make docs` / CI 自动再生成 + 失败时
  打印精确的再生成命令。
- **安全声明前置**。`subos-isolation.md` / README 里"macOS/Windows 不隔离不可信代码"这条
  改得非常对，但它现在只在文档里。建议 `subos use --sandbox` 在 macOS/Windows 上**首次运行
  打印一次**（可用 `--quiet` 抑制），因为会去读文档的用户不是会误用的那批。
- **agent skill**：见 §3.1 末尾（`agent_reference()` 补全全局选项段）。

---

## 6. 测试架构：三条必须成立的性质

本 PR 新增了 6 个单测 + 9 个脚本测试，但其中 5 个是自证的。合入前应当让下面三条成立：

1. **每条对账测试必须能指出一个今天存在的分歧。**
   验收：§3.1 的 driver 在 `--shell powershell` 上失败；§2.2 的篡改用例在注释掉
   `verify_sidecar` 后失败。第一次跑就绿的对账测试，等于没有。
2. **每个语义变更有一条指名它的测试。**
   当前缺：POSIX 交互 shell 的进程模型、`NO_COLOR` 下的进度行为、`xlings agent -h` 的输出形态。
3. **grep 类断言只用于"防误删"，不得替代行为验证。**
   `test_release_candidate_gate.py` 里断言 job 存在的部分保留；断言 smoke 脚本含
   `"--sandbox"` 字面量的部分删除 —— 它只会在重构时误报，从不在退化时报警。

---

## 7. 落地顺序与验收清单

| # | PR | 内容 | 验收 |
|---|---|---|---|
| 1 | A | #471 修复 + 双平台回归、candidate 门禁、文档诚实化、`xself` 错误事件化 | 现有测试；无新风险 |
| 2 | P0-3 | 进程模型拆分 + shell 来源统一 + macOS payload 一致性 | expect 驱动的交互 e2e |
| 3 | P0-2 | quick-install 执行式覆盖（含 `XLINGS_INSTALL_BASE_URL`） | 双向检查（篡改必失败 / 去掉校验必失败） |
| 4 | P1-1 + P1-2 | CLI 双向对账 driver + 全局选项归属 + 错误路径 + agent 早派发 | driver 首跑必须红于 `--shell powershell` |
| 5 | P1-3 + P1-4 | `Target` 值对象 + 能力注册表；输出三元组解耦 | `#if` 计数下降；`NO_COLOR` 下仍有进度 |
| 6 | **P0-1 + pkgindex** | 证据分级门禁 + 索引 `archs` 扫描补齐 | aarch64 全索引 resolve 被拒清单为空 |
| 7 | P2-1/2/3 | inventory 反转 + 发布链 + 文档分层 | `info` 回到 O(1) 量级；`sha256sum -c` 可用 |

**P0-1 排在最后不是因为它不重要，而是因为它是唯一需要跨仓联动的一项** —— 前六项都不阻塞它，
而它单独合入会立即破坏现网。在 pkgindex 扫描完成之前，本 PR 的 arch 门禁部分应当从合入范围里摘出。

---

## 8. 一句话

> PR #472 把"契约"写成了文本，然后让测试去读那份文本。**把每一处契约换成一个可以被执行、
> 因而可以被证伪的事实源**，本报告里 20 余条发现会塌缩成 3 条主线的 7 个 PR；
> 其中只有一条（arch 门禁）需要跨仓协调，其余都能独立落地。
