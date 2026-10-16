# 2026.10.16.1 发布后的缺陷与修复方案（xlings、mcpp、xim-pkgindex）

- 日期：2026-10-16。状态：已实施（xlings 2026.10.16.2、libxpkg 0.0.64、xim-pkgindex #987）；§10 记录实施中的修订。
- 范围：xlings 2026.10.16.1（#676）、mcpp 2026.10.16.1（mcpp#811）、xim-pkgindex #983/#984/#985 发布后的真实环境验证，
  以及 xlings、mcpp 两个仓库 main 分支上的 CI 失败。
- 目标版本：xlings **2026.10.16.2**、mcpp **2026.10.16.2**、Luban editions **2026.10.16.2**；每个仓库一个 PR。

## 0. 核心点

1. **D1（主缺陷，xlings）**：Luban 系统内，`/xlings` 以外的任何 xlings home（mcpp 的 registry、系统内普通用户的 home、
   任一新建 home）都无法构建包目录。层边界要求 Lua worker 运行在 bwrap 中，而系统禁止嵌套 user namespace。
   修法：层边界的目的是"配方不得写入系统层与借来的 payload"，这是写入隔离；没有命名空间时由 Landlock 满足，
   pid 由所在系统的命名空间满足。mcpp 不改逻辑，只升级内置 xlings。
2. **D2（xlings）**：沙箱的宿主用户空间把宿主 `/usr/lib` 绑定到 `/lib`、`/usr/lib64` 绑定到 `/lib64`，
   在非 usr-merge 的发行版（Alpine、openSUSE Leap 15）上遮住了真实的 `/lib`，沙箱内 `/bin/sh` 找不到加载器。
   修法：按宿主的实际形态呈现 `/bin`、`/sbin`、`/lib*`，宿主是符号链接就建同样的链接，是目录就原路径只读绑定。
3. **D3（xim-pkgindex，影响线上用户）**：`luban-fonts` 用平台级 `{ ref = "linux" }` 复用 Linux 的版本表，
   libxpkg 把 `ref` 当成版本名，macOS/Windows 上 `xlings install luban-os` 失败。
   修法：索引中三个平台共用同一张版本表，当天即可发布，不依赖客户端发布；xlings 侧让 `inherits` 生效，
   未知的平台级键拒绝而不是当作版本。
4. **D4（xlings）**：旧客户端按 `latest` 选择 edition，被 `min_client` 拒绝后不会选择自己能读的版本。
   修法：未指定版本时选择满足 `min_client` 的最新版本，并说明为什么不是 `latest`。
5. **D5（xlings）**：单独安装运行时包（glibc）不刷新 `<home>/abi` 链接；mcpp 2026.10.16.1 已通过 `config --runtime` 规避。
   修法：安装或移除运行时包本身也刷新 abi。
6. **D6（流程）**：发布窗口内两处顺序竞态，都发生在索引 bump 之前：xlings 的 `luban-fresh`（workflow_run）、
   mcpp 的 main CI 重跑。修法：这两类作业等待索引携带新版本后再运行。
7. **D7（偶发）**：ASan 下 `SessionWakeup` 的 150 ms 窗口；Windows `quick_install.ps1` 的 GitHub API 限流（403）。
8. **D8（xlings，实施中发现）**：系统内 mcpp 的 registry 声明了 `upstreams = /xlings`，同时又被隐式叠加在系统层 `/xlings`
   之上；系统层优先，payload 在原处使用，registry 的 store 中没有条目，mcpp 报 `patchelf missing`。修法：声明了 `upstreams`
   的 home 没有系统层，只经上游借用（store 中的链接条目）。连带 xim-pkgindex 的 gcc 配方：借来的只读 payload 不改写
   specs，以本 home 编译出的程序能否运行判断。

## 1. 证据

