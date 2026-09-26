# #615 解决方案：让 shim 在任何平台上都跟随 entry binary

- 关联：#615（问题分析见 `2026-09-26-issue615-windows-stale-hardlink-shims-analysis.md`）、#613、mcpp#693、#580（2026.9.3.1 路由表）、#542（entry binary 唯一写者）、#473
- 基线：`v2026.9.26.2`
- 状态：设计草案，第二轮。2026-09-26 已经确定：一次发布、只做 B1、doctor 报 Error、COMPAT 到 2027.3、POSIX 用魔数扫描。第 11 节的自我 review 结论已经合入正文。本文不含代码改动

---

## 0. 一句话

**现在对 shim 的两个判断——"这个文件是不是我们的"和"它是不是最新的"——用的都是"它和 entry binary 是不是同一个文件对象"。** Windows 上每次升级都会刻意换掉这个文件对象，所以一升级，两个判断同时答错。POSIX 的 shim 是指向路径的符号链接，这两个判断碰巧都答对。

解决方案是把这两个判断拆开，各自找一个不会被升级改变的依据：

| 判断 | 现在 | 改为 |
|---|---|---|
| 是不是我们的 | 和 entry 是同一个文件对象 | **文件本身是一个 xlings 多调用 binary**（内嵌标记；老 binary 用哈希识别） |
| 是不是最新的 | 同上 | 和 entry 是同一个文件对象，**或内容相同** |

判断改对之后，"升级后 shim 自动跟上"就可以分三层来实现：**收敛**（替换 entry 的地方负责把 shim 重新链接好）、**运行期保证**（旧 shim 在启动时发现自己不是 entry，就把执行交给 entry）、**过渡**（已经被旧客户端弄坏的 home 怎么回来）。

---

## 1. 约束（review 请按这些检查）

1. **跨平台对等**：Windows 的行为要等价于 POSIX 的符号链接语义，也就是"执行 shim 就是执行 entry 的代码"。POSIX 上的行为和性能一个字节都不能变。
2. **用户数据**：只有能证明是 xlings 的文件才能动（AGENTS.md "SubOS user data"：xlings 不能证明自己拥有的文件属于用户数据）。**读不出来的文件不能当成旧文件处理**，读失败就归为 Unknown，不碰它，只报告。
3. **单一写者**：路由表（名字集合）的唯一写者仍然是 `sync_shim_tables`；entry 的唯一写者仍然是 `entry_binary::replace_with`。不允许出现第二个写者。
4. **只读命令不写**：`--version`、`info`、`self doctor`（不带 `--fix`）、shim 的分发过程都不能修改 home。
5. **无感升级**：之后的每一次升级、降级、再升级，用户都不需要做任何事。已经坏掉的 home 最多只需要一个明确、能直接执行的步骤。
6. **分发热路径零开销**：`clang++` 这类会被 ninja 调用上千次的 shim，正常状态下不能多一次进程创建，也不能多一次读整个文件。
7. **一个问题只有一个回答者**：现在"这个文件是不是我们的 shim"有三个回答者——`shim_table.cpp: is_our_shim_`、`shim.cpp: is_own_shim_`（passthrough）、`compat::is_legacy_alias_symlink_to_bootstrap`。改完之后只能剩一个。

---

## 2. 候选方案对比

| 方案 | 做法 | 优点 | 否决或降级的理由 |
|---|---|---|---|
| A. 替换后按硬链接关系重建（issue 里的建议） | `replace_with` 在改名前用 `FindFirstFileNameW` 列出旧 entry 的所有硬链接，替换之后逐个重建 | 改动小 | 只认硬链接，**覆盖不到 copy 回退产生的 shim**（跨卷时就是 copy）；靠"记住以前的关系"，一旦中途失败就没有依据可以恢复；**已经坏掉的 home 用不上**，因为它们和 entry 的硬链接关系已经断了。作为第 2 层的一种实现可以借鉴，但不能单独使用 |
| B. 固定的小启动器（Scoop shim.exe 的思路） | shim 是一个永远不变的小 exe，每次启动转交给 `home\bin\xlings.exe` | 升级不再替换 shim | **每次执行工具都多一次进程创建**（违反约束 6）；多了一个制品；启动器本身需要更新时又回到同一个问题；和 POSIX 的模型分叉 |
| C. Windows 上能用符号链接就用 | 开发者模式或管理员权限下创建符号链接 | 语义和 POSIX 相同 | 大部分用户没有这个权限，结果是同一个版本在两类用户那里行为不同；仍然需要处理硬链接的情况 |
| **D. 按内容识别 + 收敛 + 运行期转交（本文推荐）** | 见第 3 节 | 同时覆盖硬链接、copy、符号链接和已经坏掉的 home；热路径只多一次文件比对；POSIX 上什么都不会发生 | 改动面比 A 大，一次发布全部交付（已确定） |

