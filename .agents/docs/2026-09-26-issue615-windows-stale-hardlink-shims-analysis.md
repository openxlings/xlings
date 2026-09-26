# #615 分析：Windows `self update` 后工具 shim 仍是旧版硬链接

- Issue: https://github.com/openxlings/xlings/issues/615 （reporter: julixian）
- 分析基线：`v2026.9.26.2`（= origin/main `2e3df6d`）
- 结论：**xlings 侧缺陷，只出现在 Windows。** 这个缺陷很早就存在，#613 修的是分发器，这些旧入口用不到新分发器，问题因此暴露出来。POSIX 不受影响。

## 1. 结论摘要

| 问题 | 回答 |
|---|---|
| 是不是 xlings 的问题 | 是。issue 里的源码分析逐条对得上 v2026.9.26.2 源码，没有错误 |
| 影响范围 | 所有执行过 `self update`（或 `install/use` 切换 xlings 版本）的 Windows home。`subos\*\bin\` 下每个 shim 都停在它**被创建那一刻**的 xlings 版本，包括 PATH 上的 `xlings.exe` 本身 |
| 为什么没人报错 | `self update` 退出码是 0；`xlings info` 读的是元数据；doctor 检查的是 `home\bin\xlings.exe`，不是 PATH 上真正被执行的那个文件；这些旧 shim 在 doctor 里只算 `file not ours`（Notice 级别），不影响退出码。属于"没做成"和"做成了"输出完全一样的那类问题 |
| 现有命令能不能修 | **都不能。** 重装 mcpp、`use`、`self doctor --fix`、`self init` 都修不了工具 shim。`self init` 只修 `xlings.exe` 这一个内置名字 |
| 临时办法 | 用新的 entry binary 重建旧 shim 的硬链接（第 4 节有脚本）。在修复版本发布前，**每次 `self update` 之后都要再跑一次** |

## 2. 机制（已对照源码确认）

POSIX 的 shim 是指向 `home/bin/xlings` **路径**的符号链接，entry 被替换后 shim 自动指向新文件。Windows 的 shim 是指向 entry **文件对象**的硬链接，四步串起来就会出问题：

1. **替换 entry 会换掉文件对象。**
   `modules/platform/src/platform/windows.cpp:262` `atomic_replace_executable`：先把 `dst` 改名为 `.xlings.old`，再 `copy_file(src, dst)`。`home\bin\xlings.exe` 因此变成一个新文件对象，原来的 40 个硬链接还挂在旧文件对象上（`.xlings.old` 之后被删除或排队到重启时删除，但硬链接会让旧内容一直存在）。

2. **"是不是我们的 shim"按文件身份判断。**
   `src/core/xvm/shim_table.cpp:29` `is_our_shim_` 用的是 `fs::equivalent(path, entryBinary)`。替换之后，旧硬链接和新 entry 不再 equivalent，也不是 symlink，于是返回 false。

3. **判成 foreign 之后，这些文件再也没人管。**
   `scan_actual` 把它们放进 `foreign`。`plan_table`（`shim_table.cpp:144`）按"用户自己放的真实程序不能覆盖"的规则，把 foreign 名字从 `toAdd` 里删掉。结果是 `sync_shim_tables()` 永远不会重建它们，`toRemove` 也只处理 `ours`。

4. **切换顺序也不对。**
   `src/core/xvm/commands.cpp:844`：`cmd_use` 先调用 `sync_shim_tables()`（这时旧硬链接还算 ours，而且 desired 里已经有这些名字，所以什么都不做），**然后**才调用 `entry_binary::replace_with` 替换 entry。替换之后没有任何代码回头处理这些硬链接。installer 那边的 self-replace（`installer.cpp:~2200`）顺序反过来，先替换，再在 `installer.cpp:2401` 调用 sync，但结果一样：sync 执行时这些旧硬链接已经被判成 foreign，所以被跳过。**顺序对不对都没用，问题出在第 2、3 步。**

由此解释 reporter 看到的每一个现象：

- `self update` 之后 `xlings --version` 还是 9.20.1：PATH 命中的是 `subos\current\bin\xlings.exe`，它也是旧硬链接，同样被判成 foreign。`self update` 不会调用 `ensure_home_layout`（`update.cpp` 只 exec `update` / `install --use` / `use`），所以没人重写它。
- 用新版执行 `self init` 后只修好了 xlings：`ensure_subos_shims` 只对 `SHIM_NAMES_BASE = {"xlings"}` 调用 `create_shim`（先 displace 再建硬链接，会强制覆盖）；紧接着的 `sync_shim_tables` 对其他 40 个 foreign 文件同样不处理。
- doctor 报 `41 file not ours`：40 个工具加上 1 个别的文件，都走 `ForeignBinEntry`，级别是 Notice，文案是 "is not an xlings shim — left alone"。**这句话本身不对**：它们其实是 xlings shim，只是旧版本的。
- 在 Unicode 目录下 `mcpp --version` 以 `0xC0000409` 退出：mcpp.exe 实际执行的是 9.20.1 的分发器，没有 #613 加进来的 UTF-8 manifest 和启动异常处理。

附带影响（从代码推出，没有在 Windows 上实测）：

- 在 Windows 上 `remove` 一个包时，它的 shim 如果已经被判成 foreign，就不会进 `toRemove`，残留下来。
- 同一 home 里的 shim 可以分属不同代的分发器（每个 shim 停在自己创建时的版本）。以后任何修在分发器里的问题（例如 `${XLINGS_DYNAMIC_SUBOS_DIR}` 这一类）在 Windows 上都会以同样方式无法生效。
- `sync_shim_tables` 只处理当前作用域和 global 两个 subos，其他 subos 的 bin 就算判断逻辑修好了，也要单独过一遍。

需要说明的一点：reporter 更新前这 40 个入口都是同一个 9.20.1 文件。可能是这些工具在升到 9.20.1 之后才装的，也可能是全新安装的 9.20.1。无论哪种，结论都一样；具体是哪种没有核实。

## 3. 与相关 issue 的关系

- #473：Windows 自更新时文件被占用。已经用 `displace_locked_file` 处理了，这次的问题跟它不是同一个。
- #613 / mcpp-community/mcpp#693：修复本身是正确的，但只有新建的 shim 才能用上。#615 是这个修复"送不到用户手上"的原因。
- 这类问题在 AGENTS.md 里已经记过一次（"swapping the binary under test without swapping the dispatcher is not a control"）。这次是产品自己在 Windows 上换了 binary、没换 dispatcher。

## 4. 给用户的临时解决方法

### 不能用的办法（避免用户白试）

`xlings install/remove/use`、`xlings self doctor --fix`、`xlings self init` 都**修不了**工具 shim，原因见第 2 节第 3 步。其中 `self init` 只能修好 `xlings.exe` 这一个。

### 能用的办法：把旧 shim 重新硬链接到当前的 entry binary

这就是 reporter 手工做过、并且验证有效的操作，下面把它写成可以安全重复执行的脚本。判断"这是旧版 xlings shim"用的是**内容哈希**，不是文件名：只处理字节内容和"本 home 装过的某个 xlings 构建"完全一致的文件，所以用户自己放进 bin 的真实程序不会被动到。默认只预览，加 `-Apply` 才会改文件。

```powershell
# xlings #615 workaround: relink stale Windows shims to the current entry binary.
# Usage:  .\relink-xlings-shims.ps1          (dry run: list what would change)
#         .\relink-xlings-shims.ps1 -Apply   (do it)
param([switch]$Apply)
$ErrorActionPreference = 'Stop'