| 编号 | 现象 | 证据 |
|---|---|---|
| D1 | 系统内 `mcpp build`：`Bootstrap patchelf/ninja … did not complete`；registry 的 xlings 报 `metadata boundary: Lua worker isolation unavailable; backend: bwrap not installed`；系统内 `XLINGS_HOME=/root/nh xlings update` 同样失败；系统内 bwrap 报 `setting up uid map: Operation not permitted` | 本地 `luban new ecocore --edition core`；xim-pkgindex #986 的 editions 作业（x86_64、aarch64）新增的"core 内 mcpp 用系统 gcc 构建"一步 |
| D2 | `xlings: /bin/sh: No such file or directory`（exit 127），沙箱、预设、代理、剪贴板各项均失败 | xlings-ci-host-matrix 38896104890：u-alpine、u-leap-15、u-alpine-arm（这三行由 2026.10.16.1 新加入矩阵，此前从未运行） |
| D3 | `[xim:luban-fonts@ref] failed: download failed: Only HTTPS is supported` | xlings-ci-luban-fresh 38895694757：luban-host（macos-14、windows-latest） |
| D4 | `subos:luban-tiny needs xlings >= 2026.10.16.1, this is 2026.10.15.4` | xlings-ci-luban-fresh 38895694757：luban-lived-in、luban-upgrade |
| D5 | 缓存形态的 home（只保留 `data/xpkgs`）在线构建时，glibc 已重装，gcc 仍返回 127：解释器 `<home>/abi/linux-x86_64-glibc/ld-linux-x86-64.so.2` 不存在 | mcpp e2e 737；本地删除 `abi` 后 `xlings config --runtime` 恢复 |
| D6 | luban-window：`luban-os script: the script did not finish in 10 minutes`，安装的是 luban-os 2026.10.15.4（索引尚未 bump）；mcpp：`package 'mcpp@2026.10.16.1' not found in the synced index` | xlings-ci-luban-fresh 38877516142（之后的定时运行通过）；mcpp ci 38898041961（重跑通过） |
| D7 | `SessionWakeup.AColdChildExitsWithoutWaitingForTheControlPollTick`（poll 返回 0）；`release '2026.10.16.1' not found on GitHub … 403 (rate limit exceeded)`，GitCode 429 | xlings #676 asan 3/4（重跑通过）；mcpp #811 bootstrap windows-2025（重跑通过） |

main 分支当前状态：mcpp 全部通过（ci、ci-fresh-install、homebrew/aur/pypi 发布）。xlings 的 linux/macos/windows/aarch64/
fresh-install 通过，luban-fresh 与 host-matrix 失败（D2、D3、D4）。

## 2. D1 层边界在 Luban 系统内无法满足

### 2.1 机制

- `lua_boundary.cpp` `build_index()` 与 `create_executor()` 中，`needs_layer_boundary()` 为真（`home::read_system_layer()`
  缺省取 `/xlings`，只要它与本 home 共享 store）或 `readonly_payload` 为真（为借来的 payload 运行配置 hook）时，
  使用 `minimum_layer_policy()`：`fs` 与 `pid` 均为 Must。Lua worker 以 `preferred = Bwrap` 编译。
- Luban 系统（sandbox 载体）禁止嵌套 user namespace，宿主还可能有 AppArmor 的 userns 限制，因此系统内的 bwrap
  无法启动，`compile` 拒绝，结果是除 `/xlings` 以外的 home 一律不可用。`/xlings` 本身不需要层边界，所以系统自带的
  xlings 正常，掩盖了这个问题。
- 受影响的有：系统内 mcpp（registry 是一个独立 home，且从 `/xlings` 借用 payload）；"系统内普通用户装进自己的 home"
  （2026.10.16.1 新功能，同一条路径）；系统内任何新建的 home。
- 宿主上不受影响：宿主有可用的 bwrap（或执行 `self doctor --isolation --fix` 后可用）。

### 2.2 方案

层边界要防的是配方写入系统层（`/xlings` 的 payload、索引）和借来的 payload，本质是写入隔离，不需要隐藏文件。

