# 下载失败的归因、安装输出的噪声、依赖声明：生态优化方案

2026-09-29。来源：维护者在真实 home 上跑的三条命令（xlings 2026.9.29.1，索引 `dbc7715`）。

```
xlings install chatgpt                 # 26.924.22138：成功，但被 7-Zip 的输出和 ERROR 刷屏
xlings install chatgpt@26.917.71314    # 失败：sha256 mismatch
xlings info chatgpt                    # runtime deps 35 个，build deps 是完全相同的 35 个
```

涉及四个仓库：mcpplibs/tinyhttps、xlings、libxpkg、xim-pkgindex。

## review 已定

第二轮：R7–R10 全部按建议通过（R7 = b：两个 Qt shim 都 prune；R8 = bump revision；R9 = D3 只提示；R10 = 默认写日志）。

### 第一轮

| 点 | 决定 |
|---|---|
| R1 本地写失败时立即放弃所有候选 URL | ✅ |
| R2 做下载前的空间预检（A4） | ✅ |
| R3 沿用 `E_DISK_FULL`，不新增 wire code | ✅ |
| R4 tinyhttps 修复不回补 0.2.x | ✅ |
| R5 C2（hook 子进程的输出）**本轮一起做** | ✅ → §C2 已写成可以实施的设计 |
| R6 `self update` 不重复打印 `->` 行，同时加 `already at` | ✅ |

本轮新增：§E 依赖声明（第三条命令）、§7 自审，以及第二轮待定的 R7–R10。

---

## 0. 结论

| # | 现象 | 根因 | 严重度 |
|---|---|---|---|
| **A** | `sha256 mismatch … got 68e23f…` | **本机磁盘满**。tinyhttps 不检查 `ofs.write` 的结果，写失败被当成源的完整性问题：URL 被标记 exhausted（剩下的重试都不做），主机也被降级 | **高**：归因错了 |
| **B** | 一个失败打出 3 条 error，其中一条原因为空 | 下载失败的 `Failed` 状态不带原因；后面又报了一次后果 `download artifact missing` | 中 |
| **C** | 7-Zip 的完整输出和 `ERROR: Dangerous link` 混在 `[n/88]` 中间 | hook 启动的子进程直接继承 fd 1/2；recipe 修好了问题却什么都没说 | 中（interface 模式下可能是高，见 C2） |
| **D** | `self update` 打印两遍 `xlings -> …` | 两次 `cmd_use`，第二次状态没变也照常打印 | 低 |
| **E** | runtime / build 依赖都是 35 个 | 见 §E：只有 7zip 是声明错的，qt5 + qt-base 是可选组件带进来的额外负担；**其余 32 个都是直接依赖，不能删** | 中 |

第一次安装是成功的：qt-base payload 的 symlink、prune、marker、RPATH 和 interpreter 我逐项核对过，`configured` 记录也已写入。

---

## 1. A 的证据链（均已实测）

| 步骤 | 事实 |
|---|---|
| recipe 固定的哈希 | `851ec28b…`（xim-pkgindex#889），之后没改过 |
| 上游文件 | `x-ms-creation-time` 09-23，早于 recipe（09-28）；`cache-control: immutable` |
| 重新下载同一个 URL | 420,831,054 字节，sha256 **等于 `851ec28b…`** |
| 用户拿到的 `68e23f…` | **正是正确文件前 220,979,200 字节的 sha256**（= 53950 × 4096，按每 512 字节的前缀逐一比对得出） |
| `df -h /` | 1.5T，**已用 100%** |

### 1.1 失败怎么一路被当成成功，最后算到源头上

```
mcpplibs/tinyhttps download_to_file_impl   (0.2.9 到 0.3.1 都一样)
  ofs.write(buf, n);                 // 不检查
  downloaded += n;                   // 按网络收到的字节计数
  result.bytesWritten = downloaded;  // 实际是“收到了多少”
        │ ok() == true
        ▼
xlings modules/tinyhttps/src/tinyhttps.cpp:238  download_file()
  r.success → onVerify(url)          // 先做 sha 校验，这时还没检查过文件大小
  → "sha256 mismatch"
  → onUrlAttemptFailed → penalize_host(persistent.oaistatic.com)
  → exhausted[i] = true              // 剩下 3 次重试一次都没用
        ▼
xlings downloader.cpp:690  bytesWritten != expectedBytes   // 永远走不到，而且比较的也是假数字
```

受影响的调用方有两个：`xim/downloader.cpp`（所有包的下载）和 `xim/indexfetch.cpp`（索引下载，它的 `onUrlAttemptFailed` 除了 404/401 其他错误都会惩罚主机）。

### 1.2 原则：按“是谁的问题”分三类

| 类别 | 例子 | 该做的事 |
|---|---|---|
| **Source**（源） | 大小完整、内容不对；404 | 换下一个候选；sha 不符时 exhaust 并惩罚主机（保持现状） |
| **Transfer**（传输） | 连接中断、超时、stall、实际字节少于 Content-Length | 可以重试；stall 仍然 exhaust（保持现状） |
| **Local**（本地） | 写失败（ENOSPC/EIO/EACCES）、磁盘上的文件比收到的短 | **立即停止所有候选**（R1），**不惩罚主机**，报 `E_DISK_FULL`（R3），并给出剩余空间 |

