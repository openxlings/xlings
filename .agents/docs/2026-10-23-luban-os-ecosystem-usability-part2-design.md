# Luban OS 的生态级可用性（第二部分）：多用户的 machine、私有域的一致、自检说真话、生态流程补齐

接续 `2026-10-21-luban-os-ecosystem-usability-design.md`（下称"第一部分"）。第一部分已发布到 2026.10.22.4，
全部生态在发布的版本上验证通过（第一部分 §13.3）；本篇处理它留下的（第一部分 §15）与验证中看到、尚未修的。

## 0. 核心点

1. **多用户的 machine 才是剩下的真缺口**：单用户的系统、桌面、agent 都已可用；machine 里新加的用户写不了自己的 home（J32），
   用户不加 sudo 装的包对全系统生效（J33）。两处都有已定位的原因，J32 的做法要你定（§3.1）。
2. **私有域里的系统，所有"读它的东西"的命令都到它所在的地方去读**：J37、J39 修了请求与 broker；`subos log`、`report`、`ps`
   还在读主人的 home（§3.2）。做成一条规则（"按名字找到它住的 home"），而不是再逐个命令补。
3. **自检要说真话**：`luban status --check` 的 4 条常驻提示里，3 条是查错了地方或时机（Falkon 的"能启动"、沙箱里的 sudo、
   沙箱运行时才绑定的时区），1 条是真缺（除 agent 工作区外，系统没有 machine-id；machine 里也没有时区）。修检查，也补上缺的（§3.3）。
4. **生态流程里两个会掉链子的地方**：xim-pkgindex 的 gitee-sync 只认 push（GitHub 漏发 push 时 CN 用户拿不到新索引）；
   私有域路径只有托管 runner 能测，xlings 自己的 CI 里没有一条跑它的作业（§3.4）。
5. **K5（存放方式 `own`）放到第三部分**，但先定一个方向：建在已有的私有域机制上，不另起一套（§3.5）。

## 1. 现状与定因

| 编号 | 现象 | 原因（已查证） | 影响 |
|---|---|---|---|
| J32 | machine 里 `adduser` 加的用户写不了自己的 home | machine 的目录树是宿主上的文件，非特权的 virtiofsd 与 9p（`security_model=none`）都以宿主用户存文件，`chown` 得到 EINVAL | 多用户 machine 不成立；建系统时的默认用户不受影响 |
| J33 | 用户不加 sudo 的 `xlings install` 让包对所有用户生效；用户 `remove` 后命令仍在 PATH 里 | `__store-add` 以管理员执行 `install -y`，系统 workspace 里没有激活版本时 install 会激活它（三处：`activate_requested_targets`、注册计划、借用成员） | 多用户之间没有隔离；单用户感觉不到 |
| J40 | 私有域里的系统，`subos log/report/ps` 可能看不到它的事件 | 三个命令都读 `home_view()`（主人的 home）；CI 上 agent 工作区的事件日志不在主人的 `logs/subos/agent/` 下（第一部分 J37 的诊断） | 排查 agent 时看不到记录；先在 runner 上确认 |
| J41 | `broker::enqueue` 写请求失败时仍返回请求号 | 不检查 `create_directories` 与写文件的结果 | 失败被说成"等待审批"，J37 就是这样藏住的 |
| J42 | 桌面的 `luban status --check` 常驻"falkon starts — exit 134" | 通用的"能启动"检查只在"有同名 id 的自有检查"时跳过；Falkon 的自有检查叫 `falkon-runs`，于是被不带显示地运行 | 误报 |
| J43 | "it has a machine id"、"it has a time zone" 两条常驻提示 | 只有 neutral 身份（agent 工作区）会写 machine-id、定时区；其他系统 `/etc` 里两样都没有。沙箱运行时会绑定宿主的 `/etc/localtime`（检查读的是根，所以误报）；machine 里什么都没有，程序按 UTC | machine-id：D-Bus 与依赖它的程序会告警；时区：machine 里的时间是 UTC |
| J44 | "sudo is there and says its version — no new privileges" | 系统的检查在沙箱里执行，bwrap 设了 no-new-privileges，sudo 连 `--version` 都拒绝；而这台桌面平时是以 machine 运行的 | 误报；沙箱模式下 sudo 本来就不可用（特权走 broker） |
| J45 | 私有域路径在本机测不到，xlings 的 PR CI 里 DomainProducer 也跳过 | 本机建不了私有命名空间；DomainProducer 要静态候选，PR CI 的构建带解释器 | J37、J39 只能靠 luban-os-ci 在 runner 上发现 |
| J46 | xim-pkgindex 合入后 CN 镜像可能晚一天 | gitee-sync 只有 `on: push`；2026-10-22 GitHub 漏掉了 #1030 的 push 事件，发布靠手动触发补上，gitee 没补 | CN 用户的索引落后 |
| — | `mcpp self doctor` 在系统里报错（mcpp#831） | 修复已合入 mcpp，未发布 | 等 mcpp 发布与 core edition 更新 |