1. **写入隔离可由 Landlock 满足。** 层边界策略（`minimum_layer_policy` 与 `readonly_payload`）在 bwrap 不可用时，
   以 Landlock 编译 Lua worker：只有 worker 的 scratch 目录和本次安装目录可写（借来的 payload 不在其中），
   其余文件系统只读可见。`select()` 当前规定"Landlock 永不单独使用"，这条规则对会话成立（它不隐藏任何东西），
   对层边界不适用；改为由策略声明自己只需要写入隔离（新增维度 `fs-write`，或给 `fs` 标注 write-only），
   Landlock 满足它。
2. **pid 由所在系统满足。** 在 root instance 内（`XLINGS_ROOT_INSTANCE` 或 `/etc/xlings/usr` 记录），系统本身的 pid
   命名空间已把 worker 与宿主隔开；层边界的 `pid: Must` 视为由系统满足，并在审计中记录 `pid: by the root`。
   在宿主上无 bwrap 时 pid 仍为 Must（保持现状）。Landlock ABI ≥ 6（Linux 6.12）时同时启用信号与抽象 socket 作用域。
3. **范围不扩大。** `needs_layer_boundary()` 的判定不变（宿主上共享 `/xlings` 的 home 仍受边界约束），只在"必须隔离
   而命名空间不可用"时多一个满足方式。内核没有 Landlock 时照旧拒绝，并在拒绝中写明"系统内需要 Landlock（5.13+）"。

### 2.3 验证

- xlings e2e（新）：在 sandbox 载体的 Luban 系统内，`XLINGS_HOME=<新 home> xlings update && xlings install <小包>` 成功；
  worker 写系统层（`/xlings/data/xpkgs/...`）被拒绝；普通用户在系统内装进自己的 home 成功。
- xlings host-matrix：受 AppArmor 限制的宿主（`k-ubuntu-2404-restricted`）上的同一场景。
- xim-pkgindex #986 editions：core 内 `mcpp new && mcpp build && mcpp run`，gcc 是指向 `/xlings` 的链接，没有下载
  `xim:gcc`（该步骤已在 #986 中，当前按预期失败）。

## 3. D2 沙箱的宿主用户空间

- `modules/confine/src/common.cpp` `host_userland()`：`/usr`、`/bin` 原样绑定，`/usr/lib → /lib`、`/usr/lib64 → /lib64`。
  在 usr-merge 的宿主上这与宿主一致；在 Alpine（`/lib/ld-musl-*.so.1`）、Leap 15（`/lib64/ld-linux-x86-64.so.2`）上，
  真实的 `/lib`、`/lib64` 被遮住，所有动态程序 ENOENT。
- 方案：对 `/bin`、`/sbin`、`/lib`、`/lib32`、`/lib64`、`/libx32` 逐个按宿主形态呈现：宿主是符号链接，就在沙箱中建立
  同一目标的符号链接（bwrap `--symlink`）；是目录，就原路径只读绑定；不存在则跳过。proot、Landlock 后端不受影响
  （它们看到的就是宿主）。
- 验证：host-matrix 的 u-alpine、u-alpine-arm、u-leap-15 全部通过；单元测试覆盖两种宿主形态生成的 mount 列表。

## 4. D3 跨平台复用版本表

- 索引（立即）：`pkgs/luban/luban-fonts.lua` 中，用一个 local 表承载 `2026.10.16` 的条目，linux、macosx、windows 三个
  平台共用；同样修正 `xlings-project-templates.lua`、`github-notifications-clear.lua`（`windows/macosx = { ref = "linux" }`，
  同一问题，此前未被发现）。索引 lint 增加一条：平台表的键必须是版本、`latest` 或已知的平台级键。
- libxpkg/xlings：平台级 `inherits = "<platform>"` 已被解析（`xpm.inherits`），但解析器从不使用。让它生效，即
  解析版本时缺失的平台回落到被继承的平台；平台表中值不是表的版本键报错，不再生成名为 `ref` 的版本。