和 AGENTS.md 里“could not ≠ empty”是同一类问题：写失败不能当成写成功，本地失败也不能算到源头上。

---

## 2. 工作项

### A1 mcpplibs/tinyhttps：写失败要报出来（上游修复，发 0.3.2）

- 写入回调里，每次 `ofs.write` 之后检查 `ofs`；失败就停止读取，`error = "write <path>: <strerror>"`，并给出可以区分的类型 `ErrorKind::LocalWrite`。
- `ofs.close()` 之后再检查一次，因为缓冲区里的数据是在 flush 时才真正写盘。
- `bytesWritten` 改成实际写入成功的字节数，另加 `bytesReceived` 记网络计数。
- 测试：目标路径设成 `/dev/full`，写入必然 ENOSPC。
- 不回补 0.2.x（R4）。

### A2 xlings 的 xhttp 包装层：自己核对磁盘上的实际大小（P0，不依赖上游）

位置：`modules/tinyhttps/src/tinyhttps.cpp` 的 `download_file`。

1. `DownloadFileResult` 新增 `FailureKind { None, Source, Transfer, Local, Cancelled }`。
2. `download_once` 成功返回以后、调用 `onVerify` **之前**，先取 `onDisk = fs::file_size(destFile)`。tinyhttps 返回前已经 `close()`，所以这时缓冲区已经刷到文件里了：

   | 条件 | 判定 |
   |---|---|
   | `expected` 已知，且 `onDisk == expected` | 继续 → `onVerify` |
   | `expected` 已知，`bytesWritten == expected`，但 `onDisk < expected` | **Local** |
   | `expected` 已知，且 `bytesWritten < expected` | Transfer（可以重试） |
   | `expected` 未知（chunked），且 `onDisk != bytesWritten` | Local |
   | `file_size` 本身失败 | Local（读不到就是读不到，不能当成 0） |
3. 判定为 **Local** 时：删掉 staging 文件，立即返回，不试其他候选，**不调用** `onUrlAttemptFailed`。错误信息里带上 `fs::space(dir).available`：
   ```
   could not write chatgpt_26.917.71314_amd64.deb: 220979200 of 420831054 bytes reached the disk
   (/home/speak/.xlings/data/runtimedir: 0 B free)
   ```
4. `sha256 mismatch` 这个分支只会在大小完整、内容不对时走到，归为 Source，行为不变。
5. tinyhttps 0.3.2 的 LocalWrite 直接映射成 Local。A2 的 stat 检查保留：它核对的是磁盘上的实际结果，成本只有一次 stat。

### A3 downloader 和 indexfetch：沿用分类，把 E_DISK_FULL 传出去

- `DownloadResult` 增加 `errorCode` 和 `hint`；Local → `E_DISK_FULL` 加剩余空间，做法和 #376 的 `extract_wire_error_` 一样。
- `downloader.cpp:690` 改成用磁盘上的实际大小做比较。
- `indexfetch.cpp:80`：遇到 Local 失败直接返回，不再换下一个 base 重试。

### A4 下载前的空间预检（R2）

- 在 `download_once` 的进度回调里，第一次拿到 `total > 0` 时检查一次：如果 `fs::space(dir).available < total - downloaded`，就通过 `isCancelled` 中止，并判为 Local。
- `fs::space` 本身出错时**跳过检查**，读不到空间不等于空间不够。
- 预留 `XLINGS_DOWNLOAD_SPACE_CHECK=off` 作为逃生口：btrfs、网络文件系统报告的可用空间可能不准。

### B 安装报告：一个失败只报一次，而且报根因

位置：`downloader.cpp:915`、`installer.cpp:2726-2742`、`installer.cpp:2933`。

- `onProgress(name, -1)` 只用来画下载面板里那一行 `✗ … failed`，**不再生成 `InstallPhase::Failed` 记录**。
- 结果循环里，每个失败只发**一次** `InstallStatus{Failed, message=r.error, errorCode, hint, planKey}`，不再额外 `log::error`。
- 第二阶段遇到“计划了下载但没有结果”：如果这个 planKey 已经有 Failed 记录，就静默 `continue`；如果没有，说明违反了不变量，报 `E_INTERNAL`。
- 失败后下载总进度条又重画了一次，这个也在这里一并修掉。
- **interface 协议上的变化**：一个失败的包现在恰好对应 1 个 ErrorEvent（以前是 2 个，其中一个 message 为空）。这正是 #374 的本意，release notes 里要写明。

### C1 xim-pkgindex `libs/qtsdk.lua`

- POSIX 分支用 `os.iorun` 捕获 `7zz x -y -bso0 -bsp0` 的输出。修复通过时打一行 `log.info("<tag>: 7-Zip refused N chained .so links; recreated")`；修复失败时把捕获的输出原样打出来，然后返回失败。
- Windows 分支加 `-bso0 -bsp0`。
- **C2 上线之后 C1 仍然需要**：index 同时服务所有版本的客户端，旧客户端没有 C2。
- 不 bump revision，payload 和 config 都没有变化。