---

## 3. 方案

### 第 1 层：身份——`xvm::shim_identity::classify(path, entry)` 作为唯一回答者

```
enum class ShimState { Current, Stale, Foreign, Unknown };
struct ShimIdentity {
    ShimState state;
    bool handoffCapable;      // 这个 binary 是否带第 3 层的转交代码
    std::string buildVersion; // 能读到就填，用于报告
};
```

判定顺序：先做便宜的判断，只有前面答不出来才读文件内容。

1. **符号链接**：目标解析为 entry → Current；悬空但指向 entry 的路径 → Current（保留现有逻辑）；指向别处 → Foreign。
2. **和 entry 是同一个文件对象**（`fs::equivalent`）→ Current。这是正常状态的判定路径，成本和现在一样。
3. **不是同一个对象时，看它是不是 xlings binary**：
   - **标记（从修复版开始的所有构建都带）**：
     - Windows：在 `res/xlings.rc` 里加一个 RCDATA 资源 `XLINGS_MULTICALL`，内容是 `abi=1;caps=handoff;version=<v>`。读取方式是 `LoadLibraryExW(..., LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)` 加 `FindResourceW`，只映射文件，不读全文。#613 已经把 `[resources]` 接进了 mcpp 构建，这里只是在同一个 rc 里多加一行。
     - POSIX：在 `.rodata` 放一个固定的魔数字符串，在文件里扫描它。POSIX 上 shim 正常是符号链接，只有 copy 或硬链接回退时才会走到这一步，很少发生。
   - **老 binary（修复版之前的构建，没有标记）**：放在 `xself::compat::v2026_9_x` 里，按仓库惯例写上 `COMPAT(… → drop in …)` 注明到期时间。判定条件是：哈希属于集合 K，或者和已识别的文件是同一个文件对象（同一组硬链接）。K 的来源：
     - `data/xpkgs/*-x-xlings/*/bin/xlings[.exe]`（本机装过的所有 xlings 构建）；
     - 每个 subos 里保留名 `xlings[.exe]` 的哈希。这个名字由 `ensure_subos_shims` 放置，按构造一定是 xlings；即使旧 payload 已经被 GC 清掉，也能靠它识别。
   - 按文件对象分组，每组只算一次哈希。一个 home 里通常只有 1 到 3 个不同的文件对象，每个 10 到 40 MB。只在写者路径和 doctor 里计算，分发时从不计算。
   - **先比大小，再算哈希**（自我 review R1）：K 里每个构建的大小只需要 stat 一次；候选文件的大小不在这些大小里，就直接判为 Foreign，不计算哈希。否则 bin 里只要有一个用户自己放的真实程序，每次 install/use 都要把所有历史 payload 哈希一遍：一个 home 里有 10 个以上的 xlings 版本，每个 20 MB，就是 200 MB 以上。
   - **K 必须在任何写操作之前构建**（R2）：`self init` 里 `ensure_subos_shims` 会先把保留名 `xlings.exe` 重新链接到新 entry，这样就丢掉了"旧哈希"这个依据。如果旧 payload 也已经被 GC 清掉，剩下的旧 shim 就认不出来了。所以 classify 的扫描和 K 的构建放在 `ensure_home_layout` 最前面，`*.xlings.old*` 这类改名留下的文件也算进 K 的依据。
   - **POSIX 扫描的上限**（R3，对应决定 5 的性能风险）：大于 256 MB 的文件直接判为 Foreign（xlings binary 不会这么大，所以这是可以证明的上限，不是一个阈值判断）；用 mmap 加 memmem 扫描；同一进程内按 `(dev, ino, size, mtime)` 缓存结果。
4. 识别为 xlings binary 时：内容和 entry 相同 → Current（跨卷 copy 回退的情况，避免每次 sync 都重复处理）；内容不同 → **Stale**。
5. 不是 xlings binary → Foreign（用户自己的真实程序，保持现在的处理：不覆盖、只报告）。
6. **任何一步读失败**（权限、资源读取出错、IO 错误）→ **Unknown**：不碰这个文件，并报告。不能读就不能判定为 Stale（约束 2）。