- 验证：xlings-ci-luban-fresh luban-host（macos-14、windows-latest）通过；libxpkg 单元测试覆盖 `inherits` 与非法键。

## 5. D4 旧客户端与新 edition

- 现状：未指定版本时取 `latest`，`min_client` 不满足就拒绝。用户升级前用旧客户端建系统会被拒绝；
  luban-fresh 的 upgrade、lived-in 套件正是这个场景。
- 方案（xlings）：未指定版本时，从满足 `min_client` 的版本中选最新的一个，输出一行说明（"luban-tiny 2026.10.16.1
  需要 xlings 2026.10.16.1；使用 2026.10.11.1，`xlings self update` 后可用最新版本"）；显式指定版本时保持拒绝。
- 测试：本次窗口的旧客户端（2026.10.15.4）没有这一行为，upgrade、lived-in 套件改为显式写出旧客户端能读的版本
  （`subos:luban-tiny@2026.10.11.1`）；从下一次发布起，旧客户端（2026.10.16.2）会自己选到兼容版本，届时套件改回不写版本。

## 6. D5 运行时包的 abi 链接

- 现状：`installer.cpp` 只在"依赖运行时的节点"安装时调用 `abi::refresh`；安装 glibc 本身不刷新，`<home>/abi` 缺失或
  过时，依赖它的工具链无法启动。mcpp 2026.10.16.1 在安装运行时后调用 `config --runtime` 规避。
- 方案：`abi::is_runtime(node.name)` 的节点安装、移除后也调用 `abi::refresh`。mcpp 的规避保留（幂等），并在注释中
  写明适用的 xlings 版本下限。
- 验证：xlings e2e 删除 `abi` 后执行 `xlings install xim:glibc@<v>`，链接恢复；mcpp e2e 737 不变。

## 7. D6 发布窗口的顺序

- xlings `xlings-ci-luban-fresh` 的 workflow_run（release 完成即触发）在索引 bump 之前运行，安装到上一个 luban-os。
  方案：作业开始时等待索引的 `xlings` 与 `luban-os` `latest` 等于本次版本（有超时），或直接安装 `@<本次版本>`。
- mcpp main CI 的 "the newest release builds the tree" 在索引发布前运行就会失败。方案：发布流程中，main CI 在索引 bump
  合入并发布后再重跑；该作业对"索引尚无此版本"给出明确失败原因（现为重复的 not found）。

## 8. D7 偶发失败

- `SessionWakeup`：ASan 下子进程启动慢，150 ms 窗口不足。方案：断言改为"早于 init 的 200 ms tick 被唤醒"
  （测量唤醒时刻相对子进程退出时刻），不以固定窗口判断。
- `quick_install.ps1`：通过 GitHub API 查询 release 时受限流影响。方案：指定版本时直接请求
  `releases/download/v<ver>/<asset>`（不经 API），GitCode 同理；API 仅用于解析 `latest`。

## 9. 计划与依赖

| 顺序 | 仓库 | 内容 | 依赖 |
|---|---|---|---|
| 1 | xim-pkgindex #987 | D3 的索引修正与 lint；gcc 借用 payload（D8）；edition 版本条目的 `min_client`（D4） | 无 |
| 2 | libxpkg 0.0.64 → xlings 2026.10.16.2 | D1、D2、D3（libxpkg 继承）、D4（libxpkg `min_client`）、D5、D6（luban-fresh）、D7、D8；文档：沙箱的宿主用户空间、层边界在系统内的满足方式 | 1（luban-host 套件） |
| 3 | mcpp 2026.10.16.2 | 内置 xlings 钉到 2026.10.16.2；D6 的流程说明；CHANGELOG | 2 |
| 4 | xim-pkgindex #986 | Luban 2026.10.16.2 editions 携带 mcpp 2026.10.16.2（core、desktop、desktop-xdesk、agent-workspace），`min_client` 2026.10.16.1（§10）；editions CI 中"core 内 mcpp 用系统 gcc 构建"通过 | 2、3 |
| 5 | 验证 | 干净 home：xlings、luban-os（Linux、macOS、Windows）、core 系统内 mcpp 构建运行、系统内普通用户安装；host-matrix 全部行；luban-fresh 全部套件 | 4 |