### C2 hook 子进程的输出由 xlings 处理（R5：本轮做）

**现状**：`system.exec`、`os.exec`、`system.run_in_script` 以及 recipe 里直接调用的 `os.execute`，**最终都走 Lua 的 `os.execute`**。后者调用 `system()`，子进程继承 xlings 的 fd 1/2。xlings 的任何模式下都没有做重定向（`src/` 里没有 `dup2` 或 `freopen`）。结果是：

- TTY 下：子进程的输出和渲染层抢同一块屏幕，这就是这次的 7-Zip 刷屏。
- **`xlings interface` 下：按代码推断，子进程写到 fd 1 的内容会混进 NDJSON 流**，协议消费方读到的就不是合法的 JSON 行。这是 C2 必须做的主要理由，需要在 T-C2c 里实测确认。

**设计**：

1. **拦截点只有一个**：libxpkg 执行器在 hook 的 Lua 环境里，把 `os.execute` 换成一个 C++ 实现。
   - POSIX：`posix_spawn("/bin/sh", "-c", cmd)`，通过 file actions 把 fd 1/2 指向日志文件的 fd；stdin 继承不变。
   - Windows：`CreateProcessW("cmd.exe /c " + cmd)`，`STARTUPINFO.hStdOutput/hStdError` 指向日志文件的句柄。
   - **不改写命令字符串**。字符串拼接重定向会碰到 msvc.lua 记录过的 cmd 引号问题。
   - 返回值和 Lua 5.4 的 `os.execute` 完全一致：`true|nil, "exit"|"signal", code`。现有调用方 `ret == 0 or ret == true` 的判断不受影响。
2. **输出去向**由 `ExecutionContext.hook_output` 决定，xlings 负责填写：
   - 默认：`<XLINGS_HOME>/logs/hooks/<ns>-<name>@<ver>.log`，每次 hook 执行前清空，开头写一行标明 hook 名、时间和 xlings 版本。
   - `XLINGS_HOOK_OUTPUT=inherit`（或 `-v`）：恢复直通终端，便于 CI 调试。
   - interface 模式：**必须写日志**，忽略 `inherit`。
3. **交互**：`sudo` 从 `/dev/tty` 读密码，不走 stdout/stdin，重定向不影响它（index 里有 10 个 recipe 用了 sudo，已经清点过）。stdin 保持继承。真正需要把 stdout 显示给用户的命令，显式写 `system.exec(cmd, { tty = true })`。旧客户端会忽略这个选项，行为不变。
4. **只重定向子进程**：hook 自己的 `print` / `io.write` / `log.*` 保持现状。index 里有 2 个 recipe 用 `io.read` 做提示，它们的提示语要能显示出来。
5. **失败时**：hook 失败后，xlings 打出日志的最后 20 行和日志路径，放在 `InstallStatus.hint` 里。成功时什么都不打印。
6. **让用户知道还在运行**：渲染层在当前节点那一行显示 `installing xim:qt-base … 12s`，并每 500ms 读一次日志的最后一行作为灰色尾注。非 TTY 下每 60 秒输出一行心跳，否则从源码编译 gcc 这种三十分钟的 hook 看上去就像卡死了。
7. **日志打不开时**（比如又是磁盘满）：退回 `/dev/null`（Windows 是 `NUL`），并用 `log::warn` 说明一次。**不能因为日志写不了就让 hook 失败**。
8. `io.popen`（`os.iorun`）不改：它本来就捕获 stdout，stderr 已经重定向到 `/dev/null`。

**发布链**：libxpkg 发新版本（0.0.60）→ mcpp-index 收录这个 xpkg 版本 → xlings 在 `mcpp.toml` 里 bump `xpkg`。这是 memory 里 [[xpkg-bump-needs-mcpp-floor]] 记录过的顺序：新的 xpkg 必须先进 mcpp 的索引，否则 xlings 构建时找不到它。

### D `self update` 的重复输出（R6）

- `cmd_use` 在计划 from == to（版本和 provider 都相同）时，把 `{target} -> {version}` 降到 `debug`；发生切换时格式不变。
- `self update` 在 install 子进程报 `is in the store` 且版本没变时，最后打印一行 `xlings is already at <ver> (latest)`。
- 影响范围：tests 里没有任何地方 grep `xlings -> `，已核对。

### E 依赖声明（`xlings info chatgpt`）

#### E 的实测

对 chatgpt 26.924.22138 的 payload **按 ELF magic 扫描了全部 45 个 ELF**（按扩展名和执行权限筛会漏掉 22 个 `.node`），读取 DT_NEEDED，再把每个 soname 对应到提供它的包。另外在 `ChatGPT` 主程序的字符串里找 dlopen 的库名。然后用 `origin/main` 上的 recipe 计算传递闭包：结果是 88 个包，和安装时的 `[n/88]` 一致，说明计算方法可靠。

