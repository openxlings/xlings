# 四个缺陷:深度审计重复、install 串行、空 url 噪声、Windows 输出错乱

> 状态:**已实现并落地**(2026.8.14.1)。落地结果见 §7,
> 其中**三条原方案的判断被实测或复查推翻**,已在原处标注而不是悄悄改掉。
> 所有数字都在用户真实 home(`/home/speak/.xlings`,363 payload / 73 GB)上实测,
> 命令与脚本见每节的「实测」小节,可复现。

## 0. 一句话

四条看上去无关的抱怨,有两个共同形状:

- **①③** —— **一次就够的事做了 N 次,或者根本不必做**:
  深度审计跑 7 遍、每个 ELF fork 两个进程、48% 的字节花在别人的 home 上;
  一条「故意没有 url」的声明被当成异常来报。
- **②④** —— **正确的动作,错误的边界**:
  锁的边界画在整条命令上(而不是它真正保护的 read-modify-write),
  日志的边界画在「三次 write」上(而不是一行)。

前者是**性能**,后者是**并发正确性**。分开修,互不阻塞。

---

## 1. `self doctor --fix` 很慢、日志重复循环

### 1.1 现象

用户贴的日志里,`auditing payloads N/363` 的计数**归零重来了 7 次**:

```
16/363 → 37 → 69 → 82 → ... → 358      ← 第 1 遍
16/363 → 35 → 65 → 82 → ... → 358      ← 第 2 遍
...
14/363 → 16 → 31 → 60 → ... → 343      ← 第 7 遍
```

不是「循环 bug」,是**深度审计真的被完整执行了 7 次**。

### 1.2 根因 A:`refresh()` 每次都重跑整个深度审计

`src/core/xself/doctor.cppm:3544`:

```cpp
const auto refresh = [&] {
    Config::reload_state();
    state = load_state_();
    scan  = detect_(state, probe, audit);   // audit.deep == true(--fix 蕴含 --deep)
};
```

`--fix` 路径上的调用点(同文件):

| # | 行 | 触发 | 之前那一步能改变 payload 字节吗 |
|---|---|---|---|
| 1 | 3516 | 初次 `detect_` | — |
| 2 | 3561 | 阶段 1(state / other-subos / local 元数据修复)后 | **不能** |
| 3 | 3569 | 阶段 2(payload 阶梯,会重装)后 | 能 |
| 4 | 3577 | 阶段 2b(incomplete 重装)后 | 能 |
| 5 | 3590 | 阶段 3(元数据修复再来一次)后 | **不能** |
| 6 | 3612 | 阶段 3.5(激活/去激活)后 | **不能** |
| 7 | 3619 | 阶段 4(prune,仅 `pruned>0`)后 | **不能** |
| 8 | 3623 | prune 后的 local 修复后 | **不能** |

6 次无条件 + 2 次条件 = 最多 8 遍;用户日志里 `pruned == 0`,所以正好 **7 遍**。

**其中 5 遍在重新扫描字节,而上一步只动了 JSON 元数据和符号链接。**

### 1.3 根因 B:每个 ELF 两次 `popen` + patchelf,读满整个文件

`src/core/elf_same_source.cppm:455-481`,`scan_payload()` 对每个 payload:

1. `recursive_directory_iterator` 遍历**整个 payload 目录**,对每个普通文件 open+read 4 字节判 ELF 魔数;
2. 每个 ELF 调 `patchelf --print-interpreter`;
3. `interp` 非空的再调一次 `patchelf --print-rpath`。

`platform::run_command_capture` 走 `popen` → `/bin/sh -c` → `patchelf`,**每次调用 2 个进程**。
而 `patchelf` 为读一个 header **要把整个文件读进来**。

另外 `locate_patchelf(dir)` 在**每个 payload** 都重新扫一遍 store 根目录找 patchelf
(`elf_same_source.cppm:365`),即每遍 363 次目录扫描,结果完全相同。

### 1.4 实测

```
store:            363 payload roots / 73 GB
普通文件:         477,848        ← 每遍全部 open+read(4B)
ELF 文件:         13,729         ← 每遍全部调 patchelf --print-interpreter
其中带 PT_INTERP: 1,279          ← 只有这些需要第二次调用,也只有这些是检查对象
patchelf 读取字节: 56.54 GB / 遍
```

400 个 ELF 随机采样实跑 `sh -c "patchelf --print-interpreter …"`(page cache 已热):

```
400 calls / 1690.6 MB / 2.25s  →  5.62 ms/call
外推一遍(仅 --print-interpreter): 按字节 75s,按次数 77s   ← 两法一致
```

**一遍的真值(实测,不是外推)** —— `self doctor --deep` 是只读的,直接量:

```
$ time xlings self doctor --deep
real  2m3.121s
user  0m24.802s
sys   1m3.782s        ← 一半时间在内核:15,008×2 次进程创建 + 477,848 次 open + 56 GB 读
```

**123 秒/遍**,比上面的外推(75–77s)还高约 60%——差额正是目录遍历、
1,279 次 `--print-rpath` 和 363 次 `locate_patchelf` store 扫描。

× 7 遍 ≈ **14 分钟**。冷启动更差。

> 这与 memory 里「0810.5 doctor --fix 148s→0.64s」不矛盾:那次量的是**没有可修项**
> 的路径,深度审计没有真正走完 7 遍。这次的 home 有 363 个 payload 且有可修项。

### 1.5 根因 C:48% 的字节花在**别人的 home** 上

最大的几个 ELF 之一:

```
518.9 MB  xim-x-mcpp/0.0.7/registry/data/xpkgs/xim-x-musl-gcc/15.1.0/libexec/.../cc1plus
```