系统内的 xlings 随宿主客户端更新（`luban upgrade`），D1 对"系统内普通用户安装"的修复随之进入已有系统；mcpp 使用自己
内置的 xlings，D1 的修复随 mcpp 2026.10.16.2 生效。

## 10. 实施中的修订

| 项 | 方案原文 | 实施 | 原因 |
|---|---|---|---|
| D1 | 新增策略维度 `fs-write` | 不新增维度。`lua_boundary` 在"仅层边界要求隔离（无用户声明的策略）、在系统内、bwrap 不可用、Landlock 可用"时以 Landlock 编译 worker，`fs`、`pid` 降为 Should；worker 在读取请求前建立围栏（ABI ≥ 6 时同时启用信号与抽象 socket 作用域） | 用户声明的策略保持原样，改动限于层边界一处；`select()` 的规则对会话不变 |
| D1 | 可写范围为本次安装目录 | 可写范围为本次安装目录的父目录 | hook 可能删除并重建安装目录（如 `luban-fonts` 的 `os.mv`）；bwrap 下 worker 看到的是该父目录的副本 |
| D3 | 非表的版本键报错 | 平台级 `ref` 与 `inherits` 均解析为继承，加载后沿继承链补齐版本、`deps`、`exports`、`source`；`ref` 不再生成版本 | 字符串值的版本键（`"XLINGS_RES"`）是合法写法，不能一概报错；平台级 `ref` 是 V1 规范的继承写法 |
| D4 | 从满足 `min_client` 的版本中选择 | 版本条目新增 `min_client` 字段（libxpkg 解析）；未指定版本时 `select_version_` 跳过本客户端不能使用的版本并说明；edition 的版本条目与其 manifest 的 `min_client` 一致（索引测试保证） | manifest 只有在获取 edition 之后才能读到；选择必须在获取之前完成 |
| D4 | 2026.10.16.2 editions 的 `min_client` 为 2026.10.16.2 | 保持 2026.10.16.1 | `min_client` 表达 manifest 需要的客户端能力；2026.10.16.2 editions 未使用新的 manifest 能力，上一版客户端仍可创建 |
| D6 | 作业开始时等待索引 | luban-fresh 新增 `index` 作业：等待已发布索引（`xim-index-pointers.json` 的 `source_commit`）中 `xlings` 与 `luban-os` 的 `latest` 等于本次版本，最长四小时 | 已发布索引而非仓库 main 才是客户端读到的 |
| D7 | 断言改为相对子进程退出时刻 | 以子进程变为僵尸（或已被回收）的时刻为起点，窗口 100 ms | 不计入 ASan 下的启动耗时 |
| D7 | `quick_install.ps1` 指定版本时不经 API | GitHub 与 GitCode 都直接请求 `releases/download/<tag>/<asset>` 的第一个字节，以此确认 tag 并测量来源 | 两个来源都有 API 限流（403、429） |
| D8 | — | 新增，见 §0 第 8 条 | 本地 Luban core 系统内的真实验证 |

本地验证（sandbox 载体上的 Luban core 系统，开发构建的 xlings）：`XLINGS_HOME=<新 home> xlings update` 在 Landlock 围栏中
构建目录，配方顶层写 `/root` 被拒绝（同一路径 root 直接写入成功）；以开发构建作为 mcpp 的 xlings，`mcpp new && mcpp build
&& mcpp run` 使用系统的 gcc（registry 中为指向 `/xlings` 的链接），不下载工具链。