| 声明的 35 个依赖 | 数量 | 依据 |
|---|---|---|
| 直接 DT_NEEDED | 31 | 30 个来自 `ChatGPT` 主程序和原生模块；`libusb` 来自 node-hid 的 linux-x64 glibc 预编译模块 `HID-linux-x64/node-napi-v4.node` |
| dlopen | 3 | `libsecret`、`libnotify`（二进制里有库名字符串）；`graphics`（GL/EGL/Vulkan 的发现机制） |
| 只在安装时用 | 1 | `7zip`：只有 install() 调用 `7zz` |

**结论：没有一个是“间接依赖”，不能删。** 原因有两条：
- `dep-closure-check.sh` 的 D1：payload 自己引用的每个 soname，都必须由**直接**依赖提供。
- elfpatch 写进 RPATH 的**只有直接 runtime 依赖的 libdirs**（`closure_lib_paths` 只读 `runtime_deps_list`）。如果某个包只能经由传递关系到达，而 app 又 dlopen 它，那一定找不到。

“88 个”指的是传递闭包，是各个库各自依赖的总和，并不是 chatgpt 多声明了。

真正可以优化的是下面三件事。

#### E1 `build deps` 那一行重复：显示问题（xlings）

- recipe 用的是旧的平铺写法 `deps = {...}`，libxpkg 会把它同时复制成 runtime 和 build 两份（`xpkg-loader.cppm:332`）。`commands.cpp:2401` 的注释说“避免重复打印”，但实际上把两份相同的列表都打印了。
- 改法：两份列表**相同**时只显示一行 `deps`；只有 recipe 真的分开声明了 runtime/build，才分两行显示。

#### E2 7zip 应该是 build 依赖（xim-pkgindex）

- 现状：chatgpt、qt、qt-base、qt-addons、qt5 都把 `xim:7zip` 写在 runtime 里，所以装其中任何一个，都会**把 `7z` / `7zz` 激活到用户的 PATH 上**（已在真实 home 的 `subos/default/bin` 里确认）。
- 改法：
  ```lua
  deps = { runtime = { ...原来除 7zip 以外的 34 个... }, build = { "xim:7zip@26.02" } }
  ```
  install() 里取 7zz 的路径，从 `pkginfo.dep_install_dir("xim:7zip")` 改成 `pkginfo.build_dep("7zip").path`。
- **兼容性**：`build_dep` 和 xlings 注入 `XLINGS_BUILDDEP_<NAME>_PATH` 都是 2026-05-01（#249）加的，现在所有在用的客户端都支持。
- **要注意的缺陷（E2′）**：xlings 生成环境变量名时会**去掉命名空间**（`XLINGS_BUILDDEP_7ZIP_PATH`），但 libxpkg 的 `build_dep("xim:7zip")` 查的是 `XLINGS_BUILDDEP_XIM_7ZIP_PATH`，查不到后退回 `dep_install_dir`；而 `resolved_deps` 只记录 runtime 依赖（`installer.cpp:2858`），最后返回 nil。所以 dpcpp.lua 才会先试带命名空间的名字、失败后再试裸名。**recipe 这一侧用裸名 `"7zip"`，在所有客户端上都能用。** 根治放在 libxpkg：`build_dep` 在拼环境变量名之前，先去掉命名空间和版本，和 xlings 保持一致。
- 影响范围：chatgpt、qt、qt-base、qt-addons、qt5，这五个 recipe 同一个 PR。android-system-image 和 wix 另外核对。

#### E3 qt5 + qt-base 只被可选的 Qt UI shim 用到（xim-pkgindex，需要决定，R7）

- `app/libqt5_shim.so` 和 `app/libqt6_shim.so` 是 Chromium 的 Qt UI 集成。`ChatGPT` 按 `libqt%d_shim.so` 的名字 dlopen 它们，版本由 `--qt-version` 或 `KDE_SESSION_VERSION` 决定，**只在 KDE 下或者显式传 `--ui-toolkit=qt` 时才会加载**。其他桌面环境一律走 GTK，而 gtk3 本来就是 `ChatGPT` 的 DT_NEEDED。
- 为了这两个 shim，闭包里多出 10 个包：`qt5 qt-base brotli zstd xcb-util xcb-util-cursor xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm`。磁盘占用 **约 620 MB**（qt5 315M + qt-base 305M），而 chatgpt 自身的 payload 是 1.5G。第一次安装里 qt-base 那 5 个 7z 归档，也全是因为这两个 shim 才下载的。
- 三个选项：

  | 选项 | 做法 | 收益 | 代价 |
  |---|---|---|---|
  | a 保持现状 | — | KDE 下是原生 Qt 对话框和主题 | 每个用户多 620 MB、10 个包 |
  | **b 两个 shim 都 prune** | install() 删掉这两个 `.so`，deps 去掉 qt5、qt-base | 闭包 88 → 78，−620 MB | KDE 用户改用 GTK 外观 |
  | c 只保留 qt6 shim | 删 `libqt5_shim.so`，deps 去掉 qt5 | −315 MB；Plasma 6 仍是 Qt 外观 | Plasma 5 回退到 GTK |