`mcpp` 的每个版本 payload 里都带着一个**嵌套的 xlings home**(`registry/data/xpkgs/`),
而递归遍历会一路走进去。实测:

```
嵌套 store 里的 ELF: 4,845 个 / 26.93 GB
```

**占全部审计字节的 48%**。这些 payload 属于另一个 home,xlings 既不拥有它们、
`--fix` 也修不了它们,而且装了 30 多个 mcpp 版本就重复审计 30 多遍近乎相同的内容。

### 1.6 方案

四条改动,**互相独立、可分别落地**,收益递增:

**F1 — 用进程内 ELF 解析取代 patchelf 子进程(最大收益,风险最低)**

`PT_INTERP` 和 `DT_RPATH`/`DT_RUNPATH` 都是 ELF 里几十字节的字段,不需要外部工具:
读 64 字节 ELF header → 读 program header 表 → 找 `PT_INTERP`;
需要 rpath 时再读 `PT_DYNAMIC` + `.dynstr`。

实测同等逻辑(Python,全 477,848 文件):

```
in-process header scan: 2.53s   ← 对比 patchelf 路线 ~77s,30×
```

C++ 只会更快。**子进程数 15,008 → 0**,读取字节 56.54 GB → 按需(≤25.87 GB,且实际只读 header 页)。

> 兼容性要点:`scan_payload` 是**安装路径和 doctor 共用**的(`installer.cppm:3009`),
> 这正是它们不会漂移的原因。改实现不改语义,两边同时受益;
> `locate_patchelf` 的 host-fallback 分支可以整个删掉——不再需要外部工具,
> 也就不再有「装了 patchelf 才能验」这个隐藏前提。
> **保留 `patchelf` 作为交叉校验**写进单测:同一批文件,两条实现结果必须一致。

**F2 — 深度审计只在「字节可能变了」之后重跑**

把 `detect_` 拆成两层:

```cpp
scan  = detect_(state, probe, audit);        // 现状:全量
// 改为:
scan  = detect_(state, probe, audit);        // 首次:含深度
refresh_shallow();                            // 元数据类修复之后(#2,5,6,7,8)
refresh_deep();                               // payload 阶梯 / incomplete 之后(#3,4)
```

7 遍 → **3 遍**。语义不变:阶段 1/3/3.5/4 只写 JSON、shim 和激活状态,
不可能改变任何 payload 里的 ELF 字节。

> 风险:阶段 3.5 的 `repair_inactive_` 会 `xlings use`,而 `use` 目前**会重写
> `bin/xlings` 入口二进制**(见 AGENTS.md「Diagnosing a shim」)。那是一个
> payload 之外的文件,不在审计范围内——但这条要在实现时**用测试钉住**,
> 否则 F2 就是一个静默失效的正确性回归。

**F3 — 以 install stamp 为键缓存每个 payload 的审计结果**

payload 目录安装后不可变,且每次(重)安装都会重写 `.xim-installed` stamp
(`installer.cppm:2946 / 3141`)。以 `(payload_dir, stamp mtime+size)` 为键缓存
`scan_payload` 的结果,进程内 map 即可:

- 同一次 `--fix` 的 3 遍 → 第 2、3 遍近乎免费;
- 被重装过的 payload,stamp 变了,自动失效并重扫。

> 不要用目录 mtime 做键:原地改写文件内容不会更新目录 mtime。stamp 才是正确的键。

**F4 — 不审计嵌套 store**

遍历时遇到 `*/registry/data/xpkgs/` 或任何含 `.xlings.json` 的子目录即剪枝,
并在 verbose 下说明跳过了什么(**不要静默截断**——那是本仓库反复出现的
「没发生和成功了输出一样」)。省掉 48% 的字节和 4,845 个 ELF。

### 1.7 预期

| | 现状 | F1 | F1+F2 | F1+F2+F3+F4 |
|---|---|---|---|---|
| 一遍 | **123s(实测)** | ~4s | ~4s | ~2s |
| 遍数 | 7 | 7 | 3 | 3(后两遍命中缓存) |
| 总计 | **~14 min** | ~28s | ~12s | **< 5s** |

> 基线已经有真值了(123s/遍),F1 之后**先重量一次**再决定要不要做 C 批。

### 1.8 顺带:进度输出

7 遍各自从 0 计数,用户看到的是「卡在同一个包上循环」。F2 之后仍有 3 遍,
所以进度行应当带上**遍次**:`auditing payloads (pass 2/3) 37/363 — …`。
一行字,消掉「循环 bug」的错觉。

---

## 2. 一个 `xlings install` 跑着,另一个 `xlings install` 卡住

### 2.1 根因 A:锁的边界是「整条命令」,不是它保护的东西

`src/core/xim/commands.cppm:211`,`cmd_install` **第一件事**就是取 home 全局锁:

```cpp
auto stateLock = xvm::acquire_state_lock(Config::paths().homeDir);
```

`stateLock` 是函数局部变量,**直到 `cmd_install` 返回才释放**。这中间包括:

```
index sync → catalog rebuild → 依赖解析 → 【网络下载】→ 【解压】→ 【install/config hook】→ 注册写库
                                             ↑ 几十秒到几十分钟         ↑ 可能编译
```

锁真正保护的只有最后一段(version DB + workspace 的 read-modify-write,
见 `xvm/lock.cppm` 顶部注释)。但**下载和解压也被串行化了**——
即使两条命令装的是完全不相干的包。

> 反证:下载层**已经有**自己的细粒度锁(`downloader.cppm:582` 的 cache lock)。
> 所以「细粒度锁」在本仓库不是新概念,只是 install 没用上。

