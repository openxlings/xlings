# 无感升级、兼容性与滚动发布：调研与 xlings 的体系

日期：2026-10-12　版本：2026.10.12.2（PR #659）　规范：`docs/specs/upgrade-and-compatibility.md`

核心原则：**不同功能的故障收敛到最小范围**——一个小问题不能让 xlings 的很多功能都不可用；
自动升级做不到的，要有好的提示和方法。

## 1. 行业做法

| 体系 | 怎么做无感升级 | 怎么保证兼容 | 自动做不到时 |
|---|---|---|---|
| Debian / apt | 包自带 maintainer 脚本（preinst/postinst），升级时按版本跑迁移；`dpkg --configure -a` 可续跑 | `Depends`/`Breaks` 带版本；配置文件（conffile）不被覆盖，冲突时保留两份 | 交互提问；`NEWS.Debian` 在升级时显示一次；失败的包单独停在 half-configured，其他包不受影响 |
| Android | 新版本首次打开时 `onUpgrade(old, new)` 逐级执行数据库迁移 | `minSdkVersion`：旧系统上拒绝安装并说明 | 迁移失败只影响该应用 |
| 数据库迁移（Rails、Flyway） | 有序、编号、幂等的迁移；记录已执行的 | expand/contract：新代码先能读旧结构，再写新结构，最后删旧 | 迁移失败事务回滚，状态不变，报出是哪一步 |
| Kubernetes | 滚动升级，一次替换一部分 | 版本偏差策略（N-1/N-2）；API 弃用期（beta 9 个月、GA 12 个月） | 不兼容的变化只在弃用期后，提前告警 |
| NixOS / OSTree / Flatpak | 每次变化是一个新的代，原子切换 | 旧代保留 | 回滚到上一代 |
| rustup / cargo | `rustup update` 原地升级；`rust-version` 字段 | 旧工具链读到新 `rust-version` 时拒绝并说出需要的版本 | 错误信息给出命令 |
| Chrome / Firefox | 后台下载、重启生效；频道（stable/beta/nightly）；分阶段放量 | 配置文件带版本，旧版本打开新配置会提示 | 服务器端暂停放量（kill switch） |
| Arch（滚动发行） | 持续发布，小步快走 | 不支持部分升级 | 需要人工介入的变化发 News，pacman 钩子提示 |

共同点：

1. **新版本自带迁移**，第一次遇到旧状态时**自动、按顺序、幂等**地执行，并**记录**。
2. **读者宽容，写者设门槛**：新版本总能读旧状态；旧版本读到新状态时只读或明确拒绝，并说出要哪个版本。
3. **故障是局部的**：一个包、一个迁移、一个配置文件出问题，只影响它自己；其他照常。
4. **做不到的事说一次，给出确切的命令**，不让用户猜（Arch News、`NEWS.Debian`）。
5. **可以回去**：旧代、旧版本保留，回退是一条命令。
6. **兼容的变化有弃用期**，不兼容的变化提前告知。
7. **持续验证升级路径**：从旧版本升级是测试的一部分，而不只测全新安装。

## 2. xlings 已有的

| 能力 | 位置 |
|---|---|
| home 布局版本；更新的客户端写过的 home，旧客户端只读并给出该用的客户端 | `main.cpp`（`home_context().writable()`） |
| 策略、edition 的 `min_client`：旧客户端拒绝并给出 `xlings self update` | `policy`、`subos new` |
| index 路由：旧客户端用自己时代的 index 快照 | #476 |
| interface 协议版本，只增补，客户端按能力探测 | `docs/specs/interface-ndjson-v1.md` |
| 配方 `revision`：配置变化时每个 scope 重新配置 | #632 |
| shell profile、旧 home 标记的启动时自愈 | `main.cpp` |
| 过期 shim 交给入口执行（handoff） | `xvm::handoff_target` |
| root 的代：原子切换、回滚 | SubOS part 2 |
| `self doctor --fix`：修复派生状态，不删用户数据 | doctor |
| 兼容代码带 `COMPAT(… drop in …)` 标记 | 各处 |

## 3. 缺的（本轮发现）

1. **没有统一的升级入口**：自愈分散在启动路径各处；其余的只告诉用户"运行 `xlings self doctor`"（`self.upgraded`），用户还要再读一遍报告、自己挑出要做的事。
2. **做不到的事没有确切的命令**：提示是"检查 home"，而不是"这一项要运行这条命令"。
3. **没有"怎么回去"**：新版本有问题时，用户不知道 `xlings use xlings <旧版本>` 可以回去；旧版本运行在新版本升级过的 home 上时也不说。
4. **一个坏文件静默影响一切**（故障注入实测）：home 配置不是合法 JSON，或 `versions` 字段类型不对时，每条命令都正常退出，镜像、索引、SubOS 登记、版本记录全部悄悄回落为默认值；`self doctor` 只报后果（"一个没有登记的 subos 目录"）甚至报 OK。
5. **`self update` 失败时没有最后的办法**：索引更新失败只有一句"failed to update package index"。
6. **兼容约定没有写成规范**：哪些东西有版本、旧客户端遇到新状态怎么办、迁移代码留多久，散在各处注释里。

## 4. 设计

### 4.1 故障域（最核心）

xlings 的状态按故障域划分，每个域坏了只影响它自己：