- **建议选 b**，但有一个前提需要实测：删掉 shim 以后，在 `XDG_CURRENT_DESKTOP=KDE KDE_SESSION_VERSION=6` 下启动，以及显式传 `--ui-toolkit=qt` 启动，app 都要**回退到 GTK 而不是崩溃**。按 Chromium `linux_ui_factory` 的逻辑，Qt UI 创建失败应该回退，但我还没有在这个 app 上跑过。实测不通过就改选 c 或 a。
- 已有 payload 怎么处理（R8）：只 prune、不 bump revision 的话，已安装的 payload 保持原样，照常能用，但 qt5/qt-base 仍被占着、GC 回收不了。bump `26.924.22138` 到 revision 1，下次 install 会把 payload 换成 prune 后的版本，qt 两个包就能被回收；代价是这次要重新下载 400 MB（如果 `runtimedir` 里的 deb 缓存还在就不用）。

#### E4 index 工具：提示“声明了但没用到”（R9，建议做成非致命）

- 在 `dep-closure-check.sh` 里加一个 **D3 advisory**：某个声明的 runtime 依赖，如果它提供的 soname 没有出现在 payload 的 DT_NEEDED 或 ELF 字符串里，**并且它没有提供任何库**（纯工具包，比如 7zip、patchelf、cmake），就提示“应该改成 build 依赖”。
- 只提示、不失败：dlopen 的库名可能是运行时拼出来的（`libqt%d_shim.so` 就是例子），`graphics` 这类负责发现机制的包也不提供被 NEEDED 的 soname，静态扫描都看不到。对 chatgpt 实测，它只会指出 7zip，没有误报。

---

## 3. 测试

| 项 | 测试 | 形式 |
|---|---|---|
| A1 | `/dev/full` → LocalWrite，`bytesWritten == 0` | tinyhttps 单测 |
| A2 | `transferOverride` 只写一半、却报 `bytesWritten == expected` → Local；`onVerify`、第二个候选 URL、`onUrlAttemptFailed` **都没有被调用** | xlings 单测 |
| A2 | 大小完整、内容错误 → 仍然 exhaust 并惩罚主机（不回退） | xlings 单测 |
| A2 | `bytesWritten < expected` → Transfer，下一轮会重试 | xlings 单测 |
| A3 | Local → `E_DISK_FULL`，hint 里有剩余空间 | xlings 单测 |
| A4 | 注入 `space()` 返回较小值 → 传第一个字节前就中止；`space()` 出错 → 不拦 | xlings 单测（加一个测试接口） |
| B | 一个下载失败 → interface 流里恰好 1 个 ErrorEvent，message 是根因，没有 `download artifact missing` | xlings 单测 |
| C1 | 装 qt-base：stdout 没有 7-Zip 的横幅；只有一行 `recreated`；`libQt6DBus.so` 是 symlink | pkgindex CI |
| C2a | fixture 的 hook 执行 `echo OUT; echo ERR >&2` → 终端上什么都没有，日志里两行都有 | xlings e2e |
| C2b | 同一个 hook 再 `exit 3` → 失败，hint 里有 `OUT`、`ERR` 和日志路径 | xlings e2e |
| **C2c** | 用 `xlings interface` 执行 C2a → **每一行都是合法 JSON**。先在当前版本上跑一次，确认“现在会污染 NDJSON”这个推断 | xlings e2e |
| C2d | `system.exec(cmd, {tty=true})` → 输出直通终端；`XLINGS_HOOK_OUTPUT=inherit` 同样直通 | xlings e2e |
| C2e | `os.execute` 的返回值三元组和原生一致（exit 0、exit 3、被信号杀掉三种情况） | libxpkg 单测 |
| D | 已经是最新时 `self update`：没有 `->` 行，有一行 `already at` | xlings e2e |
| E1 | 平铺写法 → info 只显示一行 `deps`；分开声明 → 分两行 | xlings 单测 |
| E2 | chatgpt 和 qt-base 装好后，`subos/<s>/bin` 里**不新增** `7z`、`7zz`；安装本身成功 | pkgindex CI（隔离 home） |
| E2′ | `build_dep("xim:7zip")` 和 `build_dep("7zip")` 返回同一个路径 | libxpkg 单测 |
| E3 | prune 后 D1 通过；KDE 环境变量下和 `--ui-toolkit=qt` 下都能启动、回退到 GTK | 手动实测（GUI） |

e2e 里**不做真实的 ENOSPC**：需要 root 去挂 loop/tmpfs；`ulimit -f` 又会先触发 SIGXFSZ 把进程杀掉，测不到要测的路径。改用 `transferOverride` 和 `/dev/full` 覆盖。

---

## 4. 发布顺序