$xhome = if ($env:XLINGS_HOME) { $env:XLINGS_HOME } else { Join-Path $env:USERPROFILE '.xlings' }
$entry = Join-Path $xhome 'bin\xlings.exe'
if (-not (Test-Path $entry)) { throw "entry binary not found: $entry" }
Write-Host "entry: $entry -> $(& $entry --version)"
$entryHash = (Get-FileHash $entry -Algorithm SHA256).Hash

# Every xlings build this home can identify: installed payloads, plus each
# subos's own xlings.exe (by construction always an xlings build).
$known = @{}
function Add-Known($p) { if (Test-Path $p) { $known[(Get-FileHash $p -Algorithm SHA256).Hash] = $p } }
Get-ChildItem (Join-Path $xhome 'data\xpkgs') -Directory -Filter '*-x-xlings' -ErrorAction SilentlyContinue |
  ForEach-Object { Get-ChildItem $_.FullName -Directory } |
  ForEach-Object { Add-Known (Join-Path $_.FullName 'bin\xlings.exe') }

# Real subos directories only; `subos\current` is a junction to one of them.
$subosDirs = Get-ChildItem (Join-Path $xhome 'subos') -Directory |
  Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) }
$subosDirs | ForEach-Object { Add-Known (Join-Path $_.FullName 'bin\xlings.exe') }