### 2.2 根因 B:等待期间**完全静默**

`src/platform/unix.cppm:57-77`:

```cpp
auto deadline = now() + timeout;              // timeout = 30s
while (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    ...
    std::this_thread::sleep_for(50ms);        // ← 30 秒,一个字都不打印
}
```

30 秒 0 输出,然后才吐出错误。用户说的「卡住」就是这 30 秒。
而且错误信息**说不出是谁占着锁**——注释解释了原因(往 lock 文件里写内容会因为
`write_string_to_file` 的 rename 换掉 inode 而破坏 flock,见 memory
`reference_atomic_write_vs_flock`)。这个理由对 **lock 文件本身**成立,
对**旁边另一个文件**不成立。

### 2.3 方案

**L1 — 立刻说话(改动最小,先落地)**

第一次 `flock` 失败就打印,而不是等超时:

```
[xlings] another xlings is changing this home (pid 12345, `install gcc`, 已 8s);等待中… Ctrl-C 取消
```

- **holder 身份**:取锁成功后写 `<home>/.xlings.lock.owner`(pid / 命令行 / 起始时间)
  —— 一个**旁文件**,不是被 flock 的那个 inode,所以 rename 语义与 flock 无冲突。
  读取方把它当**提示**,不当真相(进程可能已死;pid 可复用)。
- **超时**:等待期间有进展就不应该硬性 30s 失败。改为「无限等 + 可 Ctrl-C」,
  或保留超时但显著加长,并在消息里给出 `XLINGS_NO_LOCK=1` 之外的正常出路。

**L2 — 缩小锁的范围(真正的修复)**

把 `cmd_install` 切成三段,只有第三段持锁:

```
① 解析(catalog / DAG / 版本选择)   ── 只读,无锁
② 获取(下载 → 校验 → 解压到 staging) ── 无 home 锁;
                                        用 per-payload 锁(destDir 上的 flock)
                                        防止两个进程写同一个 payload
③ 提交(payload 落位 + hook + 注册)  ── 持 home 锁
```

前提与代价,**必须一起交底**:

- ① 在无锁状态下算出的计划,到 ③ 可能已经过时(另一个进程装完了同一个包)。
  ③ 拿到锁后**必须重读 state 并复核计划**——这正是现有注释
  (`commands.cppm:207-210`)已经在做的 `Config::reload_state()`,把它变成
  「重新校验」而不只是「重新读取」。
- install hook 会 `xlings install`(d2mcpp 的 config hook 就是),
  靠 `XLINGS_STATE_LOCK_HELD` 重入标记跳过加锁。切段之后 ③ 才持锁,
  **hook 在 ③ 之内**,重入语义不变。这条要有 e2e 钉住。
- ② 的 per-payload 锁不能用 home 锁那把:两个进程装**不同**包必须能并行,
  装**同一个**包必须串行。键就是 `destDir`。

**L3 — 只读命令不该被写命令挡住(核对,可能已成立)**

`lock.cppm` 顶部声明「只有 mutating 命令取锁」。需要逐条核对
`info` / `list` / `search` / `doctor`(不带 `--fix`)确实不取锁,
并补一个 e2e:持锁进程在跑时,这些命令必须在 1s 内返回。

### 2.4 落地顺序

L1 单独就能把「卡住」变成「在等,等谁,等多久」——**先发 L1**。
L2 是行为改变,需要并发 e2e(两个 install 同时跑,一个装 A 一个装 B,
断言二者都成功且总耗时 < 串行的 1.5×)。

---

## 3. 故意留空 url 的包,不该打 warn

### 3.1 现象与根因

`src/core/xim/installer.cppm:2376`:

```cpp
auto resource = detail_::resolve_download_resource_(...);
if (!resource) {
    log::warn("skipping {}: {}", node.name, resource.error());
    continue;                       // ← 只是不下载;hook 照常跑,包照常装成
}
```

`continue` 之后节点仍进入安装阶段。所以对于**纯 hook 包**(在宿主机上建符号链接、
不下载任何字节的那一类),这条 warn 是**纯噪声**:什么都没出错。

真实例子(`xim-pkgindex/pkgs/n/nvidia-gl-host-link.lua`):

```lua
["latest"] = { ref = "0.1.1" },
-- No payload: everything this package installs is a symlink it
-- creates at install time from what it finds on the host.
["0.1.1"] = { },        -- ← 空 entry,故意的
```

### 3.2 判据已经现成:两个错误串是不同的东西

| 错误串 | 来自 | 含义 |
|---|---|---|
| `resource has neither url nor source for <plat>@<ver>` | libxpkg `xpkg-compat.cppm:180`(`kind == SourceKind::None`) | 配方**根本没声明**下载源 → **故意的** |
| `resolved resource URL is empty` | xlings `installer.cppm:1196` | 已识别出源的种类,但选完 mirror / 展开模板后**变成了空** → **异常** |

第二条是真 bug(mirror 表里有空值、模板展开成空、xlings-res URL 构造失败),
它必须**继续是 warn**。第一条应该降级。

### 3.3 方案

**W1 — 分流,不是一刀切降级**

让 `resolve_download_resource_` 返回**带类型的**失败,而不是字符串:

```cpp
enum class NoResource { Declared,   // 配方没声明源 —— 正常
                        Empty,      // 声明了却解析成空 —— 异常
                        Error };    // 其它(平台不匹配等)
```

- `Declared` → `log::debug("{} declares no download source; install hooks only", name)`
- `Empty` / `Error` → 维持 `log::warn`