1. **libxpkg 0.0.60**：C2（`os.execute` 拦截、`hook_output`、`{tty=true}`）和 E2′（`build_dep` 名字规范化）。
2. **mcpp-index**：收录 xpkg 0.0.60，否则下一步 xlings 构建时找不到它。
3. **xlings**：A2、A3、A4、B、D、E1，加上 C2 在 xlings 一侧的部分（日志路径、失败时的尾部输出、渲染层的尾注和心跳），并 bump xpkg。版本号取当天的下一个 `.N`。
4. **xim-pkgindex**：C1 和 E2 可以立即合入，不依赖第 1–3 步，因为 `build_dep("7zip")` 在现有客户端上就能用。E3 等 R7/R8 定了、实测通过以后再做。E4 单独提。
5. **mcpplibs/tinyhttps 0.3.2**：A1。xlings 升级到 0.3.x 时一起带上。

---

## 5. 需要 review 决定的点（第二轮）

| # | 问题 | 建议 |
|---|---|---|
| R7 | chatgpt 的 Qt shim：a 保持 / b 全部 prune / c 只留 qt6 | **b**，前提是 GTK 回退实测通过，否则 c |
| R8 | E3 之后是否 bump chatgpt 的 revision，让已有 payload 被替换、qt 能被 GC 回收 | bump。只是 prune 的话，已有用户那 620 MB 永远回收不了 |
| R9 | E4 的 D3 提示放进 index CI 吗 | 放进去，只提示、不失败 |
| R10 | C2 在 TTY 下的默认值：写日志，还是直通终端 | 写日志。直通就是这次刷屏的原因；需要看输出时用 `-v` 或 `XLINGS_HOOK_OUTPUT=inherit` |

---

## 6. 附：本机状态

- `/` 1.5T，已用 100%，剩约 3.3G。重试 `chatgpt@26.917.71314` 前至少要清出 1 GB。
- 本地 `xim-pkgindex` 工作区在 `40290f92`，比 `origin/main`（`dbc7715`）落后 25 个提交。本文引用的 recipe 都以 `origin/main` 为准。

---

## 7. 自审

逐条检查本方案，记录发现的问题以及在方案里怎么处理的：

| # | 检查 | 发现 | 处理 |
|---|---|---|---|
| S1 | A2 的 stat 检查时，数据是否已经刷盘 | tinyhttps 返回前已经 `ofs.close()`，缓冲区已刷，stat 得到的是最终大小 | 在 A2 第 2 条里写明 |
| S2 | A4 会不会误拦 | `fs::space` 在 btrfs、网络文件系统上可能不准；出错时的默认处理必须是放行 | A4 写明出错时跳过检查，并加 `off` 逃生口 |
| S3 | A2 的 Local 判定会不会把真实的传输截断误判为本地失败 | 不会。区分依据是 `bytesWritten` 和 `onDisk` 两个数：网络少收了是 Transfer，收全了但盘上短才是 Local | 判定表里两行分开 |
| S4 | B 改变了 interface 协议的输出 | 一个失败从 2 个 ErrorEvent 变成 1 个；消费方如果按个数计数会受影响 | B 里写明，release notes 要说明 |
| S5 | C2 会不会弄坏 sudo 或交互式 recipe | sudo 从 `/dev/tty` 读密码，不受影响；`io.read` 类 recipe 的提示走的是 hook 自己的 `print`，而 C2 不重定向 `print` | C2 第 3、4 条 |
| S6 | C2 会不会让长时间构建看上去像卡死 | 会，如果什么都不显示 | C2 第 6 条：尾注加心跳 |
| S7 | C2 在磁盘满时会不会让 hook 失败，重演 A 的问题 | 日志打不开就退回 `/dev/null` 并警告一次 | C2 第 7 条 |
| S8 | C2 的拦截能不能覆盖所有执行路径 | `system.exec`、`os.exec`、`run_in_script`、直接 `os.execute` 都汇到 `os.execute`；`io.popen` 本来就捕获输出 | C2 第 1、8 条 |
| S9 | C1 在 C2 之后是否就多余了 | 不多余：index 同时服务旧客户端 | C1 写明 |
| S10 | E2 改成 build 依赖后，旧客户端能不能拿到 7zz | `dep_install_dir` 拿不到（`resolved_deps` 只记 runtime）；`build_dep("xim:7zip")` 因为环境变量名不一致也拿不到；**只有裸名 `build_dep("7zip")` 能用** | E2 用裸名；E2′ 在 libxpkg 修名字规范化 |
| S11 | E2 会不会把 7zip 带进 RPATH | 不会：`closure_lib_paths` 只读 `runtime_deps_list` | 已核对 elfpatch.lua:878 |
| S12 | E3 的收益估计会不会偏高 | 620 MB 是 qt5 + qt-base 两个 payload 的实际 `du`；另外 8 个独占包一共不到 5 MB；gtk3 等共享包本来就在闭包里 | E3 表格用的是实测值 |
| S13 | E3 的“回退到 GTK”只是推断 | 是推断，还没有在这个 app 上实测 | 列为 E3 的前提和 R7 的条件 |
| S14 | 方案有没有越界到用户没问的东西 | E4 和 A4 是新增的，但都有数据支撑；E4 做成非致命提示 | 分别列为 R9 和 R2（R2 已通过） |
| S15 | 有没有“断言版本号”式的测试 | 没有：测试断言的都是行为（事件个数、文件是否存在、返回值三元组） | — |