$stale = 0; $skipped = 0
foreach ($s in $subosDirs) {
  $bin = Join-Path $s.FullName 'bin'
  if (-not (Test-Path $bin)) { continue }
  foreach ($f in Get-ChildItem $bin -File -Filter '*.exe' | Where-Object { $_.Name -notlike '*.xlings.old*' }) {
    $h = (Get-FileHash $f.FullName -Algorithm SHA256).Hash
    if ($h -eq $entryHash) { continue }                       # already current
    if (-not $known.ContainsKey($h)) { $skipped++; Write-Host "  keep (not an xlings build): $($f.FullName)"; continue }
    $stale++
    Write-Host "  stale: $($f.FullName)"
    if (-not $Apply) { continue }
    # Rename aside first: this works even while the old shim is running.
    # xlings ignores `*.xlings.old*` names.
    $aside = "$($f.FullName).xlings.old$(Get-Random)"
    Move-Item -LiteralPath $f.FullName -Destination $aside
    try {
      try   { New-Item -ItemType HardLink -Path $f.FullName -Target $entry | Out-Null }
      catch { Copy-Item -LiteralPath $entry -Destination $f.FullName }   # e.g. different volume
    } catch {
      Move-Item -LiteralPath $aside -Destination $f.FullName              # restore
      throw
    }
    Remove-Item -LiteralPath $aside -Force -ErrorAction SilentlyContinue  # may stay if still running
  }
}
Write-Host ("{0} stale shim(s){1}; {2} non-xlings file(s) left alone" -f `
  $stale, $(if ($Apply) { ' relinked' } else { ' (dry run, pass -Apply)' }), $skipped)
```

给用户的操作步骤：

1. 先确认新 entry 已经到位：`& "$env:USERPROFILE\.xlings\bin\xlings.exe" --version`，应显示最新版本。
2. 关掉正在运行、而且经由 xlings shim 启动的工具（IDE 里的 clangd、正在跑的 ninja 等）。不关也能执行，但被占用的旧文件会留一个 `.xlings.old*`，重启后才会被删掉。
3. 先 `.\relink-xlings-shims.ps1` 预览，确认列出的都是工具入口，再执行 `.\relink-xlings-shims.ps1 -Apply`。
4. 验证：`xlings --version`、`mcpp --version` 应显示最新版本；在含中文或 emoji 的目录里执行 `mcpp --version`，应当有输出、退出码为 0。
5. **修复版本发布前，每次 `self update` 之后都要重跑一次。**

注意：上面这个脚本**没有在 Windows 上实际运行过**（本机是 Linux，也没有 pwsh）。回复用户之前，建议在 Windows CI 或真机上跑一遍 dry-run 和 `-Apply`。reporter 手工做过同样的操作（逐个重建 40 个硬链接），这一点已经被验证有效。

如果用户只想马上修好 PATH 上的 `xlings.exe`：运行 `& "$env:USERPROFILE\.xlings\bin\xlings.exe" self init` 就够了，但 mcpp、clang 等工具 shim 仍然需要上面的脚本。

## 5. 修复方向（供后续实现参考，本次未改代码）

核心问题：在 Windows 上，"这个文件是不是我们的 shim"不能用"是不是同一个文件对象"来回答，因为每次替换 entry 都会刻意换掉这个文件对象。

1. **把"旧版 xlings shim"识别出来，作为单独一类处理，不要混进 foreign。** 判断要和版本无关，也要能和用户放进来的真实程序区分开。可选办法：在 binary 里嵌入一个固定标记（例如 PE 资源里的一个字符串）；老 binary 没有这个标记，就退回到和本 home 里 `*-x-xlings/*/bin/xlings.exe` 比哈希。`scan_actual` 增加一类 `stale` 分组，`plan_table` 对它的处理是替换（relink），不是跳过。doctor 把它报成 `ShimTableDrift`，而且在 Windows 上级别应为 Error，因为用户实际执行的分发器是旧的。
2. **在替换 entry 的地方把 shim 一起搬过去。** `entry_binary::replace_with` 目前是唯一的写者。在它成功替换之后，对**所有**真实 subos 的 bin（不止当前作用域和 global）做一遍第 1 条的 relink。`cmd_use` 和 installer 两条路径都经过 `replace_with`，放在这里只需要写一次。
3. **只有第 2 条而没有第 1 条不够。** 已经坏掉的 home 需要一条回来的路，最好让新版 `self update` 顺带修好（和 #582 的做法一致：修掉原因的那次升级，也负责修好它造成的状态）。
4. **回归测试（Windows e2e）：** 用旧 release 初始化 home，装一个带 program 的包，再升级到被测构建。然后断言：每个 subos 的 `bin\<tool>.exe` 和 `bin\xlings.exe` 的内容（或文件身份）都等于新 entry；`self doctor` 在升级前后都能报出或确认这一点；在非 ACP 目录里执行一次真实工具 shim。注意 AGENTS.md 的提醒：只换被测 binary、不换分发器的测试不能算对照组，这个测试本身就是在验证"分发器有没有换"。

另外，§2 提到的 doctor 文案（"is not an xlings shim — left alone"）在 Windows 升级后的 home 上是错的。修第 1 条时顺手改掉。

## 6. 为什么以前没出现 / 什么时候引入 / CI 为什么没拦住

### 时间线（按 git 历史核对）

| 版本 | 提交 | 变化 | Windows 上的后果 |
|---|---|---|---|
| v0.4.7（2026-04-30） | `8164874` #236 | 首次引入 self-replace，Windows 上的 `atomic_replace_executable` 是"先改名，再复制" | **潜在缺陷从这里开始**：每替换一次 entry，所有硬链接 shim 都和 entry 断开。但当时 `cmd_use` 和 installer 对**每个被切换的 target 都会调用 `create_shim`**（displace 后重建硬链接，强制覆盖）。`self update` 最后一步是 `use xlings latest`，`to_switch` 里总是包含 xlings（`plan.members = selection->members`，不管是否已经 active），所以 PATH 上的 `xlings.exe` 每次都会重建，内容是新的。工具 shim 虽然也旧了，但用户下次 `install` / `use` 这个工具时就会被刷新 |
| 2026.9.3.1（2026-09-03） | `ba51901` #580 | shim 改成派生路由表，唯一写者是 `sync_shim_tables`。所有权用 `fs::equivalent` 判断，规则是"foreign 永不覆盖" | **可见的回归从这里开始。**（1）`self update` 不再重建 `subos\*\bin\xlings.exe`：它在第一次替换之后就成了 foreign，所以升级后 PATH 上的 `xlings --version` 仍是旧版本；（2）旧工具 shim 变成**永久**修不了，`install` / `use` / `doctor --fix` 都跳过 foreign |
| 2026.9.26.2 | `2e3df6d` #613 | 修复放在分发器自己的 PE manifest 和启动代码里 | 分发器过旧**第一次带来致命后果**：旧版 mcpp.exe 分发器在非 ACP 目录下以 0xC0000409 退出 |

需要注意，决定行为的是**升级前那个旧客户端**。`self update` 里的 `xlings install ... --use` 和 `xlings use xlings latest` 都通过 PATH 执行，也就是 `subos\current\bin\xlings.exe`，也就是旧版本。所以从 ≥ 2026.9.3.1 的版本发起的每一次 Windows 升级都会遇到这个问题。reporter 是从 9.20.1 升级，正好在这个范围里。

### 为什么"以前没有"

- **9.3.1 之前**：PATH 上的 `xlings.exe` 每次升级都会被 `use xlings latest` 重建。工具 shim 虽然旧了，但下次 `install` / `use` 这个工具时会被刷新。而且旧分发器在绝大多数情况下都能正常分发，所以看不出问题。
- **9.3.1 到 9.26.1**：`xlings --version` 在升级后显示旧版本，这个症状其实已经存在，只是没人报告（维护者的日常环境是 Linux，Linux 用符号链接，不会出现这个问题；这一点是推断）。工具 shim 过旧照样看不出来。
- **9.26.2**：#613 的修复只在分发器本身生效，"分发器是哪个版本"第一次决定了工具能不能启动。问题就此暴露。

### CI 为什么没拦住

1. **Linux/macOS 上物理上不可能出现。** 那里的 shim 是指向路径的符号链接，entry 被替换后自动生效。唯一测试 self-replace 的 E2E-15（`xlings_self_replace_test.sh`）只在 bash 里跑，断言的也只有 `home/bin/xlings` 的内容，从不检查 subos 下的 shim。
2. **Windows CI 里没有"先有工具 shim，再替换 entry"这个顺序。** `tests/candidate-install/smoke.ps1` 确实测了"覆盖正在运行的 binary"（#473），但：
   - 替换发生时 home 里还没有任何工具 shim，`candidate-helper` 是在替换**之后**才安装的，它的 shim 自然链接到新 entry；
   - 所有断言都直接调用 `$installed`，也就是 `home\bin\xlings.exe`，**从来不经过 `subos\current\bin\xlings.exe`（PATH 实际命中的文件）**。这和用户报告里"entry 是新的、PATH 上是旧的"是同一个盲区；
   - 它走的是 `self install`，会执行 `ensure_home_layout`，强制重建 `xlings.exe` shim，而用户真正走的 `self update` 不会。
3. **fresh-install（Windows）按设计不测 xlings 升级。** 它测的是"新用户拿到的 latest"。里面的 MCPP_OLD→NEW 是普通包的升级，不会替换 entry。
4. **doctor 把这种状态归为 Notice（`ForeignBinEntry`，文案是 "not an xlings shim — left alone"）。** 就算某个测试进入了这个状态，`self doctor` 也会返回 0。
5. **发布后的验证在 Linux 上做。** 2026.9.26.1 的记录是"`xlings self update`：entry binary `2026.9.20.1 -> 2026.9.26.1`"。这是在 Linux 真机上核对的 entry 版本，在 Linux 上它和 PATH 上的版本一致，所以这个检查在 Windows 上说明不了任何问题。

一句话：所有检查都在问"entry binary 是不是新的"，没有一个在问"用户真正执行的那个文件是不是新的"。在 Linux 上这两个问题答案相同，在 Windows 上不同。

### 补测试时要注意的点

- 必须按"旧客户端建 home → 装一个带 program 的包 → 用**旧客户端**执行 `self update`（或 `use xlings <new>`）"这个顺序来。只有这样才能复现"执行 sync 的是旧代码"的真实情况。
- 断言要经过 `subos\current\bin\xlings.exe` 和工具 shim 本身：比较文件哈希或执行 `--version`，不要只看 entry。
- 还要加一条断言：doctor 在这种状态下不能返回 0。