**不要**靠匹配错误字符串来分流——那是把 libxpkg 的措辞变成 xlings 的 ABI。
若 libxpkg 当前不暴露 kind,先在 xlings 侧判断「entry 是否声明了 url/source/per-arch 子表」,
并在 libxpkg 下个版本加一个显式的 `SourceKind::None` 出口(它内部已经有这个枚举值)。

**W2 — 降级之后,谁来抓真正忘了写 url 的配方?**

这是 W1 唯一的风险:一个**手滑漏写 url**的配方,现在也会静默。
答案是:**下游已有更强的检查**,而且它们检查的是结果而不是意图——

- `programs` 声明了却没注册 → 「installed but registered none of the programs it declares」
- payload 目录空 → `payload_has_content` / closure check
- install hook 失败 → 失败标记 + `info` 报 incomplete(2026.8.11.1)

所以正确的做法是:**降 warn 为 debug 的同时,确认上述三道检查对纯 hook 包确实生效**,
并补一个 index 侧的结构检查(见 memory `reference_index_edit_structural_check`):
「声明了 `xpm.<plat>` 版本 entry 但既无 url/source 又无 install hook」= 配方错误,CI 拦。

---

## 4. Windows 安装时日志换行错乱、前面莫名缩进

### 4.1 四个可验证的结构性缺陷

**D1 — 一行日志被拆成 2–3 次 write,中间没有任何原子性**

`src/core/log.cppm:128-138`:

```cpp
std::print("{} ", colored_(palette::cyan(), "[xlings]"));                 // write 1
if (!gContext_.empty()) std::print("{} ", colored_(..., "[ctx]"));        // write 2
std::println("{}", msg);                                                  // write 3
```

任何别的写入者落在 1 和 3 之间,就会**插进这一行里面**。
`[xlings] ` 于是变成孤零零挂在另一行开头的「缩进」——正是用户描述的症状。

**D2 — 下载期间确实有别的写入者,而且是另一个线程**

`src/core/xim/downloader.cppm`:

- **TUI 刷新线程**(:937)每 200ms 重绘一帧;
- **N 个下载 worker 线程**(:995)在同时调 `log::warn` / `log::debug`
  (:473 / :549 / :656 / :702 等 15 处)。

worker 的 `log::*` **不持** TUI 那把 `mutex`。两者对同一个终端并发写。

**D3 — 帧重绘的 `lastLines` 记账,假设「两帧之间没人写过 stdout」**

`src/ui/progress.cppm:400-408`:

```cpp
if (rewrite && prevLines > 0)
    output += "\033[" + std::to_string(prevLines) + "A\r";   // 上移 prevLines 行
output += body;
if (rewrite) output += "\033[J";                              // 清到屏幕末尾
std::cout << output << std::flush;
```

两帧之间只要打了一行日志,光标就比 `prevLines` 记的位置**多下移了**,
于是「上移 N 行」落在日志文本中间:帧从半行处开始画,`\033[J` 从光标处清,
**日志行的前半截(正是那个 `[xlings] ` 前缀)被留在屏幕上**。

**D4 — 两套输出 API 指向同一个 fd**

`log::*` 走 `std::print`(C stdio `stdout` FILE*),
`ui::progress` / `ui::layout` 走 `std::cout`(iostream),
`doctor` 还夹着 `std::cout.flush()`。

在 glibc 上两者最终都经 `stdout` FILE*,顺序至少是**对的**(只是交错)。
**在 MSVC 上这两条路是真正的两个 sink**:
`std::print` 检测到控制台会走 `WriteConsoleW` 直接输出,绕过 FILE* 缓冲;
`std::cout` 写进自己的 streambuf → FILE* 缓冲。
再加上 MSVC 的 stdout 对控制台**没有行缓冲**(`_IOLBF` 等同 `_IOFBF`),
而 `log::warn`/`log::error` 写的是**无缓冲的 stderr**。

→ **同样的代码,Linux 上是「交错但有序」,Windows 上是「可以乱序」**,
并且缓冲区会在**任意字节边界**冲刷,包括一行的正中间。
这就是「只有 Windows 特别乱」的那部分原因。

> **需要在 Windows 上实测确认的是 D4**(D1–D3 从代码即可确定)。
> 最小复现:`xlings install <多个包>` 在 Windows Terminal 里跑,
> 分别测 ① 直接跑 ② `2>$null` ③ `| more`;再用
> `XLINGS_TERM_WIDTH=0`(关掉重绘)对照。如果 ③ 和「关重绘」都正常,
> D2/D3 是主因;如果 ① 关了重绘仍乱,D4 是主因。

### 4.2 方案

**O1 — 一行日志 = 一次 write(独立、低风险、先做)**

`log.cppm` 里先把前缀 + context + msg + `\n` 拼成**一个 string**,再一次输出。
顺带解决多行消息:嵌入的 `\n` 后面补上等宽缩进,让续行对齐而不是顶到第 0 列
(`downloader.cppm:702` 的候选 URL 列表就是多行消息,它现在的 `"\n    "` 是手写的,
应该由 log 层统一负责)。

**O2 — 全进程一个输出 sink**

选定 `std::print`(stdio)或 `std::cout`(iostream)**其中之一**,把另一套改掉。
建议统一到 `std::cout`:ftxui/layout 已经在用它,且 iostream 的 streambuf
在 Windows 上不会走 `WriteConsoleW` 旁路。改完在启动时显式
`std::ios::sync_with_stdio(false)` 之外**不要**再混用 `printf`/`std::print`。

**O3 — 终端输出加一把锁,并让日志与帧协作**