---

## 8. 实施：任务拆分与依赖（R1–R10 均已通过）

### 8.1 C2 细化后的接口（实现以此为准，替代 §2 C2 第 1、2 条的部分细节）

实现前读了 libxpkg 执行器，发现两件事，设计据此简化：

- hook 自己的 `print` / `io.write` / `io.stderr:write` **已经**被 `HookCapture` 捕获进 `HookResult.output`（16 KB 尾部），失败时由 xlings 的 `format_hook_failure` 打印。**只有子进程在直通终端。** 所以 C2 不必另起一套“日志 + 尾部”机制：让子进程的输出进入同一个文件，`HookResult.output` 取这个文件的尾部，失败时的打印路径不变。
- libxpkg 现有的 `os.iorun` 用的是“在命令字符串末尾拼接重定向”。这种写法对 `a && b` 只会重定向 `b`；在 Windows 的 cmd 上，在开头加任何字符都会改变 `/c` 的去引号规则，而 msvc.lua 正是依赖这个规则的。所以 C2 **不改写命令字符串**，改在进程层面接管 fd。

接口约定：

| 位置 | 内容 |
|---|---|
| `ExecutionContext::hook_log`（新增 `fs::path`） | 为空：行为和 0.0.59 **完全一致**（子进程继承 fd 1/2，`print` 进环形缓冲）。其他使用 libxpkg 的程序不受影响 |
| `run_hook` 且 `hook_log` 非空 | 清空并打开这个文件，写一行 `# <hook> hook of <pkg>@<ver>`；`print`、`io.write`、`io.stderr:write` 在进环形缓冲的**同时**追加写进这个文件；hook 里所有的 `os.execute`（`system.exec`、`os.exec`、`run_in_script`、recipe 直接调用的都走这里）以 fd 1/2 = 这个文件的方式启动：POSIX 用 `posix_spawn("/bin/sh","-c",cmd)` + `adddup2`；Windows 用 `CreateProcessA(COMSPEC, "<COMSPEC> /c <cmd>")` + `STARTF_USESTDHANDLES`，命令行与 UCRT `system()` 构造的一致。hook 结束时 `HookResult.output` = 文件尾部 16 KB（沿用 UTF-8 清洗和截断标记） |
| `os.execute` 的返回值 | 与 Lua 5.4 原生一致：`true\|nil, "exit"\|"signal", code` |
| 文件打不开 | 子进程的 fd 1/2 指向 `/dev/null`（`NUL`），`log.warn` 一次，hook 照常执行 |
| `system.exec(cmd, { tty = true })`、`system.run_in_script(c, true)` | 走原来的 `std::system`，直通终端。旧客户端会忽略 `tty`，行为本来就是直通 |
| `run_hook` 以外（`run_script`、`apply_elfpatch_auto`） | 不接管，保持原样 |

xlings 这一侧：
- 每次 `run_hook` 之前设置 `ctx.hook_log = <XLINGS_HOME>/logs/hooks/<ns>-<name>@<ver>.<hook>.log`。
- `XLINGS_HOOK_OUTPUT=inherit` 或 `-v` 时留空；interface 模式下始终设置，忽略 `inherit`。
- 失败信息 = `<hook> hook failed: <error>` + 输出的最后 20 行 + `full log: <path>`。
- **运行状态提示不重绘光标**：hook 运行满 15 秒打一行，之后每 60 秒一行：`  … <coord> install hook running 2m — <日志最后一行>`。TTY 和非 TTY 一样，不会和其他日志行抢同一块屏幕。interface 模式下发 `progress{phase:"hook"}`，协议升到 1.5（只新增，不改已有字段）。

### 8.2 仓库、PR 与依赖

```
T  mcpplibs/tinyhttps      A1 ─────────────────────────────── tag 0.3.2 ─┐
L  openxlings/libxpkg      C2(libxpkg) + E2′ ── tag 0.0.60 ── gtc ────────┤
M  mcpplibs/mcpp-index                        xpkg 0.0.60 (+ tinyhttps 0.3.2)
X  openxlings/xlings       A2 A3 A4 B D E1 C2(xlings) ── bump xpkg 0.0.60 + 版本号 ── CI ── release ── gtc 补 GitCode
I  openxlings/xim-pkgindex C1 E2 E3(+revision) E4 + 文档 ──────── 可以先合（只依赖现有客户端）
                           发布后：xlings latest bump PR
```