**改为使用它的地方（约束 7）**：`scan_actual`、doctor 的 shim 检查、`find_host_passthrough_` 的 `is_own_shim_`（带标记的文件永远不会被当成 host 程序，其他 home 里的旧 shim 也不会被误执行）、compat 里的 alias 清理。

### 第 2 层：收敛——"重新指向"，而不是"重建"

在 `plan_table` 里增加 `toRepoint`：

- Stale 并且名字属于 desired 或保留名 → `toRepoint`：通过 `create_shim` 重新链接到当前 entry（先 displace 再链接；即使旧文件正在运行也能改名）。
- Stale 但名字不属于 desired → `toRemove`。它已经被证明是我们的，所以可以删。
- Foreign 和 Unknown → 保持现在的处理，不碰。

**重新指向和重建是两种操作，范围也不同**：

| 操作 | 会不会改变名字集合 | 范围 | 理由 |
|---|---|---|---|
| 重建（`sync_shim_tables`，现有） | 会 | 当前作用域 + global | 名字集合是这个 subos 的路由状态，另一个 shell 可能正在使用另一个 subos，不能替它做决定（现有的多 subos 边界） |
| **重新指向（新增 `repoint_stale_shims`）** | **不会**，只换文件对象 | **所有真实 subos + 所有 `knownProjects` 的项目 subos** | 不做任何路由决定，只是恢复"这个名字经由当前 entry 分发"这件事，不越过多 subos 边界 |

项目 subos（`<project>/.xlings/subos/_/bin`）里的 shim 同样是指向 entry 的硬链接，必须包括在内。枚举时从配置读取 subos 列表和 `knownProjects`，不要直接遍历目录，否则会把 `subos\current` 这个 junction 访问两遍。项目状态读不出来 → 跳过并报告。

**谁触发重新指向**（遵守单一写者）：

1. **`entry_binary::replace_with` 替换成功之后，对所有范围执行一次重新指向。** 原则是：谁改变了 entry 的文件身份，谁负责恢复。`cmd_use` 和 installer 两条路径都经过这里，所以只写一次。
2. **`replace_with` 先比较，内容相同就不替换。** 现在 `self update` 会在 `install --use` 里替换一次，再在 `use xlings latest` 里用同样的内容替换一次，每次都会平白改变文件身份。比较之后，文件身份只在版本真的变化时才改变。
3. `sync_shim_tables`：`toRepoint` 自然进入 diff（当前作用域 + global）。
4. `self init`、`self doctor --fix`：对所有范围执行一次重新指向。这是已经坏掉的 home 的恢复入口，见第 4 层。

**重新指向的执行约束**（自我 review）：

- **持有状态锁**（R4）：重新指向会写所有 subos 的 bin，必须在 `xvm::acquire_state_lock` 下执行。`cmd_use`、install、`doctor --fix` 已经持锁；`ensure_home_layout`（`self init` 和 `self install`）目前没有看到持锁，需要核实，没有的话要补上。
- **清理改名留下的文件**（R5，这是现有的潜在问题，这次会被放大）：`displace_locked_file` 和 `atomic_replace_executable` 对无法删除的文件调用 `MoveFileExW(..., MOVEFILE_DELAY_UNTIL_REBOOT)`。**这个调用需要管理员权限**（要写 HKLM 的 PendingFileRenameOperations），普通用户调用会失败，而且返回值被丢掉了。所以现有注释里"重启后由系统删除"对普通用户不成立，`*.xlings.old*` 会一直累积。现在每次升级累积 1 个；重新指向时如果有工具正在运行，会累积更多。解决办法：每次重新指向时，先对这些文件做 classify，确认是 xlings binary 再尝试删除，失败就留到下次。名字匹配不能作为证据，原因同约束 2。

**`self update` 的后置检查**（去掉"没做成也报成功"）：`use` 结束后，对 `subos/<current>/bin/xlings[.exe]` 和所有范围做一次 classify：

- 全部 Current → 打印 `entry 2026.9.26.2 -> 2026.9.27.1; 41 shim(s) repointed in 2 subos`；
- 还有 Stale → 报出名字和原因（例如被占用），退出码按现有约定处理。

### 第 3 层：运行期保证——旧 shim 把执行交给 entry（纵深防御）

第 2 层之后，Stale 只会出现在少数边缘情况：重新指向失败（杀毒软件锁住文件）、降级到修复前的版本再升级回来、跨卷 copy。第 3 层保证：**只要一个 shim 带转交代码，不管它有没有被收敛，执行的都是 entry 的代码。** 这就是符号链接在 POSIX 上提供的保证。

`main()` 最开头，在 `init_console_output` 和任何 narrow 路径转换之前（mcpp#693 就是这里出的问题）执行：