一个 `terminal_mutex`,`log::*` 和帧重绘都持有它。日志在持锁期间要:

```
擦掉当前帧(上移 prevLines + \033[J) → 打日志行 → 重画帧 → 更新 prevLines
```

这才是 D3 的真修复:`prevLines` 不再被外部写入悄悄作废。
最简实现是把 `log::*` 在「有活动帧」时路由给渲染器,由渲染器统一落笔。

**O4 — Windows 启动时把 stdout 设为无缓冲(或行为等价)**

`init_console_output()` 里加 `setvbuf(stdout, nullptr, _IONBF, 0)`(或每行显式
`fflush`)。代价是 syscall 变多,但 CLI 的输出量本来就小,而收益是
stdout/stderr 的相对顺序在 Windows 上变得可预期。
**这条要先由 4.1 的实测确认 D4 成立再做**——否则是在修一个不存在的问题
(见 memory `reference_validate_release_via_self_install` 的教训)。

---

## 5. 落地建议

按「独立可发、风险从低到高」排:

| 批次 | 内容 | 风险 | 收益 |
|---|---|---|---|
| **A** | W1+W2(空 url 分流)、L1(锁等待可见)、1.8(进度带遍次) | 低 | 噪声与困惑立刻消失 |
| **B** | F1(进程内 ELF 解析)、O1(一行一次 write) | 低—中 | `--fix` 快 25×;输出错乱大部分消失 |
| **C** | F2+F3+F4(遍数、缓存、嵌套 store) | 中 | `--fix` 进入秒级 |
| **D** | L2(install 锁切段)、O2+O3(输出 sink 与协作)、O4 | 中—高 | 并发安装;Windows 输出彻底干净 |

**每批都要有能失败的测试**,尤其是:

- F1:同一批文件,进程内实现与 `patchelf` 结果逐条一致(patchelf 从依赖降级为**对照组**)。
- F2:构造一个「阶段 3.5 之后 payload 字节变了」的场景,断言它**被抓到**——
  如果抓不到,说明 F2 的前提错了,应当回退而不是加特例。
- F3:重装一个包后同一进程内再审计,断言缓存**失效**(否则就是又一个静默成功)。
- L2:两个 install 并发装不同包,均成功,且总耗时 < 串行 × 1.5。
- O1/O3:把输出接到管道,断言每行以 `[xlings] `/`[warn] ` 起始且没有裸露的前缀残片。

## 6. 已知的不确定项(不要当成结论)

1. **D4 未在 Windows 实测**。4.1 给了判别实验,先测再改。
2. **F2 的前提**(阶段 1/3/3.5/4 不改 payload 字节)是读代码得出的,
   `repair_inactive_` 会 `xlings use`,而 `use` 会重写 `bin/xlings`——
   该文件不在 payload 审计范围内,但这要用测试钉住,不能只靠推理。
3. **单遍 123s 是实测,7 遍 ≈ 14 分钟是由此相乘得到的**。
   没有直接量 `--fix` 的总时长,因为它会真的改这个 home。
   如果要一个端到端真值,应当在 `slice-real-home.sh` 切出的副本上跑
   (见 memory `reference_repro_from_real_home_slice`),不要在真 home 上量。

---

## 7. 落地结果(2026.8.14.1)

### 7.1 实测

同一台机器、同一个 home(`--deep` 是只读的,可以反复量):

| | 改前 | 改后(冷) | 改后(热) |
|---|---|---|---|
| `self doctor --deep` 单遍 | **2m3.1s** | 59.8s | **3.0s** |
| 其中 user+sys | 24.8s + 63.8s | 6.2s + 10.9s | 1.9s + 1.2s |

**输出逐字节相同** —— 这是比速度更重要的那个数:

```
$ diff <(grep -v "auditing payloads" 改前.log) <(grep -v "auditing payloads" 改后.log)
*** IDENTICAL OUTPUT ***
```

363 个 payload、13,729 个 ELF、一个真实的 73 GB home,新旧两条读取路径给出
完全一样的结论。

`--fix` 的多遍现在命中缓存,所以总时长不再是「单遍 × 7」。

### 7.2 三条被推翻的判断

**① F2(按阶段跳过深度审计)没有做,而且是故意不做的。**

原方案说「阶段 1/3/3.5/4 不改 payload 字节,所以可以跳过重扫」。这是对的,
但它是一条**没有任何机制强制**的断言:哪天某个阶段长出副作用,审计就看不见了,
而且不会有任何提示 —— 正是本仓库反复付出代价的那个形状。

改用 **F3 缓存**。每一遍仍然问每一个 payload,只是 key 没变就不重读字节。
它不对「哪个阶段会做什么」下任何断言:谁改了 payload,谁的 key 就变,谁就被重扫。
**更慢一点点,但它不会静默失效。**

**② F1 的收益比预估大得多,原因也不是预估的那个。**

预估 30×(基于 patchelf 调用次数)。实测热态 123s → 3.0s = **41×**。
差额来自一个预估里没有的项:`patchelf --print-interpreter` 被调用在**全部
13,729 个 ELF** 上,而其中只有 1,279 个带 PT_INTERP。剩下 12,450 个是共享库,
patchelf 为了回答「没有 .interp」把每个文件整个读了一遍 —— 56.54 GB 里的大部分
花在了确认「这里没有答案」上。进程内读 program header 表,这一类文件的成本降到几 KB。

**③ patchelf 从「依赖」变成了「对照组」,这比性能更值钱。**

