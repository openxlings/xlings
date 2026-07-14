# OSC-11 背景色查询：从「50ms 单次 read」改为「有界帧读 + DSR/CPR 栅栏」— 修复终端回复泄漏

**日期**: 2026-07-15
**类型**: 方案 / 修复 (design + bugfix)
**状态**: Implemented
**范围**: 修复 [#368](https://github.com/openxlings/xlings/issues/368)（Tabby 下 OSC-11 回复泄漏进 zsh 输入缓冲区）。仅改客户端 `xlings` 二进制，无跨仓改动。
**关联代码**:
- `src/platform/unix.cppm:103-160`（`query_terminal_is_light()`，Linux/macOS 共用）
- `src/platform.cppm:37`（`export using platform_impl::query_terminal_is_light`）
- `src/ui/theme.cppm:63-70`（`detect_()`，检测顺序：env → OSC-11 → COLORFGBG → Dark）
- `src/core/config.cppm:16`（`VERSION` 0.4.65 → 0.4.66）
- `tests/unit/test_terminal_query.cpp`（新增回归测试）

---

## 0. TL;DR

`xlings` 无参运行时会用 OSC-11 (`ESC ] 11 ; ? ESC \`) 询问终端背景色以选深/浅色主题。当前实现（`unix.cppm`）有两条**不成立的时序假设**：

1. **整个终端往返必须在 50ms 内开始**（单次 `select()`，硬编码 50ms 超时）。
2. **单次 `read()` 必须一次读到足够解析的完整回复**。

Tabby（Electron + xterm.js + PTY，多层异步边界）的回复常晚于 50ms 到达。此时：`select()` 超时 → 函数返回 `nullopt` → `RestoreGuard` 立刻**恢复 ECHO** → Tabby 的合法 OSC-11 回复随后到达 tty 输入侧 → 被回显成 `^[]11;rgb:1717/1717/1717^[\` 并被 zsh/ZLE 当作命令输入。分片回复是同一根因的另一种表现（首个 `read()` 只拿到前缀）。

**根因不是 Tabby**，而是查询端的不安全假设：符合规范的终端**不保证**在一次 read 或 50ms 内返回完整 OSC 回复。

**修复**：把回复当作**帧字节流**处理，并用一个**确定性栅栏**判定「终端已回完」：

- 在 OSC-11 查询后**紧跟一个 DSR 光标位置查询** `ESC [ 6 n`。终端按输入顺序处理，故 CPR 回复 `ESC [ <行> ; <列> R` 必然排在 OSC-11 回复（若支持）之后。**收到 CPR 即证明 OSC-11 回复（若有）已全部到达并被我们读走**。
- 用**单调总截止时间**（默认 500ms，可经 `XLINGS_TERM_QUERY_TIMEOUT_MS` 覆盖，钳制 [50, 5000]）循环 `select()`/`read()`，累积进有界缓冲区，直到出现 CPR 终止符 `R`、缓冲区满、或超时。
- **在读循环全程保持 ECHO 关闭**（`RestoreGuard` 仍在函数末尾恢复），因此把 OSC-11 回复**和** CPR 一并读走消费，不会泄漏、不回显。

对**能应答的终端**（绝大多数，含 Tabby）栅栏在数毫秒内命中即返回，无泄漏、无输入丢失，是干净路径。只有「既不应答 OSC-11 也不应答 DSR」的罕见 tty 才会走满超时（每进程一次性成本）。

## 1. 为什么用 DSR/CPR 栅栏而不是「单纯加大超时」

Issue 里已点明：*「单纯增大超时只降低概率，不解决分片读，也不解决刚好在新截止时间之后到达的回复。」*

- **加大超时**：仍是「猜一个够大的数」。回复晚于新阈值到达仍泄漏；分片时首个 read 拿到前缀就解析失败。
- **读到终止符为止**（BEL `\a` / ST `ESC \`）：能处理分片，但**无法判定「终端不支持 OSC-11」**——不支持时永远等不到 OSC 终止符，只能走满超时；且若我们一见 OSC 终止符就停，尚在途中的其它回复仍可能泄漏。
- **DSR/CPR 栅栏**（本方案，`termbg` 等成熟实现采用）：几乎所有支持 OSC-11 的终端都支持 DSR。CPR 是**确定性完成信号**——收到它就知道「终端已把该发的都发完」，这正是把 ECHO 安全恢复所需要的判据。既处理慢、又处理分片，还能区分「不支持 OSC-11」（只回 CPR、不回 OSC）。

`R` 作为停止符是安全的：我们只发送 OSC-11 + DSR 两个查询；OSC-11 回复体（`ESC ] 11 ; rgb:HHHH/HHHH/HHHH` + ST/BEL）只含十六进制与分隔符，绝不含 `R`。故缓冲区中首个 `R` 唯一标记 CPR 结束。

## 2. 可测试性重构

`query_terminal_is_light()` 直接开 `/dev/tty` + 改 termios，CI 里没有受控 tty，无法单测。故拆成两块纯逻辑 + 一个薄封装：

1. **`parse_terminal_bg_is_light(std::string_view buf) -> std::optional<bool>`**（纯函数，导出）
   在累积缓冲区里找 `rgb:`，解析三个 16-bit 通道，按 Rec.601 luma 阈值（32768）判浅色。可直接用字节串单测：完整回复、BEL/ST 终止、前置无关字节、畸形、无 rgb。
2. **`read_terminal_query_reply(int fd, std::chrono::milliseconds timeout) -> std::string`**（导出）
   在给定 fd 上做「单调截止时间 + 循环 select/read + 有界累积 + 见 `R` 即停」的帧读。用 `socketpair` 单测：另一端线程按 立即 / 延迟>50ms / 分片 / 无应答 各场景写入，验证「有界时间内返回、拿到正确字节、终止符处理」。
3. **`query_terminal_is_light()`**（薄封装）：`isatty` 卫语句 → 开 `/dev/tty` → termios raw → 写 `OSC-11 + DSR` → `read_terminal_query_reply` → `parse_terminal_bg_is_light`。termios/`/dev/tty` 部分不在 CI 单测覆盖（无受控 tty），逻辑全在上面两块被覆盖。

## 3. 验收（对应 Issue 的 Acceptance Criteria）

针对每个「延迟 / 分片 / 畸形 / 无应答」场景：
- 函数在**有界时间**内返回或回退 ✓（单调截止时间）；
- 原 termios 状态被恢复 ✓（`RestoreGuard` 不变）；
- **无 OSC 字节泄漏进父 shell** ✓（读循环消费 OSC 回复 + CPR，ECHO 全程关闭）；
- 无关输入不丢失 ✓（不做 `tcflush` 盲刷，只读走终端自己产生的应答字节）。

单测覆盖：立即完整回复、延迟 > 50ms、跨多次 read 分片、BEL 与 ST 两种终止符、畸形回复、完全无回复、OSC 回复前有无关字节。`/dev/tty` + termios 路径与「stdout/stderr 重定向但存在受控 tty」由既有 `isatty` 卫语句处理，保持不变。

## 4. 兼容性 / 风险

- 对 macOS Terminal、iTerm2、Alacritty、Kitty、WezTerm、Windows Terminal 等**能应答**的终端：栅栏毫秒级命中，行为等价于旧实现但更稳。
- 新发一个 DSR 查询：CPR 回复被我们完整读走消费，屏幕上不可见。
- 罕见「isatty 但既不应答 OSC 也不应答 DSR」的 tty：走满超时（默认 500ms，可下调）后回退到 COLORFGBG/Dark，每进程一次，不挂死。
- Windows 走 `windows.cppm` 自己的 stub，不受影响。