| 域 | 坏了时 | 其他域 |
|---|---|---|
| home 配置（`.xlings.json`） | 每条命令仍运行，并一行说明哪个文件、哪个字段、什么回落为默认、怎么修；`self doctor` 是一条 Error；`--fix` 保留原文件并重建能推导的部分 | 正常 |
| 一个 SubOS 的状态 | 该 SubOS 不可用，doctor 报告 | 其他 SubOS、全局命令正常（已有，本轮加测试） |
| 一个包的 payload | 该包不可用 | 其他包正常（已有） |
| 一个策略文件 | 该实例拒绝进入，说明原因 | 其他实例正常（已有） |
| 主题、提示 | 回落默认，说一次 | 正常（本轮） |
| 一个升级步骤 | 成为一项待办，带命令 | 其他步骤和用户这条命令照常 |
| 索引 | 用上一个快照或旧客户端的快照 | 已安装的包正常（已有） |

规则：**"读不了"从不当作"空"**（已有原则）——并且读不了时**每次都说出来**，不静默回落。

### 4.2 升级阶梯（`xself::upgrade`）

新客户端在一个 home 上**第一条修改 home 且成功的命令之后**执行一次（只读的、被拒绝的命令让 home 保持原样——已有测试守护这一点）：

1. 比较 home 记录的上次升级版本（`upgrade.to`，没有则用 `verifiedBy` / `version`）与当前版本：
   - 相同：什么都不做（每条命令只多读一次已缓存的配置）；
   - 没有记录（新 home）：记录当前版本，不做别的；
   - 记录的版本**更新**（降级）：什么都不改，提示一次"这个 home 最后由 X 升级；回到它：`xlings use xlings X`"（X 还在时），否则 `xlings self update`；
   - 记录的版本更旧：执行下面的步骤。
2. 步骤，**每步独立的故障域**（异常被接住，成为待办；不阻止后续步骤，也不阻止用户这条命令）：
   1. home 配置健康检查（`home_health`）；坏了就只记待办，后续读配置的步骤跳过；
   2. 派生状态重建：路由表、root 的代（幂等，所以"重做"而不是"迁移"）；持 home 锁，拿不到锁就不记录，下一条命令再做；
   3. 只读检查：doctor 的检测器（同一个函数，所以两者从不矛盾），把有修复命令的错误和警告变成待办。
3. 记录：`.xlings.json` 的 `upgrade: {from, to, at, pending}`，追加 `logs/upgrade.ndjson`。
4. 显示：没有待办时**什么都不说**（无感）；有待办时说一次——每项是什么、运行哪条命令；版本还在时加"或回去：`xlings use xlings <from>`"。
5. 在 SubOS 会话内不执行（会话的 home 属于宿主上的所有者，由他的下一条命令升级）；`self`、`interface` 不执行（它们自己报告）。

### 4.3 做不到时的办法

| 情况 | 说什么 |
|---|---|
| 升级步骤留下待办 | 每项一条命令（多为 `xlings self doctor --fix`），说一次；`self doctor` 一直能看到 |
| home 配置坏了 | 每条命令一行：哪个文件/字段、什么回落、`xlings self doctor --fix`（保留原文件） |
| `self update` 索引失败 | 换镜像的命令；以及最后的办法：安装脚本覆盖安装（保留 home 里的一切） |
| `self update` 安装失败 | 看原因的命令；同样的最后办法 |
| 新版本有问题 | `xlings use xlings <旧版本>`（旧版本保留着） |
| 旧客户端遇到新状态 | 布局：只读并给出 home 自己的客户端；策略/edition：`xlings self update` |

### 4.4 滚动发布与兼容约定（规范化已有做法）

- 版本按日期 `YYYY.M.D.N`；通过 index 的 `latest` 引用发布；GitHub 资源 + GitCode 镜像 + 跨区回退。
- 发布门禁：PR CI（含静态性能通道）、候选安装、全新安装（跟随 `latest`，不钉版本）、host matrix（PR 上 smoke，夜间与按需 full，含 `oldhome`：旧客户端填满的 home 升级到候选版本）。
- 兼容面与规则写入规范：读者宽容、写者设 `min_client`、协议只增补、`COMPAT(… drop in YYYY.M)` 至少保留 6 个月。

## 5. 不在本轮

- 分阶段放量（按比例推送 `latest`）：index 只有一个 `latest`；需要时用渠道（`latest` / `next`）实现。
- 自动回滚（检测到新版本启动失败自动切回）：需要入口二进制之外的守护；目前给出命令。
- 升级步骤的可插拔注册表：当前只有三步，写成一个函数更清楚；步骤多起来时再拆。

## 6. 测试

| 需求 | 测试 |
|---|---|
| `UPGRADE-LADDER` | `Upgrade.TheFirstCommandOfANewClientUpgradesTheHomeByItselfAndSaysNothing` |
| `UPGRADE-PENDING-ONCE` | `Upgrade.WhatItCannotDoByItselfIsSaidOnceWithItsCommand` |
| `UPGRADE-DOWNGRADE` | `Upgrade.AnOlderClientChangesNothingAndSaysHowToGoBack` |
| `FAULT-HOME-CONFIG` | `FaultIsolation.ABrokenHomeConfig…`、`…AFieldOfTheWrongShape…` |
| `FAULT-ISOLATION` | `FaultIsolation.AnUnreadableSubosStopsNothingElse` |
| 真实升级路径 | host matrix `oldhome`（旧客户端 2026.10.9.2 填满的 home） |