改前:`locate_patchelf` 找不到 patchelf → `scan_payload` 返回空 → **一次找不到工具的
扫描和一次干净的扫描输出完全相同**。这正是 §0 说的那个形状,而它一直藏在
性能问题下面。改后审计不需要任何外部工具;patchelf 留在
`tests/unit/test_elfread.cpp` 里做交叉校验(`XLINGS_ELFREAD_ORACLE`)。

### 7.3 逐项落地情况

| | 状态 | 说明 |
|---|---|---|
| F1 进程内 ELF 解析 | ✅ | 新模块 `xlings.core.elfread`;子进程 15,008 → **0** |
| F2 按阶段跳过 | ❌ **主动放弃** | 见 §7.2 ①;由 F3 取代 |
| F3 stamp 缓存 | ✅ | `elfcheck::PayloadScanCache`,键 = stamp mtime/size + 目录 mtime |
| F4 跳过嵌套 store | ✅ | 少走 4,845 个 ELF / 26.93 GB,`log::debug` 说明跳过了什么 |
| 进度带遍次 | ✅ | `auditing payloads (pass 2) 37/364` |
| W1 空 url 分流 | ✅ | `declares_no_download_source` 问配方,不匹配错误串 |
| W2 下游检查确认 | ✅ | payload 空 / programs 未注册 / 失败标记三道检查都还在 |
| L1 锁等待可见 | ✅ | 首次失败即打印 + `.xlings.lock.owner` 旁文件 + 存活检测 |
| L1b 超时 30s → 10min | ✅ | `XLINGS_LOCK_TIMEOUT`(秒,0 = 无限等) |
| L2 锁切段 | ⚠️ **只做了安全的一半** | 见 §7.4 |
| O1 一行一次 write | ✅ | 含续行缩进;并发撕裂有测试 |
| O2 单一输出 sink | ✅ | `std::cout` → `stdout` FILE*,log 与 UI 同一个 |
| O3 终端仲裁 | ✅ | `xlings.core.console`:互斥 + foreign-output epoch |
| O4 Windows 无缓冲 stdout | ✅ | `setvbuf(_IONBF)`,仅 Windows |

### 7.4 L2 只做了安全的一半,以及为什么

**做了的**:home 锁的获取从 `cmd_install` 第一行**移到索引加载之后**。
索引同步是网络操作、可能好几秒,而它读的是 index 树、不是这个 home ——
两条 install 装不相干的包时,不该有一条在等另一条拉索引。

**没做的**:把下载和解压移出锁外。

线索是清楚的 —— 下载写的是 `<dataRoot>/runtimedir` 这个**缓存**(而且它
已经有自己的 per-file 锁,`downloader.cpp:498`),不是 payload store。所以
「解析(无锁)/ 获取(per-payload 锁)/ 提交(home 锁)」这个切法是可行的。

不做的理由是**它改变了两条并发 install 能观察到什么**:无锁算出的计划到提交
时可能已经过时,而「拿到锁后重新校验计划」不是一句 `reload_state()` 能了事的 ——
它是包管理器事务语义的重新设计,应当有自己的并发 e2e 和自己的 PR。
本轮把它塞进来,是拿已经量准的四项收益去赌一个没想清楚的事务模型。

**用户报的「卡住」已经不是这条在解决的了**:L1 之后,第二条 install 会立刻说出
「谁占着锁、跑的什么命令、等了多久」,而不是沉默 30 秒然后失败。

### 7.5 #4 里仍未实测的那一条

D1/D2/D3 是从代码直接确定的,并且都有测试(`tests/unit/test_log_line.cpp`,
含 8 线程并发撕裂测试)。

**D4(MSVC 的 `std::print` 控制台旁路 / stdout 无行缓冲)仍未在 Windows 上实测。**
O2 + O4 是按它成立来修的,而且**即使 D4 不成立这两条也无害**(单一 sink 和
显式 flush 在任何平台上都是对的)。要证实或推翻它,用 §4.1 的判别实验。

> **2026-08-16 更新:§4.1 三个case里的第 ③ 个(管道)已经在 Windows 上量到了,
> 结论是「管道那条路本来就没坏」。详见 §7.6。控制台那条仍然开着。**

### 7.6 D4 的一半:管道那条路在 Windows 上量到了,而且它本来就没坏

**样本**:xim-pkgindex 的 `windows-test` job —— 一次真实的
`xlings install msvc` + `xlings install windows-sdk`,在 Windows runner 上
下载 23 个 payload、约 220 MB,下载线程与 TUI 刷新线程都在跑。
这正是 §4.2 D2 描述的那个并发形状,不是构造出来的复现。

**测的是什么**:GitHub 把 job 的 stdout/stderr 接到管道,所以这是 §4.1 判别
实验里的第 ③ 个 case(`| more`)。逐行扫 491 行原始日志:

| 症状 | 判据 | 结果 |
|---|---|---|
| D1 一行被拆开 | 只有 `[xlings]` 前缀、后面空的行 | **0** |
| D1/D3 前缀出现在行中间 | `\S\s*\[xlings\]` | **0** |
| D3 帧重绘落在日志里 | `\033[<n>A` / `\033[J` | **0** |

出现的转义只有 `\033[K`(擦到行尾)和 `\033[0m`(颜色复位),各 56 / 20 次 ——
**光标上移和清屏一次都没有**。也就是说渲染器认出了非 tty,老老实实线性输出,
没有走那条会与日志抢光标的路径。

**⚠️ 这个样本量的是 2026.8.10.1,不是 2026.8.14.1。** index CI 把 xlings 钉在
`v2026.8.10.1`(`.github/workflows/ci-test.yml`),即 O1/O2/O3/O4 **全部落地之前**
的版本。所以这条不能读成「修复生效了」,只能读成一句更强的话:

> **管道那条路从来就没坏过。** 在修复之前,同样的并发写、同样的 Windows,
> 通过管道输出零撕裂。

这恰好**收窄**了 D4 而不是回答它:用户报的错乱如果是真的,它只能来自
**控制台**那条路 —— `std::print` 的 `WriteConsoleW` 旁路、控制台上没有行缓冲,
以及只有 tty 才启用的帧重绘。三者都被管道绕开了。

**为什么 CI 结不了这个案**:GitHub runner 上 job 没有附着的控制台,所以
`GetBufferContents()` 那类办法在那里无从谈起。要证实或推翻 D4,仍然只能按
§4.1 在一台 Windows Terminal 里手动跑 ① 与 ②,并用 `XLINGS_TERM_WIDTH=0`
关掉重绘做对照。**这一条依然开着,不要因为上面这张表就当它关了。**

**顺带一条与 D4 无关、但同一份日志暴露出来的**:index CI 用 4 个版本以前的
xlings 验证 recipe(`ci-test.yml` 钉 `v2026.8.10.1`,`ci-xpkg-test.yml` 更旧,
钉 `v2026.8.8.2`)。这意味着 `windows-test` 证明的是「这些包在 2026.8.10.1 上
装得上」,而不是「在用户实际运行的 xlings 上装得上」。没有在这里改 —— 换版本
可能影响别的 recipe,应当是它自己的 PR —— 但记下来,因为这正是
「门禁测的不是发出去的东西」那个形状。

---

## 8. 端到端验证(不是「跑通了」,是「测过了什么」)

在 `.agents/tools/slice-real-home.sh` 切出的**真实 home 副本**上跑
(73 GB / 364 payload,12.4 秒切完;每次跑完都 `verify-untouched`,
真 home `data/xpkgs` 全程未被改动)。

### 8.1 `--fix` 全流程:14 分钟 → **8.76 秒**

```
$ XLINGS_HOME=<slice> xlings self doctor --fix
7.49user 1.26system 0:08.76elapsed 99%CPU
```

**缓存确实在起作用,而且有直接证据。**
`elfcheck: skipping nested store` 这行只在 `scan_payload` **真的执行**时才打
(缓存命中根本不进那个函数),所以数它就等于数真实扫描次数:

```
nested-store skips:  74
distinct nested stores: 74      ← 一一对应
```

整条 `--fix`(多遍 detect)里,**每个 payload 只被真正扫描了一次**。
缓存不生效的话这里会是 74 × 遍数。

顺带,这 74 个嵌套 store 也就是 §1.5 里那 48% 的字节,现在一个都不走。

进度行只出现 `pass 1` —— 不是 bug:进度是**为打破沉默而存在**的,
它扫描满 1 秒才开口,而第 2 遍之后全是缓存命中,根本没有沉默可打破。

### 8.2 锁等待:从沉默 30 秒变成一秒一句

用一个持有 flock 并写了 sidecar 的假 holder 复现:

```
[xlings] waiting for another xlings to finish with this home (pid 2041759, `xlings install gcc`)
[xlings] still waiting for another xlings (pid 2041759, `xlings install gcc`) — 1s
[xlings] still waiting for another xlings (pid 2041759, `xlings install gcc`) — 2s
...
（holder 释放后立即继续,install 正常完成）
```

### 8.3 空 url 包:确实变成 debug 了

第一次测试是**无效的** —— 包已安装,循环在 `node.alreadyInstalled` 就
`continue` 了,根本没走到改动的那一行。删掉 payload 目录重测才算数:

```
[debug] nvidia-gl-host-link: no download source declared; install hooks only
```

`[debug]`,不是 `[warn]`,且安装成功(exit 0)。

### 8.4 F1 等价性:与 patchelf 逐个对照

```
[ RUN      ] ElfRead.AgreesWithPatchelfOverARealStore
[       OK ] ElfRead.AgreesWithPatchelfOverARealStore (8338 ms)
```

真 store 里取样 300 个真实 ELF,`--print-interpreter` 和 `--print-rpath`
两个字段与 patchelf **逐条相同**。加上 §7.1 的整体输出逐字节相同,
这是「换了个读取器但结论没变」能给出的最强证据。

### 8.5 单元测试

41/41 目标全绿。新增:
`test_elfread.cpp`(9 个,含上面那个 oracle)、
`test_log_line.cpp`(5 个,含 8 线程并发撕裂测试)、
`test_no_download_source.cpp`(7 个)、
`test_elf_same_source.cpp` 里 4 个缓存失效测试。

> `test_log_line.cpp` 第一版用 `freopen("/dev/tty")` 还原 stdout —— 本机能过,
> **CI 上没有 tty,还原会失败,之后这个二进制里所有测试的输出都会写进临时文件**,
> 连 gtest 自己的报告一起消失。复查时发现,改成 dup/dup2 保存原 fd。

---

## 9. e2e 抓到的:两个测试的**观测手段**被这次改动拿掉了

单测 41/41 全绿、真机实测全对、`diff` 逐字节相同 —— 然后本地跑 e2e,**两个测试挂了**。
两个都不是产品回归,但都不是「改个断言就行」,值得记下来。

### 9.1 `elf_host_loader_payload_libc_guard_test.sh`:假 ELF 不再是 ELF

夹具是这样造的:

```sh
printf '\177ELF fixture\n' > "$PAYLOAD/bin/host-linked-app"    # 11 字节,不是 ELF
cat > "$TOOLS_DIR/patchelf" <<'SH'                              # PATH 上的桩
  --print-interpreter) printf '/lib64/ld-linux-x86-64.so.2\n' ;;
  --print-rpath)       printf '%s\n' "$ELF_GUARD_LIBDIR" ;;
SH
```