```
own = 自己的映像路径              // Windows: GetModuleFileNameW；POSIX: /proc/self/exe 或 _NSGetExecutablePath
if own 位于 <H>/subos/<s>/bin/ 或 <P>/.xlings/subos/_/bin/    // 纯路径判断，只用宽字符 API
   and 存在 <H>/bin/xlings[.exe]
   and 两者不是同一个文件对象        // Windows 比较 GetFileInformationByHandle 的 volume serial + file index
   and 环境变量 XLINGS_HANDOFF 未设置 // 防止循环
   and entry 的标记版本 != 自己的编译期版本   // 内容相同的 copy 不转交，避免每次多一个进程
then
   Windows: CreateProcessW(lpApplicationName = entry, lpCommandLine = GetCommandLineW() 原样传递)
            // argv[0] 仍然是 shim 的路径，entry 的 extract_program_name / 按所在位置解析 home 都不需要改
            // 设置 XLINGS_HANDOFF=1；子进程放进 KILL_ON_JOB_CLOSE 的 job；父进程忽略 Ctrl+C；等待子进程；原样返回退出码
   POSIX:   execv(entry, argv)，argv[0] 保持不变，不增加进程
```

- **正常状态零开销**：同一个文件对象的判断只需要几十微秒的元数据调用。POSIX 的符号链接 shim 解析后就是 entry，不会走到后面的判断。
- **降级也一致**：`use xlings <older>` 之后，带新代码的 shim 会转交给较旧的 entry，和 AGENTS.md "the file is the authority on what is actually running" 一致。
- **只执行，不修复**：转交路径里不写任何文件（约束 4）。修复只在写者路径里做，所以 ninja 并发调用上千次也不会产生并发写。
- **转交要自己调用 `CreateProcessW`**，不能复用 `exec_host_program_` 用的 `platform::exec` 拼接命令行：那条路径是拼好命令行交给 shell 执行的，引号和 Unicode 都不可靠。
- **"这个 shim 属于哪个 home"不能新增一个回答者**（R6）：直接调用现有的 `xvm::resolve_dispatch_home`（从文件位置向上查找 `is_home_root`），不要在 main 开头另写一套"路径形如 `subos/<s>/bin`"的判断。它在 Windows 上会抛异常，原因是调用了 `path::string()` 做 narrow 转换，不是查找逻辑本身的问题。改成全程使用 `fs::path` 或宽字符串、不抛异常之后，就可以在最前面调用。
- **子进程一启动就清除 `XLINGS_HANDOFF`**（R7）：否则这个变量会被 entry 分发出去的工具继承。比如 clang 内部再调用一个 `ld` 的旧 shim，这个 shim 看到变量已经设置，就不转交了，结果执行的是旧代码。清除后防循环的作用不变，因为 entry 自己和 entry 是同一个文件对象，第一个条件就不成立。
- **POSIX 按 `(st_dev, st_ino)` 比较，不比较路径**（R8）：entry 自己也可能经过符号链接放置（比如 home 被重新定位过）。按路径比较会误判为"不是 entry"，导致不该发生的 exec。

### 第 4 层：过渡——已经被旧客户端弄坏的 home

**先说一个限制：新代码只靠自己无法自动修复这批 home。** 原因是：

- PATH 上的 `xlings.exe` 是旧的，用户之后执行的每条命令都跑旧代码；
- 升级过程里新 binary 只被执行过一次，是 `replace_with` 调用的 `--version` 探测。让 `--version` 顺手修 home 违反约束 4，否决。

所以这批 home 需要一个外部动作，有两个选项：

| 选项 | 做法 | 覆盖面 | 代价和风险 |
|---|---|---|---|
| **B1（推荐，必须做）** | 在 issue 回复和 release note 里给出一条可以直接执行的命令：`& "$env:USERPROFILE\.xlings\bin\xlings.exe" self init`（新版的 `self init` 会对所有范围重新指向）；或者重跑一次 quick_install 的一行安装命令（走 `self install` → `ensure_home_layout`） | 所有情况，包括 reporter 那种"payload 已经装过"的情况 | 用户需要手动执行一次，之后就不用再管 |
| B2（**已确定不做**） | 在 xim-pkgindex 的 `xlings.lua` 的 `config()` 里，只在 Windows 上 exec 新 payload 的 `xlings.exe self repoint …`。这有先例：recipe 自己的注释写过"the recipe owns what an upgrade does"，主题文件就是这么补上的 | 只覆盖 payload 是这次新下载的情况；reporter 那种"already installed"的情况 hook 不会执行 | **时序不对**：`config()` 执行时旧客户端还没替换 entry（`process_xvm_operations_` 在 hook 之后），只能把 shim 链接到 payload 文件本身，这会让 payload 的删除和 GC 和 shim 耦合；子命令运行在旧客户端持有的锁里，必须不拿锁；等于在别的进程的事务里放了第二个 shim 写者 |

