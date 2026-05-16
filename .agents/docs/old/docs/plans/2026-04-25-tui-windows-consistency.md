# 分析：xlings TUI 在 Linux/macOS vs Windows 上的一致性

**问题：** Windows 上经常出现乱码 / 部分字符显示不出。
**目标：** 给出现状、根因、业界常见解法、xlings 该走哪条路的判断。

## 一、xlings 现状

### TUI 渲染路径（`src/ui/`、`src/runtime/event_stream.cppm`）
- 全部走 ftxui 6.1.9：每个 ui::print_* 函数都是 `Screen::Create() → Render() → screen.Print()`，最后 `std::cout.flush()`。
- 没有原生 `println` 的中文输出，所有用户可见文本（包括 i18n/zh.json 里的中文、help、安装计划、版本面板）都过 ftxui。

### Windows 控制台初始化（`src/platform/windows.cppm:186-198`）
xlings 已经在 `main.cpp` 入口调用 `platform::init_console_output()`，里面做了：

```cpp
::SetConsoleOutputCP(CP_UTF8);          // 65001
::SetConsoleCP(CP_UTF8);
mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
::SetConsoleMode(hOut, mode);
```

→ 三件事都做对了。

### 已经做的平台分支（`src/ui/theme.cppm:36-58`）

```cpp
#ifdef _WIN32
    inline constexpr auto pending = "o";
    inline constexpr auto done    = "+";
    inline constexpr auto failed  = "x";
#else
    inline constexpr auto pending = "\xe2\x97\x8b";   // ○
    inline constexpr auto done    = "\xe2\x9c\x93";   // ✓
    inline constexpr auto failed  = "\xe2\x9c\x97";   // ✗
    // ⟐ (U+27F0)、↓ (U+2193) 等
#endif
```

→ Linux/macOS 看到一种视觉效果，Windows 看到另一种；**这本身就是一致性问题**。

### Windows CI（`.github/workflows/xlings-ci-windows.yml`）
- pwsh 跑 `xlings -h` / `config` / `search` 等命令
- 只断言英文关键字（"FAIL"），不检查中文/Unicode 字节
- 没有给 PowerShell 设置 `$OutputEncoding` / `[Console]::OutputEncoding`

## 二、Windows TUI 乱码的常见根因（业界综合）

按出现频率排：

### R1. 字体缺 CJK 字形
PowerShell / conhost 默认 **Consolas**，Windows Terminal 默认 **Cascadia Mono / Code** —— 这三个都**不带 CJK 字形**。即使输出是正确的 UTF-8 字节，字体没字形就显示 `□` / `?` / 透明方块。

修复：用户层面装 Cascadia Code SC / TC / JP（带 CJK 的变体）或 MS Gothic 作为 fallback。**这不是 xlings 程序能修的**，只能在文档里告诉用户。

### R2. PowerShell 编码不对齐 chcp
`SetConsoleOutputCP(CP_UTF8)` 只改 **console handle 的输出编码**：当 stdout 被重定向 / 管道走时（`xlings ... > out.txt`、`xlings ... | grep`），PowerShell 用的是 `[Console]::OutputEncoding` 和 `$OutputEncoding`，这俩默认不是 UTF-8。

修复：要么用户在 PowerShell profile 里设 `$OutputEncoding = [Text.UTF8Encoding]::new()`，要么程序自己把 BOM 写进 stdout（不可取，会破坏管道）。**xlings 已经做对了 console 这一层**，剩下是用户/CI 配置问题。

### R3. ftxui 6.0.x 在 Windows 有过严重渲染回归（[FTXUI #1020](https://github.com/ArthurSonzogni/FTXUI/issues/1020)）
6.0.0/6.0.1 在 Windows 上鼠标一动就乱码 box-drawing。**6.1.x 已修**。xlings 现在锁的是 6.1.9，理论上不受影响 —— 但要在 CI 里验证一下。

### R4. ENABLE_VIRTUAL_TERMINAL_PROCESSING 不跨进程
该标志只对当前 console handle 生效。子进程（spawn 出来的 git / curl / 包里的 hook）不继承。Windows Terminal 默认会启用，conhost (legacy) 不会。

xlings 自己启用了 → 主进程 OK。但子进程输出的颜色 / 进度条若依赖 VT 序列，在 conhost 下会显示成 `^[[31m` 字面量。

### R5. 平台 ifdef 让 Linux/macOS 和 Windows 视觉不一致
xlings 现在的 theme.cppm 就是这样：Windows 见 `o + x`，Linux 见 `○ ✓ ✗`。这是程序员手动把"Windows 兼容"做成"Windows 体验降级"。
**Cascadia Mono 实际是支持 ✓ ✗ ○ 这些 BMP 区符号的**（不是 CJK），所以这个 ifdef 是过度保守。

### R6. CJK 双宽字符的 width 计算偏移
ftxui 用 unicode-13 宽度表，**但 Windows 的某些字体会把宽度 2 的 CJK 渲染成宽度 1**（取决于字体的 `panose-1` 标志），导致 box-drawing 边线错位。这是字体级问题，程序层无能为力，只能用窄字符布局。