老实现只校验 4 字节魔数,然后**把两个字段问 PATH 上的 patchelf** —— 于是一个
11 字节的假文件加一个 shell 桩就能演出完整的场景。新实现读真正的 program header
表,假文件没有可读的结构,发现自然消失。

**这恰恰是这次改动的目的。** 一个能被 PATH 上的脚本满足的读取器,就是一个
**答案取决于机器**的读取器 —— 而 §7.2 ③ 说的正是它的另一面:patchelf 缺失时,
「干净」和「没扫」输出完全相同。

所以夹具改成**真 ELF**:ELF64,PT_INTERP 指宿主 loader,DT_RUNPATH 指 payload
的 core runtime 目录,用 python3 逐字节写出来(保留原来「不依赖宿主编译器和 libc」
这一点)。桩 patchelf 整个删掉。

### 9.2 `self_doctor_depth_test.sh`:整个测试建立在数 patchelf 调用次数上

它断言 `deep_patchelf == 1000`(500 个 ELF × 2 次调用)、`quick_patchelf == 0`,
以此证明「`--deep` 扫了,`--quick` 没扫,`--scope` 只扫一个」。
现在 patchelf 调用数恒为 0,**这个观测手段整个没了**。

要测的性质本身完全有效,没了的是**怎么看见它**。所以给审计加了一条它自己的覆盖报告:

```
[xlings] deep audit (pass 1): 1 payload(s) examined
[xlings] deep audit (pass 2): 365 payload(s) examined, 365 unchanged since the last pass
```

这比原来的观测**更好**,不只是「还能用」:

- 原来数的是问题下面两层的实现细节;现在报的**就是那个问题** ——
  「你到底看了多少」,由看的那个东西回答。
- 它顺手把 `PayloadScanCache::hits()` 用上了 —— 我写了却没有任何调用者,
  这本身就是个味道,复查时才发现。
- 它让 §8.1 里我原本要靠 debug 日志反推的缓存效果,变成**产品自己说的话**:

```
[xlings] deep audit (pass 1): 365 payload(s) examined
[xlings] deep audit (pass 2): 365 payload(s) examined, 365 unchanged since the last pass
... (共 6 遍)
```

一遍真扫、五遍全缓存,**不用再去数 `skipping nested store` 了**。

### 9.3 顺带踩了 memory 里记过的坑

新的 `audited_payloads()` 第一版是 `grep | grep | grep | awk`。
`set -euo pipefail` 下,**quick 那次跑没有匹配 ⇒ grep 退出 1 ⇒ 管道失败 ⇒
`x="$(audited_payloads)"` 直接杀掉整个脚本,一个字都不打印**。
现象就是 `EXIT=1`、零输出 —— 正是 `reference_e2e_set_e_silent_death` 记的形状,
而且是在**它要测的那个正确情形**上触发的。改成单个 awk(无匹配也退出 0)。

### 9.4 又两条:一条是我新引入的缺陷,一条是我给 CI 加的 40 分钟

跑完整 e2e(114 个)才出来的,单测和真机 `diff` 都看不见。

**① 覆盖行在非 deep 的 doctor 上也打了。**

`tui_output_contract` 报的是宽度:

```
S1: `xlings self doctor` at 40 cols: 1 line exceeds 40 columns;
    first is 51: '[xlings] deep audit (pass 1): 0 payload(s) examined'
```

宽度只是症状。真正的问题是 **`onAuditDone` 在 quick 模式也触发了** ——
quick 的 root 列表是空的,循环不跑,但回调照样调用,于是每一次普通
`xlings self doctor` 都会宣布一次它根本没被要求做的审计,还报 `0 payload(s)`。
加上 `audit.deep` 门禁。

值得记的是:**我自己那条深度测试抓不到它**。`quick_audited == 0` 这个断言,
无论「打印了 0」还是「压根没打印」都成立。抓到它的是一条完全无关的契约测试
(有没有东西渲染超宽)。两条测试、两个视角,只有那条不相干的看见了。

**② `home_config_lock_test.sh` 从 ~2 分钟变成 2401 秒。**

它持有 state lock,断言四条命令被拒绝。它从来没说过自己愿意等多久 ——
它**隐式继承了产品默认值**。默认值从 30s 改成 10 分钟(为的是让第二条
`install` 等得起一次真实安装,而不是在上面失败),四次拒绝就变成 40 分钟:
全套里最慢的一项,高出第二名一个数量级,而且**每次 CI e2e 都多花 40 分钟**。

钉死 `XLINGS_LOCK_TIMEOUT=2`(正是这次加的那个环境变量)。**2401s → 8.9s。**

这个耦合不是「新出的问题」,是一直就错:这个文件测的是**拒绝**,不是**耐心**,
一个为「盯着终端的人」选的数字,不该决定 CI 静坐多久。钉死之后,
下一次改这个默认值也不会再悄悄多花 38 分钟。

### 9.5 这一轮的账

代码「跑通了」之后才发现的缺陷,一共 **10 条**:

| 抓到的方式 | 条数 | 内容 |
|---|---|---|
| 自我复查 | 6 | `/dev/tty` 还原、无效的空 url 验证、描述不存在设计的注释、`hits()` 零调用者、双重缩进、行尾换行产生空格行 |
| e2e | 4 | 假 ELF 夹具、深度测试的观测手段归零、覆盖行在非 deep 打印、锁测试 40 分钟 |

单测 41/41 全绿、真机输出逐字节相同 —— **这两样一条都抓不到**。