**已确定：只做 B1。** B2 这一行保留在表里，只是为了记录不做的理由。

Doctor 的提示也必须能直接执行：在这种 home 上，用户输入 `xlings` 会命中旧客户端，所以提示里不能写 `xlings self doctor --fix`，必须写 entry 的完整路径（`& "<home>\bin\xlings.exe" self doctor --fix`）。

---

## 4. 跨平台对照

| | Linux / macOS | Windows |
|---|---|---|
| 正常的 shim | 符号链接 → Current（第 1 步），行为不变 | 硬链接 → Current（第 2 步），成本和现在一样 |
| copy 或硬链接回退 | 魔数扫描；内容相同 → Current | 资源标记；内容相同 → Current（不重复处理、不转交） |
| 升级后 | 符号链接自动跟上，重新指向时一个 Stale 都找不到 | `replace_with` 之后对所有范围重新指向 |
| 转交 | `execv`，不增加进程；实际上只有回退形态的 shim 才会触发 | `CreateProcessW` + job；只有 Stale 时才触发 |
| 标记 | `.rodata` 魔数 | `res/xlings.rc` 里的 RCDATA |

顺带修一个注释：`mcpp.toml` 里 #613 的注释写着"The shims xlings creates on Windows are hard links or file copies of xlings.exe, so they carry the same manifest"。这正是 #615 推翻的前提（只有升级前是这样），要改掉。

---

## 5. 新旧客户端混用时的行为

"驱动升级的客户端"是升级前 PATH 上的那个版本。决定升级过程怎么执行的是它的代码。

| 驱动者 → 目标 | shim 最终状态 | 用户需要做什么 |
|---|---|---|
| 修复版 → 更新的版本 | `replace_with` 之后全部 Current | 什么都不用做 |
| 修复版 → 修复前的版本（降级） | 修复前的代码不做重新指向：已有 shim 带转交代码，会转交给这个较旧的 entry；降级期间新增的名字链接到较旧的 entry | 什么都不用做（行为和 entry 一致） |
| 修复前的版本 → 修复版（再升级回来） | 带转交代码的 shim 转交给新 entry；降级期间新增的旧 shim 在下一次写操作时收敛 | 什么都不用做 |
| **2026.9.3.1 到 2026.9.26.2 → 修复版**（这次的存量） | 所有 shim 都是没有转交代码的旧版本 | **执行一次 B1** |
| 0.4.7 到 2026.9.2.x → 修复版 | PATH 上的 `xlings.exe` 已经被那一代的 `use` 重建成新版；工具 shim 是旧的 | 新 `xlings` 的第一次写操作（install/use/remove 会重建当前作用域和 global）会收敛当前 subos，其他 subos 由 `self init` 或 `doctor --fix` 收敛；doctor 会报出来 |

---

## 6. 诊断

- 新增 `FindingKind::ShimDispatcherStale`，替代把这类文件报成 `ForeignBinEntry`（现在的文案"not an xlings shim — left alone"在这种 home 上是错的）：
  - Stale 且没有转交代码（老 binary）→ **Error**：`41 shim(s) in subos default run xlings 2026.9.20.1; entry is 2026.9.26.2`。用户实际执行的分发器是旧的，这是真实的故障。
  - Stale 但有转交代码 → Notice：功能正确，只是多一个进程；`--fix` 会收敛它。
  - Unknown → Notice，列出读取失败的原因。
- `EntryBinaryDrift` 的检查对象扩展到 PATH 实际命中的 `xlings`：现在只比较 `home/bin/xlings` 和版本记录，这正是 CI 和发布验证的盲区。
- `--fix` 执行第 2 层的重新指向，并列出处理过的名字和所在范围。

---

## 7. 测试与 CI

**测试首先要能在 `v2026.9.26.2` 上失败。** 做法是先把测试跑在这个 release 上，确认它失败，再跑在修复版上（记忆里有"差分测试在旧 binary 上也能通过"的坑）。