| 任务 | 依赖 | 并行 | 负责 |
|---|---|---|---|
| T：A1 + `/dev/full` 测试 + 0.3.2 | 无 | ✅ | agent |
| L：C2（8.1）+ E2′ + 测试 + 0.0.60 | 无 | ✅ | agent |
| I：C1 E2 E3 E4 + contributing 文档 | 无（`build_dep("7zip")` 在现有客户端上能用） | ✅ | agent |
| X-core：A2 A3 A4 B D E1 + 单测 | 无 | ✅ | 我 |
| X-c2：C2 的 xlings 一侧 + e2e | L 的接口（先用本地 seed 的 xpkg 构建） | L 开发期间 | 我 |
| M：mcpp-index 收录 xpkg 0.0.60 | L 合并并打 tag，gtc 上传 `mcpp-res/xpkg` | — | 我 |
| X 的 CI | M 的 artifact 发布 | — | 我 |
| 发布与生态验证 | X 合并 | — | 我 |

### 8.3 多角度检查（实施时逐项对照）

| 角度 | 要求 |
|---|---|
| 架构 | 每个问题只由一个地方回答：Local/Source/Transfer 的判定只在 xhttp 包装层做；hook 输出的去向只由 `ExecutionContext::hook_log` 决定；7zip 的路径只从 `build_dep` 取 |
| 稳定性 | 新增的逃生口都默认安全：`space()` 出错放行；日志打不开退回 `/dev/null`；`hook_log` 为空时行为完全不变 |
| 简洁 | C2 复用已有的 `HookResult.output` 通路，不新增 xlings 侧的尾部读取器 |
| 用户体验 | 一个失败只有一条原因；磁盘满时直接给出剩余空间；安装不再刷屏；长时间的 hook 有心跳 |
| 兼容性 | libxpkg 新字段默认关闭；index 只用现有客户端就支持的 API（裸名 `build_dep`）；interface 只新增一个 phase |
| 跨平台 | 新的 spawn 代码 POSIX 和 Windows 各一份；Windows 命令行和 `system()` 一致；macOS 与 Linux 共用 POSIX 分支；用 `llvm@20.1.7` 在 Linux 上预跑 libc++ 编译 |
| 一致性 | 错误码沿用 `E_DISK_FULL`；日志目录沿用 `<XLINGS_HOME>/logs/`；新测试放在现有 E2E 编号之后 |
| 无感升级 | 不需要用户做任何操作；已有的 chatgpt payload 因 revision 1 在下次安装时被替换，qt5/qt-base 随后可以被 GC 回收 |

### 8.4 实施中推翻的判断

| 原判断 | 实测 | 影响 |
|---|---|---|
| C2：interface 模式下子进程的 stdout 会混进 NDJSON（§2 C2 “按代码推断”） | 不会。`src/interface.cpp` 在每个能力外面都装了 `platform::StdoutCapture`（2026.9.28.1），fd 1 上的杂散输出被转发到 stderr，每行前缀 `[stray stdout]`；子进程的 stderr 原样到达 stderr | C2 在 interface 下的收益是：这些输出进 hook 日志、失败时带上尾部，而不是散在 stderr 上。协议文档 1.5 的说明按此改写，§5 的保证原本就成立 |
| D：“目标已激活”就等于 `use` 什么都没动 | 不对。一个 release 的入口已激活、某个成员没激活时（release 新增了程序），`use` 会移动那个成员 | `alreadyActive` 要求整个 release 的每个成员都已在目标版本，单测 `AlreadyActiveOnlyWhenTheWholeReleaseIsInPlace` 覆盖三种情况 |
| B：下载失败的状态可以交给转发包装补 planKey | 不行。下载发生在逐节点循环之前，那时包装里的 `currentPlanKey` 为空，失败不会进入 outcome 表 | 失败状态显式设置 `planKey`（下载任务就是以 planKey 命名的） |
| 版本号 2026.9.29.2 | 实施到 23:05，发布必然跨零点 | 版本号用 2026.9.30.1 |

## 9. 发布后的真实验证（计划）

在真实 home 上执行（`xlings config --mirror CN`），每次经过沙箱前先确认 `xlings --version`：

| # | 命令 | 期望 |
|---|---|---|
| V1 | `xlings self update`（由 2026.9.29.1 执行） | 升到 2026.9.30.1，exit 0 |
| V2 | 再执行一次 `xlings self update` | 最后一行是 `xlings is already at 2026.9.30.1 (latest)`；没有 `xlings -> …` 行 |
| V3 | `xlings subos new verify-0930`，然后 `xlings subos use verify-0930 --sandbox --cmd "xlings --version"` | 2026.9.30.1 |
| V4 | 沙箱里 `xlings install chatgpt -y` | 26.924.22138 revision 1：payload 被替换，闭包 78 个节点、没有 qt5/qt-base；输出里没有 7-Zip 的横幅；`app/libqt*_shim.so` 不存在 |
| V5 | 沙箱里 `xlings install chatgpt@26.917.71314 -y` | 用户最初失败的那条命令，在有空间时成功，sha256 与 recipe 一致 |
| V6 | 沙箱里 `xlings install qt-base -y`（新 scope） | 只 configure；没有 7z/7zz 被激活到这个 subos 的 bin |
| V7 | `xlings info chatgpt` | runtime deps 34 个，build deps 只有 `xim:7zip@26.02` |
| V8 | `ls <home>/logs/hooks/` | 本轮安装过的 hook 各有一个日志 |