## 三、推荐的修复方案（按 ROI 排序）

### S1. 摘掉 theme.cppm 的 ifdef，统一用一组"全平台安全"的字符（小，立即收益）
| 含义 | 现 Win | 现 Linux | 推荐统一 | 是否需 CJK 字体 |
|---|---|---|---|---|
| pending | `o` | `○ U+25CB` | `○` | 否 |
| done | `+` | `✓ U+2713` | `✓` | 否 |
| failed | `x` | `✗ U+2717` | `✗` | 否 |
| downloading | `↓` | `↓ U+2193` | `↓` | 否 |
| extracting | `⟐ U+27F0` | `⟐ U+27F0` | **替换为 `~` 或 `…`** | ⟐ 在很多终端字体里不存在 |

→ 删 `#ifdef _WIN32` 分支，全部用 BMP 区常见符号 + 替掉 `⟐`。代码量级 ~20 行。改动后 Linux/macOS/Windows 视觉完全一致。

### S2. 给 Windows CI 加输出验证（小，立即收益）
在 `xlings-ci-windows.yml` 的 E2E step 里加：

```powershell
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$out = & "$bin" use 2>&1 | Out-String
# 断言输出包含特定 Unicode 字符（如 ✓），不是 mojibake
if (-not ($out -match '✓')) { throw 'UTF-8 output broken on Windows' }
```

→ 把"乱码"变成可被 CI 抓到的回归。代码量级 ~20 行。

### S3. 文档化 Windows 用户的字体推荐（零成本）
在 `docs/quick-install.md` 或新文档说明：
- 推荐 Windows Terminal（默认开 VT，比 conhost / pwsh 旧版好）
- 推荐字体 `Cascadia Code SC` / `Microsoft YaHei Mono` / `JetBrains Mono CN`，覆盖 CJK
- 如果一定要用 conhost / Win10 1809 之前：`chcp 65001` 之前在用户 profile 里加 `$OutputEncoding`

→ 不能修复程序，但帮用户绕开 R1。

### S4.（可选）增加 `--ascii` / `XLINGS_NO_UNICODE=1` 降级模式
对真的没法装 CJK 字体的环境（哑终端、远程串口、屏幕阅读器），让用户**显式**走 ASCII 渲染。比当前编译期 `#ifdef _WIN32` 灵活。代码量级 ~50 行（theme.cppm 换查表）。

### S5.（暂缓）字体内嵌 / fallback 机制
xlings 不该管字体 —— 这是终端模拟器和 OS 的事。任何"嵌入 CJK 字体"或"自动切字体"的尝试都超出包管理器边界。**不做**。

## 四、决策建议

| 项 | 做不做 | 优先级 | 工作量 |
|---|---|---|---|
| S1 摘 theme.cppm ifdef + 替 ⟐ | 做 | 高 | ~30 行 |
| S2 Windows CI UTF-8 输出回归 | 做 | 高 | ~20 行 yaml + ps1 |
| S3 文档化 Windows 字体推荐 | 做 | 中 | 一段 markdown |
| S4 `--ascii` 降级模式 | 等用户提 | 低 | ~50 行 |
| S5 字体相关 | 不做 | — | — |
| 升级 ftxui 到 ≥ 6.2.x | 检查后定 | 中 | 取决于 API 是否变 |

## 五、不一致的根本观察

> "Windows 上 TUI 体验差"在 xlings 这个 case 里，**程序侧能做的就是 S1+S2+S3**。剩下的（CJK 字形是否渲染、PowerShell 是否被重定向、用户终端版本是否过旧）属于环境配置范畴，包管理器无权也不该接管。

把当前 theme.cppm 的"Windows 给特殊待遇"那段去掉，统一用 BMP 区安全字符 + 把 `⟐` 这种偏门字符换掉，**就是 80% 收益**。剩下 20% 走文档 + CI 回归。

## 参考

- [FTXUI #1020 — 6.0.1 broken on Windows](https://github.com/ArthurSonzogni/FTXUI/issues/1020)
- [FTXUI #2 — CJK input crash](https://github.com/ArthurSonzogni/FTXUI/issues/2)
- [FTXUI #27 — Windows can not be used normally](https://github.com/ArthurSonzogni/FTXUI/issues/27)
- [Microsoft — PowerShell Console characters garbled for CJK](https://learn.microsoft.com/en-us/troubleshoot/windows-server/system-management-components/powershell-console-characters-garbled-for-cjk-languages)
- [winget-cli #1832 — CJK font not showing in PowerShell](https://github.com/microsoft/winget-cli/issues/1832)
- [Windows Command Line — Unicode and UTF-8 Output](https://devblogs.microsoft.com/commandline/windows-command-line-unicode-and-utf-8-output-text-buffer/)
- [Cascadia Code CJK 兼容性讨论](https://github.com/microsoft/cascadia-code/issues/55)
- [PowerShell #7233 — Make console UTF-8 by default](https://github.com/PowerShell/PowerShell/issues/7233)