| 测试 | 内容 |
|---|---|
| 单元测试：`classify` 矩阵 | 符号链接、悬空链接、同一对象、有标记且内容不同、有标记且内容相同、老 binary 哈希命中、同一组硬链接、Foreign、**读失败 → Unknown** |
| 单元测试：`plan_table` | Stale 在 desired 里 → repoint；Stale 不在 desired 里 → remove；Foreign 和 Unknown 不碰；保留名的处理方向 |
| Windows e2e：修复版驱动升级 | 用候选构建初始化 home → 安装 fixture program → 再建一个 subos 和一个项目 subos → `use xlings 2.0.0`。2.0.0 的 fixture payload 是候选构建末尾追加几个字节得到的，内容不同、代码相同；做法沿用 E2E-15 的 fixture index → 断言三个范围里所有 shim 都和 entry 是同一个对象 → **经由 `subos\current\bin\xlings.exe` 和 fixture shim** 在含 U+1F9EA 的目录里执行，断言版本 |
| Windows e2e：过渡路径 | 用 `xlings-ci-windows.yml` 里已经 pin 的 `BOOTSTRAP_XLINGS_VERSION`（修复前的版本）作为旧客户端驱动升级 → 断言新版 `self doctor` 报 Error 并返回非 0 → 执行 B1 命令 → 断言全部 Current |
| Windows e2e：转交 | 手工放一个旧版但带转交代码的 copy → 执行它 → 输出 entry 的版本；entry 不存在时执行自己；`XLINGS_HANDOFF` 防循环；退出码原样返回；Ctrl+C 能结束子进程 |
| `candidate-install/smoke.ps1` 补充 | 在"覆盖正在运行的 binary"**之前**先装一个工具；断言经过 `subos\current\bin` 执行，而不是 `$installed` |
| Linux | 现有测试不变（验证符号链接路径零影响）；新增 copy 回退形态的 classify 和 repoint 用例 |

fresh-install 保持不 pin xlings（AGENTS.md 的规定）；升级场景放在 `xlings-ci-windows.yml`。

---

## 8. 交付范围（一次发布，已确定）

| 部分 | 内容 |
|---|---|
| 第 1 层 | `shim_identity::classify`（标记资源 `XLINGS_MULTICALL` 加魔数；老 binary 识别放在 `COMPAT(… → drop in 2027.3)` 里；大小预筛；Unknown 的处理）；三个回答者（`is_our_shim_`、`is_own_shim_`、compat 里的检查）统一改用它 |
| 第 2 层 | `plan_table` 增加 `toRepoint`；`repoint_stale_shims` 覆盖所有真实 subos 和 `knownProjects`；`replace_with` 替换之后执行、内容相同时不替换；`self init` 和 `--fix` 执行；持有状态锁；清理改名留下的文件；`self update` 后置检查 |
| 第 3 层 | `main()` 开头的转交（Windows 用 `CreateProcessW` 加 job，POSIX 用 `execv`）；复用 `resolve_dispatch_home`；清除 `XLINGS_HANDOFF`；按文件对象比较 |
| 第 4 层 | B1：issue 回复模板、release note、doctor 的提示里写 entry 完整路径 |
| 诊断 | `ShimDispatcherStale`：老 binary 报 Error，有转交代码的报 Notice；`EntryBinaryDrift` 扩展到 PATH 实际命中的文件 |
| 测试 | 第 7 节全部；`mcpp.toml` 注释改正 |

因为一次发布，从第一个带标记的构建开始 `caps=handoff` 就是真的。所以"有标记"等价于"能转交"；`caps` 字段只为以后的 abi 变化预留。

## 9. 已确定的决定（2026-09-26 review）

1. 一次发布，不分期。
2. 过渡只做 B1。
3. 老 binary 形态的 Stale 在 doctor 里报 Error，返回非 0。
4. 老 binary 识别到 2027.3 到期，之后没有标记的文件一律按 Foreign 处理。
5. POSIX 标记用 `.rodata` 魔数扫描，性能上限见第 3 节 R3。

## 10. 已经否决的做法

- **让 `--version` 顺手修 home**：这是新 binary 在旧客户端升级过程里唯一会被执行的机会，但违反"只读命令不写"。
- **在 shim 分发时顺手重新链接自己**：ninja 并发调用上千次会产生并发写，而且分发必须是只读的。
- **按"数量异常就不处理"之类的阈值判断**：AGENTS.md 明确要求这类拒绝必须是二值的，阈值等于引入第二个回答者。
- **所有 foreign 文件一律覆盖**：违反用户数据规则。
- **Windows 上原地覆盖 entry 来保持文件对象不变**：entry 正在运行时它的映像被映射，无法原地写入。这正是当初要"先改名再复制"的原因。

---