## 2. 原则

- 第一部分 §2 的原则不变；本篇只补两条：
  - **一个系统住在哪里，只有一个答案**：按名字解析出它的 home（主人的前缀，或私有域），所有读写它的命令都用这个答案（§3.2）。
  - **检查描述系统运行时的样子**：系统以什么方式运行（沙箱、machine），就按那种方式判断；判断不了的说"运行时检查"，不报失败（§3.3）。

## 3. 设计

### 3.1 多用户的 machine（J32、J33）

**J33：用户装的只给自己**（第一部分已定的方向，本篇实施）

- `__store-add` 改为"只放进仓库并登记"：payload 放进系统的 store，版本记入系统 scope 的 `installed`，**不激活**；三处激活逻辑一起改。
- 用户视图随后借用它，并且只在自己的 workspace 里激活。
- 与 J36 一致：J36 判断"系统有没有"看的就是 `installed` 登记，不看激活。
- 已经因旧行为而对全系统生效的包：不迁移（无法区分是谁装的）；`sudo xlings remove` 照旧可以撤掉。
- 测试：用户视图的 e2e 增加反向断言（另一个用户看不到）；luban-os-ci L2-02 去掉 J33 的 known，加"另一个用户看不到"。

**J32：新用户的 home**（要你定，见 §7 的 R1）

| 方案 | 做法 | 代价 |
|---|---|---|
| A（建议） | machine 里**有默认用户以外的用户**时，`/home` 改走 9p `security_model=mapped-xattr`（属主与权限存在宿主文件的 xattr 里）；根仍走 virtiofs | 只有多用户的 machine 的 `/home` 慢一些（9p）；宿主上文件仍归宿主用户，home 仍是用户数据、仍可导出 |
| B | virtiofsd 在用户命名空间里按 subuid 映射 | 宿主上的文件归 subuid，主人直接读写自己系统的 home 会碰到权限；依赖宿主的 `newuidmap`（T0） |
| C | 新用户的 home 放在 machine 自己的磁盘镜像里 | 完整 POSIX、最快；但这些 home 在宿主上看不见，沙箱模式进不去，导出要另做 |

- A 的触发：guest agent 在 `adduser` 之后（或启动时）发现 `/etc/passwd` 里有 home 在 `/home` 下、且不是默认用户的用户，就请宿主下次启动时用 mapped-xattr 挂 `/home`；当次用 broker 的方式补建该用户的目录（属主写进 xattr）。
- 已有文件没有 xattr：9p 按宿主属主显示，也就是默认用户——与现在一致，默认用户无感。
- 测试：luban-os-ci L2-03、L2-05 去掉 J32 的 known（alice、bob 各有自己的版本）；L5 的 9p 与 virtiofs 预算分别记。

### 3.2 私有域的一致（J40、J41）

- 一条规则：`subos::locate(name)` 返回系统住的 home（`domain_producer::read_scope` 已经能答）；
  `requests/approve/deny`（J37）、`__broker-serve`（J39）、`upgrade/boot/export` 已经按它转发；`log/report/ps` 也按它转发。
- 不再逐个命令补：`subos` 子命令的分派处列一张"读写系统状态的子命令"表，表里的统一转发；新增子命令时 lint 检查它在不在表里。
- J41：`enqueue` 写失败就返回错误（"请求没有记下：<原因>"），客户端照实说，不说"等待审批"。
- 测试：DomainProducer 的 e2e 已有 requests、`__broker-serve` 的断言，补 `log`、`report`、`ps`；这些测试在 §3.4 的新作业里真正执行。

### 3.3 自检说真话（J42、J43、J44）

- **J42**：通用的"能启动"检查，凡是包**声明了任何自有检查**就跳过（自有检查比 `--version` 更懂它）；没有声明的图形程序，
  检查时设 `QT_QPA_PLATFORM=offscreen`、不给 `WAYLAND_DISPLAY`/`DISPLAY`。