## 11. 自我 review（第二轮）

### 已经合入正文的修正

| # | 问题 | 不改的后果 | 修正写在哪里 |
|---|---|---|---|
| R1 | 识别老 binary 时，每次都把所有历史 payload 哈希一遍 | bin 里只要有一个用户自己的程序，每次 install/use 都要多读 200 MB 以上 | 第 1 层：先比大小 |
| R2 | `self init` 先重写保留名 `xlings.exe`，再做扫描 | 旧 payload 被 GC 清掉的 home 里，B1 修复不了剩下的旧 shim，而且没有任何提示（又一个"没做成也报成功"） | 第 1 层：K 在任何写操作之前构建 |
| R3 | POSIX 魔数扫描没有上限 | 用户放一个大文件进 bin，每次 sync 都要扫描整个文件 | 第 1 层：256 MB 上限加缓存 |
| R4 | 重新指向会写所有 subos，但 `self init` 的路径目前没有看到持锁 | 和另一个 shell 里正在进行的 install 同时写同一个 bin | 第 2 层：必须持有状态锁（**实现时先核实 `ensure_home_layout` 的调用方**） |
| R5 | 普通用户调用 `MOVEFILE_DELAY_UNTIL_REBOOT` 一定失败，返回值还被丢掉 | `*.xlings.old*` 永远累积；现有注释的说法不成立 | 第 2 层：先 classify，再清理 |
| R6 | 转交如果另写一套"shim 属于哪个 home"的判断 | 出现第二个回答者，和 `resolve_dispatch_home` 在项目 subos、home 被重新定位这些情况下答案不一致 | 第 3 层：复用它，改成不抛异常 |
| R7 | `XLINGS_HANDOFF` 被子进程继承 | 工具内部再调用的旧 shim 不再转交，执行旧代码 | 第 3 层：子进程启动后立即清除 |
| R8 | POSIX 按路径比较 | home 被重新定位后误判，导致不该发生的 exec | 第 3 层：按 `(dev, ino)` 比较 |

### 逐条检查约束（第 1 节）

| 约束 | 结论 |
|---|---|
| 1 跨平台对等 | 满足。POSIX 的符号链接 shim 在 classify 第 1 步、转交的第一个条件就结束，行为和性能不变 |
| 2 用户数据 | 满足。只动有标记、哈希命中或同一组硬链接的文件；读失败归为 Unknown；清理改名文件也要先证明 |
| 3 单一写者 | 满足。名字集合仍然只由 `sync_shim_tables` 决定；重新指向不改名字，只由 entry 的写者（`replace_with`）和两个显式修复入口触发。**B2 正是因为违反这一条而被否决** |
| 4 只读命令不写 | 满足。转交、`--version`、doctor（不带 `--fix`）都只读 |
| 5 无感升级 | 修复版之后的升级、降级、再升级都满足；存量 home 需要执行一次 B1，**这是新代码在这批 home 里根本不会被执行决定的，不是设计上的遗漏** |
| 6 热路径零开销 | 基本满足。Windows 上每次调用多三次元数据系统调用，**需要实测**（见下面 O1） |
| 7 一个问题只有一个回答者 | 满足。三个回答者统一为 classify；转交复用 `resolve_dispatch_home` |

### 仍然开放、要在实现中测量或核实的风险

- **O1 热路径开销没有测过。** 实现后要在 Windows 上测：`clang++ --version` 执行 1000 次，对比修复前后的总耗时；对照组是 `v2026.9.26.2`。多出几十微秒可以接受，达到毫秒级就要把"同一个文件对象"的判断挪到更便宜的实现（比如只比较 own 和 entry 的文件 ID，不打开 entry 读资源）。
- **O2 doctor 退出码变化的影响面。** 按"Blast-radius"那条经验，要 grep 所有**获取** `self doctor` 退出码的地方（CI、e2e、`candidate-install`、fresh-install），不能只 grep 断言。只有 Windows 上从旧版本升级上来的 home 会触发，全新 home 不会，但模拟旧 home 的测试可能会碰到。
- **O3 转交时的 job 和控制台语义。** 父进程本身已经在一个不允许嵌套的 job 里时，`AssignProcessToJobObject` 会失败，这时要降级为不使用 job 并继续执行，不能报错。交互式工具（node、python 的 REPL）经过两层进程时，Ctrl+C、Ctrl+Break 的行为要在 e2e 里覆盖。这条路径只在 Stale 时才会走到，平时很少被执行，**所以必须有专门的 e2e，否则很快会失效而没人发现**。
- **O4 `self update` 的第二步会变成由新代码执行。** 重新指向之后，`platform::exec("xlings use xlings latest")` 经过 PATH 命中的是新的 `xlings.exe`。这是好事，但意味着 update.cpp 里"本函数运行的是旧 binary"的前提只对父进程成立。`use xlings latest` 的命令行接口是稳定的，风险低，但要写进注释。
- **O5 B1 命令的写法。** 要同时给出 PowerShell 和 cmd 两种写法；设置了 `XLINGS_HOME` 的用户路径不同，模板里要写"如果设置了 `XLINGS_HOME` 就用它"。doctor 打印的是实际路径，没有这个问题。
- **O6 测试 fixture 的做法。** 用"候选构建末尾追加字节"制造内容不同但代码相同的 2.0.0，只影响 Authenticode 签名（本来就没有签名），不影响资源读取。实现时要确认 mcpp 的 release 构建对 PE overlay 没有校验。