- **J43**：
  - machine-id：每个系统在创建时生成一次，写进系统自己的 `/etc/machine-id`（与 neutral 身份的做法相同，只是不随 persona 变）；
    已有的系统在下一次进入或升级时补上一次（幂等，记录在 home 的格式步骤里）。
  - 时区：沙箱模式照旧绑定宿主的 `/etc/localtime`，检查改为"运行时由宿主提供"——不报失败；
    machine 模式在启动时由 guest agent 写入宿主时区的 TZif 内容（宿主 `/etc/localtime` 的字节，自带 POSIX TZ 规则，不需要 tzdata 包）；
    用户在系统里自己设过的（`/etc/localtime` 已存在且不是我们写的）不覆盖。
- **J44**：sudo 检查按系统的运行方式判断：以 machine 运行的系统，在 machine 里检查（`luban status --check` 已能在运行中的 machine 里执行检查时）；
  machine 没在运行，就标"machine 运行时检查"；沙箱模式说明"沙箱里特权走 broker"，不报失败。
- 结果：新建与升级后的桌面，`luban status --check` 没有常驻提示；提示只在真有问题时出现。

### 3.4 测试与生态流程（J45、J46、mcpp）

- **私有域作业**（J45）：xlings 的 Linux CI 增加一个作业，用这一轮的静态构建（dist + musl）作为 `XLINGS_BIN`，在开放了用户命名空间的 runner 上
  只跑 `requires static-xlings` 的 XTEST（DomainProducer 等）；它属于 main 与 `ci:asan` 一类的"合入前必须"作业，耗时约 10 分钟。
- **gitee-sync**（J46）：xim-pkgindex 的 gitee-sync 增加 `workflow_dispatch` 与每日一次的 `schedule`；发布流程里"push 没触发"时手动补的一步写进 AGENTS.md 的发布说明。
- **luban-os-ci**：
  - xlings CI 派发 L2 的 `LUBAN_OS_CI_TOKEN` 若未配置，补上（只读派发权限的 fine-grained token）；
  - 每日任务覆盖 L2–L4，L5、L6 在发布前；场景里的 known 随 J32、J33 修复删除。
- **mcpp**：mcpp 发布（会通知 AUR、Homebrew、Nix）由你触发；随后 xim-pkgindex 一个 PR 更新 core edition 里 mcpp 的版本，
  luban-os-ci L2-06 的 known 随之删除。

### 3.5 K5（`own`）的方向（第三部分）

- 第一部分把 `own` 设想为"系统的实例、闭包、ABI、版本登记在两个 home 之间搬动"；而这一轮看到，runner 上的桌面与 agent 已经住在私有域里——
  自己的 /xlings、自己的版本库，就是 `own` 想要的隔离。
- 方向：`own` = "系统住在私有域"；`shared` = "系统住在主人的前缀"。切换就是在两种住处之间迁移（复用 `subos upgrade` 迁入自己用户态的那段），
  不新增第三种机制。J36、J38 这类"共用版本库"的问题在 `own` 下从根上消失。
- 本篇不实施；第三部分先写清迁移、回滚、用户数据的边界与宿主管理。

## 4. 兼容与无感升级

| 什么 | 旧 → 新 | 处理 |
|---|---|---|
| J33 的行为 | 用户装的包对全系统生效 → 只对自己 | 已生效的不动；新装的按新规则；`xlings list` 里标出"系统的"与"你的" |
| J32 的挂载 | `/home` 一律 virtiofs → 多用户时 9p mapped-xattr | 只在出现第二个用户后、下一次启动时切换；默认用户的文件属主不变 |
| machine-id | 无 → 每系统一个 | 下一次进入或升级时补；不改已有的 |
| 时区 | machine 里 UTC → 宿主时区 | 用户自己设过的不覆盖 |
| `log/report/ps` | 读主人的 home → 读系统住的 home | 前缀里的系统两者相同，无变化 |
| 旧客户端读新系统 | — | 都不新增文件格式；machine-id 与 localtime 是标准文件 |

## 5. 多视角自审

| 视角 | 发现 | 处理 |
|---|---|---|
| 架构 | J37、J39、J40 是同一类：不知道系统住在哪里 | 一个 `locate` + 一张子命令表 + lint，代替逐个补（§3.2） |
| 架构 | J36、J38 是"共用版本库"的同一类 | 本篇以登记为准；根治放到 K5 的 `own`（§3.5） |
| 稳定性 | 写失败被说成等待（J41） | 失败就说失败 |
| 稳定性 | 9p mapped-xattr 的 xattr 写不进的文件系统（宿主是 tmpfs、某些 FUSE） | 启动前探测，探测失败就不切换，并在 `luban status` 里说明多用户受限的原因 |
| 简洁优雅 | 新概念 | 没有新命令、新配置；J32 的切换是自动的 |
| 用户体验 | 自检常驻提示让人不信任检查 | 修到"没有提示 = 没有问题"（§3.3） |
| 用户体验 | 多用户的 `/home` 变慢 | 只有多用户的 machine 才付这个代价；`luban status` 说明原因 |
| 兼容 | 见 §4 | — |
| 跨平台 | 9p mapped-xattr 在 macOS、Windows 的 carrier 上 | 那两条路径的 machine 本来就走各自的共享；J32 的方案只用于 Linux 宿主的 qemu，其他平台写进能力报告 |
| 一致性 | 沙箱与 machine 的时区来源不同 | 都是"宿主的时区"，只是送达方式不同（绑定 / 写入） |
| 无感升级 | machine-id、时区、J33 都影响已有系统 | 幂等补齐、不覆盖用户的设置、已生效的包不动（§4） |
| 可测性 | 私有域路径在本机测不到 | §3.4 的作业 + luban-os-ci 在 runner 上；harness 在有用户命名空间的 Linux 上本地也能跑 |
| 隐私 | machine-id 是标识 | 每个系统随机生成、只在系统里；不上报 |

## 6. 任务与依赖

| 编号 | 任务 | 仓库 |
|---|---|---|
| T1 | J42 通用检查的跳过与 offscreen | xlings |
| T2 | J43 machine-id 生成与补齐（含 home 格式步骤） | xlings |
| T3 | J43 时区：沙箱检查、machine 启动写入 | xlings |
| T4 | J44 sudo 检查按运行方式 | xlings |
| T5 | J40 `locate` + 子命令表 + lint；`log/report/ps` 转发 | xlings |
| T6 | J41 enqueue 报错 | xlings |
| T7 | J45 私有域作业 | xlings |
| T8 | J33 `__store-add` 不激活 + 反向断言 | xlings、luban-os-ci |
| T9 | J32（按 R1 的决定） | xlings、luban-os-ci |
| T10 | J46 gitee-sync 的 dispatch 与 schedule | xim-pkgindex |
| T11 | luban-os-ci：派发 token、每日 L2–L4、删除失效的 known | luban-os-ci、xlings（secret） |
| T12 | mcpp 发布后 core edition 的 mcpp 版本 | mcpp（你触发）、xim-pkgindex |
| T13 | 第三部分：K5 的设计 | xlings（文档） |

```
T7 ──► T5（新作业先就位，T5 的转发才有真执行的测试）
T1、T2、T3、T4、T6 互不依赖
T8 ──► T9（先定"装给谁"，再定"写到哪"）；T8、T9 ──► T11 删 known
T10、T12 独立；T13 在 T8、T9 之后
```

- 交付：xlings 一个 PR（T1–T9，版本 2026.10.23.1，若 R1 未定则 T9 单独随后）；xim-pkgindex 一个 PR（T10，及 T12 待 mcpp 发布）；
  luban-os-ci 一个 PR（T8、T9、T11 的场景）。
- 发布与验证按第一部分 §13.1 的顺序；验证用第一部分 §13.3 的表，加三行：多用户 machine（alice、bob 各自的版本与 home）、
  `luban status --check` 无常驻提示、私有域里 `subos log/report` 有记录。

## 7. 请你定

| 编号 | 问题 | 选项 | 建议 |
|---|---|---|---|
| R1 | J32 的做法 | A 多用户时 `/home` 走 9p mapped-xattr / B subuid 映射 / C 新用户 home 放在 machine 的磁盘镜像 | **A**：只影响多用户 machine 的 `/home` 速度，home 仍是宿主上的用户数据、可导出；B 破坏主人直接读写自己系统的文件，C 让这些 home 在宿主上不可见 |
| R2 | machine 的时区来源 | 宿主时区的 TZif 字节写入 / 装 tzdata 包并设 `TZ=<名字>` | **写入字节**：不加包、离线可用；要换时区的用户在系统里自己设，我们不覆盖 |
| R3 | J33 已生效的包 | 不动 / 升级时改回只属于装它的人 | **不动**：记录里分不出是谁装的，猜错会让别人的命令消失 |
| R4 | 交付切分 | T1–T9 一个 PR 一个版本 / T1–T7 先发，T8、T9 随后 | 若 R1 当场定，**一个 PR**；否则先发 T1–T7 |
| R5 | mcpp 发布与 K5 的时间 | mcpp 现在发 / 随下一次 mcpp 迭代；K5 第三部分现在写 / 本篇完成后 | mcpp 现在发（只差发布）；K5 在本篇完成后 |