### 结论

可以进入实现。R1 到 R8 已经合入正文，都不改变方案的结构。O1 和 O3 是这个方案里唯一有实际不确定性的部分，都集中在第 3 层。一次发布的决定不变，但**第 3 层的 e2e（O3）和热路径实测（O1）是合并的前提条件**，不能推到发布之后。

---

## 12. 实现与设计的差异（2026-09-26 实施，2026.9.26.3）

以下是实现时对正文所做的修改。每一条都更简单，或者更准确。

| # | 设计原文 | 实现 | 原因 |
|---|---|---|---|
| I1 | Windows 用 RCDATA 资源作标记，POSIX 用魔数 | **所有平台一律用 `.rodata` 里的同一个魔数**（`xvm::kMulticallMarker`） | 只在写者路径和 doctor 里读文件，每个文件对象只读一次，资源读取在 Windows 上省下的时间没有意义；只有一种机制，Linux CI 就能测到 Windows 用的同一段代码 |
| I2 | 老 binary 靠哈希集合 K 识别，加大小预筛 | **用已有的特征字符串识别**：`create_shim` 的错误格式串 `[xlings:self]: failed to create shim` | 实测 0.4.40 到 2026.9.26.2 的每个版本里都恰好出现一次，mcpp 里没有。这样 R1、R2 都不存在了（没有 K，也就没有"K 何时构建"和"payload 已被 GC"的问题）。放在 `COMPAT … drop in 2027.3` 里 |
| I3 | 在 `replace_with` 之后重新指向 | 新增 **`xself::replace_entry_binary`**：先 `replace_with`，再重新指向，`use` 和 install 两处都调用它 | `entry_binary` 在 core 层，不能依赖 Config（`knownProjects`）；判断和执行仍然只写一次 |
| I4 | 转交时用 `KILL_ON_JOB_CLOSE` 的 job 管理子进程 | **不使用 job** | job 关闭时会连带杀掉工具启动的守护进程（构建服务器、语言服务器）；现有的分发路径同样不管子进程，保持一致 |
| I5 | passthrough 的 `is_own_shim_` 改用 classify | **不改** | passthrough 在分发热路径上，classify 可能读取 PATH 里的大型 host 程序；现有的"路径在本 home 内"判断已经挡住了本 home 的旧 shim。约束 7 在这一处例外，记录在这里 |
| I6 | `EntryBinaryDrift` 扩展到 PATH 实际命中的文件 | 由 Check 1''（`ShimDispatcherStale`，检查所有 bin 目录）覆盖 | PATH 命中的 `subos/current/bin/xlings` 就是其中一个 shim；再加一个 finding 会把同一件事报两次 |
| I7 | doctor 的标签用 "stale shim" | **"outdated shim"** | 和已有的 "stale shim table" 提示冲突，测试的否定断言会被误伤 |
| I8 | — | 路由表 diff 同时包含 `toRepoint`，`sync_shim_tables` 顺带重新指向当前作用域和 global | 每次写操作都会收敛，不依赖是否刚替换过 entry |
| I9 | — | 修复提示（`hint`）在存在 outdated shim 时写 entry 的完整路径 | 同第 3 节第 4 层："`xlings` 在 PATH 上就是旧客户端" |

实测：Linux 的单元测试（gcc 和 clang/libc++ 各 61 个套件）和 E2E-120 都通过；在 Linux 上用硬链接复现了 #615 的机制（`ReplacingTheEntryDetachesHardLinkShimsAndRepointHeals`）。Windows 路径（`CreateProcessW` 转交、硬链接重新指向）由 `xlings-ci-windows.yml` 的 E2E-120w 覆盖。
