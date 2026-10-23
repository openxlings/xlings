# Luban OS 的生态级可用性（第三部分）：发布链路的归属、跨仓库的契约、验证闭环与剩下的功能

接续第一部分（`2026-10-21-luban-os-ecosystem-usability-design.md`）与第二部分（`2026-10-23-…-part2-design.md`）。
第二部分已合入并发布为 xlings 2026.10.23.1（#698），GitCode 已补齐；本篇处理发布之后看到的问题，
以及前两部分留下、尚未做的。

## 0. 核心点

1. **发布产物的索引指针要有唯一的写入者**：xim-pkgindex#1027 的自动更新（`extensions.autoupdate`）也接管了
   xlings、luban、luban-os、xdesk；它与 xlings 的发布流程互相否定——发布流程写的 `XLINGS_RES` 被自动更新的校验拒绝，
   自动更新写的显式 URL 被索引自己的测试拒绝，而它的候选安装 `local:xlings` 被 xlings 按设计拒绝。
   结果：2026.10.23.1 发布了却进不了索引，用户拿不到。建议这四个包归发布流程（§3.1）。
2. **发布流程里写索引的那一步已经坏了**：xlings 的 `release.yml` 的 bump-index 调用索引的 `version-check.py`，
   #1027 删了它；luban、luban-os、xdesk 一直靠手写。把"按发布的 `.sha256` 写四个包的指针"做成 xlings 仓库里的正式工具，
   发布时自动开索引 PR（§3.2）。
3. **跨仓库的契约没有被任何一边测**：xlings 的 distro 测试用正则读索引里 `xlings.lua` 的文本，#1027 改了文件结构，
   xlings 的 main 与所有 PR 同时变红（#700 临时修好）。改为经客户端读数据，并在索引侧对这类文件的改动跑 xlings 的那条测试（§3.3）。
4. **验证闭环**：2026.10.23.1 要等索引合入后才能在真实系统与托管 runner 上验证（多用户 machine、用户视图、自检无常驻提示）；
   luban-os-ci 去掉 J32、J33 的 known 也在那之后合入（§3.4）。
5. **剩下的功能与清理**：mcpp 发布（mcpp#831 的修复）与 core 里 mcpp 的版本；K5（`own` 建在私有域上）；
   J47（旧布局迁移在建不了私有域的主机上白白导出几 GB）；luban-os-ci 的派发 token 与每日 L2–L4（§3.5）。

## 1. 现状

| 项 | 状态 |
|---|---|
| 第二部分（J32、J33、J40–J44、J48、J49） | 合入 #698，发布 2026.10.23.1，四个产物都已推到 GitCode |
| xlings main 的 CI（bc52ad5b） | macOS 通过；Linux 的 distro 分片因 #1027 失败（与本次改动无关），#700 修复中 |
| xlings #699（别人的 PR） | 同一个 distro 分片失败，其余通过 |
| xim-pkgindex #1031（core/desktop/agent-workspace 2026.10.23.1、gitee-sync、四个指针） | 被 §2.1 的冲突卡住 |
| luban-os-ci 分支 `after-part2`（删 J32、J33 的 known，加"别人看不到"的断言） | 等索引合入、在 runner 上跑过后合入 |
| 本机 luban-os-ci（静态构建、machine） | L2 15 通过，L2-06 为 mcpp#831 known，L2-13 等 K5 |

## 2. 问题与定因

### 2.1 一个包的版本由谁写（#1027 与发布流程）

| 写法 | 被谁拒绝 | 原因 |
|---|---|---|
| 发布流程：`url = "XLINGS_RES"` + 各架构 sha256 | `xpkgindex-update verify`："candidate resource architecture matrix differs from declaration" | 自动更新只认它自己的形状（每架构 `{url = {GLOBAL, CN}, sha256}`） |
| 自动更新：显式 GLOBAL/CN URL | 索引自己的 `test_xlings.py::test_recent_versions_use_xlings_res` | `XLINGS_RES` 是 xlings 的多镜像解析（资源服务器、地区回退），新版本必须用它 |
| 同上 | 自动更新的候选安装：`xlings install local:xlings@…` | xlings 只允许名为 xlings 的正式包注册共享的客户端——候选安装对客户端本身永远不成立 |

另外两处：luban、luban-os 的 autoupdate 写的 macOS 资源名是 `macosx-aarch64`，发布的是 `macosx-arm64`（声明本身就错）；
自动更新每三天扫描一次，会为这四个包开出同样过不了索引测试的 PR。

### 2.2 发布流程写索引的一步

`release.yml` 的 bump-index 运行 `tools/bump_index.sh`，它调用索引的 `.github/scripts/version-check.py --apply --only xlings`——
#1027 已删除这个脚本；而且它只写 xlings，luban、luban-os、xdesk 一直是人按 `.sha256` 手写（第一部分起我用一个本地脚本做）。
这一步还排在 mirror-binaries 之后，而 mirror-binaries 每次发布都因 GitCode 上传慢而超时（15 分钟），整个 release 运行显示为
cancelled，bump-index 被跳过——发布其实成功了，但看起来像失败，后续全靠人补。

### 2.3 跨仓库的契约

- xlings 的 `tests/e2e/rootfs_lib.sh` 用正则取索引 `pkgs/x/xlings.lua` 里第一个 `linux = {` 当作版本表；#1027 在 `xpm` 之上加了
  `extensions.autoupdate`（里面也有 `linux = {`），测试取错表，"fixture needs a distinct published predecessor"。
- 它读的是索引 main 的实时内容，不是固定的版本：索引的任何格式变化都会让 xlings 的所有 PR 同时变红，而索引的 PR 自己看不到。

### 2.4 本地覆盖不到的路径

本机建不了私有命名空间（`--domain-if-possible` 落回前缀），J37、J39 这类只在私有域出现的问题本机看不到；DomainProducer 的 e2e
需要静态候选，只在 acceptance 作业里跑。第二部分的验证里，本机 machine 路径已全跑；私有域路径要靠 runner 上的 luban-os-ci。

## 3. 设计

### 3.1 发布产物的指针归发布流程（要你定，R1）

- xlings、luban、luban-os、xdesk 去掉 `extensions.autoupdate`，回到 #1027 所说的"手动维护"一类（#1027 的说明里也写了
  "Existing xlings-res versions remain intact"）：它们的版本只由 xlings 的发布流程写，资源先进 xlings-res 与 GitCode，再写 `XLINGS_RES`。
- 索引加一条静态检查：`pkgs/x/xlings.lua` 等由发布流程拥有的包不得声明 `autoupdate`（列表写在一处，测试与说明都读它），
  避免以后再被自动加回。
- 备选（R1-B）：改 `openxlings/xpkgindex-update`，让 verify 接受 `XLINGS_RES` 条目、对共享客户端跳过候选安装；
  统一成一个机制，但 xlings 发布依赖另一个仓库的机器人跟上。

### 3.2 发布时自动写索引

- xlings 仓库新增 `tools/index_pointers.py <version> <index-dir>`：从发布的 `.sha256` 资源写 xlings（三个平台）、luban（三个平台）、
  luban-os（三个平台）、xim:xdesk（linux）的指针与 `latest`，形状与现在的条目完全一致；有单元测试（固定输入 → 固定输出）。
  `bump_index.sh` 改用它，不再依赖索引里的脚本。
- release.yml：mirror-binaries 按产物拆成四个作业、各自 30 分钟上限，失败不影响 release 的结论；bump-index 只依赖 create-release
  与 xlings、luban 的镜像（索引只要求这两个在 GitCode 上就能解析）——发布不再"看起来失败"。
- AGENTS.md 的发布说明随之更新：本地 gtc 补 GitCode 仍是第一步，索引 PR 由 release 自动开，人只审与合。

### 3.3 跨仓库契约：经客户端读数据，并在两边都测

- xlings 的 rootfs 测试不再解析 Lua 文本：用测试用的 xlings 在隔离 home 里读索引（结构化的包信息），得到已发布的版本，再在
  测试自己的索引副本里追加候选——格式归客户端解析，测试只关心数据。
- 索引的 consumers CI：改动 `pkgs/x/xlings.lua`、`pkgs/luban/{luban,luban-os}.lua`、`pkgs/x/xdesk.lua` 或 `libs/` 时，
  用最新发布的 xlings 跑这条 rootfs 自更新测试（与现有 consumers 作业同一种方式）——破坏 xlings 的改动在索引 PR 里就红。
- 不改为固定索引版本（R2 的备选）：固定版本会过时，而且别的测试也读实时索引。

### 3.4 验证闭环（2026.10.23.1）

索引合入、发布后等 CDN，然后：

| 验证 | 在哪里 |
|---|---|
| d1、d7 升级到 desktop 2026.10.23.1（core 的 sudo 检查换新）；`luban status --check` 无常驻提示；machine-id、时区 | 本机 rv3 |
| d7 里 `adduser alice`，重启后 alice 的 home 归她、`xlings use node` 只对她生效 | 本机 rv3（machine） |
| luban-os-ci 全部 L2，用 `after-part2` 分支的场景（J32、J33 不再 known） | GitHub 托管 runner（私有域路径）+ 本机 |
| luban fresh 套件、fresh-install | xlings CI |

通过后合入 luban-os-ci `after-part2`。

### 3.5 剩下的功能与清理

- **mcpp**：发布 mcpp（含 #831 的修复；会通知 AUR、Homebrew、Nix），索引 PR 把 core 的 mcpp 升到新版本（发 core 新版本，
  因为 revision 移不动已装系统），luban-os-ci L2-06 去掉 known。
- **K5（`own`）**：第二部分 §3.5 的方向——`own` = 系统住在私有域，`shared` = 住在主人的前缀；切换即两种住处之间的迁移
  （复用迁入自己用户态的那段）。另写第四部分，先定迁移、回滚、用户数据与宿主管理。
- **J47**：旧布局迁移前先试建私有域（`self doctor --isolation` 的同一个判断）；建不了就不导出、不删除，只记 `.stays`。
  已经导出的归档在 `luban status` 里说明大小与释放办法。
- **luban-os-ci**：xlings 仓库配置 `LUBAN_OS_CI_TOKEN`（只有派发权限的 fine-grained token），main 的 CI 派发 L2；
  每日任务跑 L2–L4，失败开 issue（已有）；L5、L6 在发布前。

## 4. 兼容与无感升级

| 什么 | 影响 | 处理 |
|---|---|---|
| 四个包去掉 autoupdate | 只是不再由机器人写；已发布的条目不变 | 无 |
| `tools/index_pointers.py` | 写出的条目与手写的逐字相同 | 单元测试对比现有条目 |
| rootfs 测试改为经客户端读 | 测试内部 | 无 |
| core/desktop/agent-workspace 2026.10.23.1 | `luban upgrade` 换上新的 sudo 检查；包不变 | 已在 #1031 |
| J47 先试建域 | 建不了域的主机上，旧布局系统不再迁移、不再占几 GB | 已迁移过的不动，只说明归档 |

## 5. 多视角自审

| 视角 | 发现 | 处理 |
|---|---|---|
| 架构 | 一个字段两个写入者（发布流程与自动更新）是这次冲突的根 | 每个包的版本只有一个写入者，归属写在一处、由测试守住（§3.1） |
| 架构 | 测试依赖另一仓库的文本格式 | 经客户端读数据；契约在提供方测（§3.3） |
| 稳定性 | 发布每次显示 cancelled，后续靠人补 | 镜像按产物拆、不阻塞结论；索引 PR 自动开（§3.2） |
| 简洁 | 新增一个工具、一条静态检查 | 替换掉索引里已删除的脚本与本地临时脚本，总量不增 |
| 用户体验 | 发布到用户可用之间隔着人工步骤，这次卡了一整轮 | 自动开索引 PR；冲突修好后发布当天可用 |
| 兼容 | §4 | — |
| 跨平台 | 指针工具写三个平台；镜像拆分不改平台 | 单元测试覆盖三个平台的条目 |
| 一致性 | 索引里同类包（xlings-res 的产物）规则不一 | 归属列表一处定义，四个包同一规则 |
| 无感升级 | 已安装的客户端读新条目 | 条目形状不变 |
| 可测性 | 私有域路径本机测不到 | 托管 runner 上的 luban-os-ci 是正式的一层；本机覆盖前缀与 machine |

## 6. 任务与依赖

| 编号 | 任务 | 仓库 |
|---|---|---|
| U1 | 四个包去掉 autoupdate + 归属静态检查；#1031 的指针改回 `XLINGS_RES`；合入 | xim-pkgindex |
| U2 | #700 合入；#699 与 main 的失败作业重跑 | xlings |
| U3 | `tools/index_pointers.py` + 测试；`bump_index.sh` 改用它 | xlings |
| U4 | release.yml：镜像按产物拆分、bump-index 依赖调整；AGENTS.md 发布说明 | xlings |
| U5 | rootfs 测试经客户端读版本 | xlings |
| U6 | 索引 consumers CI 跑 rootfs 自更新测试 | xim-pkgindex |
| U7 | 2026.10.23.1 的验证（§3.4）+ luban-os-ci `after-part2` 合入 | 本机、luban-os-ci |
| U8 | J47：迁移前先试建域 | xlings |
| U9 | `LUBAN_OS_CI_TOKEN`、每日 L2–L4 | xlings（secret，需你配置）、luban-os-ci |
| U10 | mcpp 发布（你触发）→ core 新版本的 mcpp → L2-06 去 known | mcpp、xim-pkgindex、luban-os-ci |
| U11 | 第四部分：K5 的设计 | xlings（文档） |

```
U1 ──► U7（索引合入后才能验证 2026.10.23.1）
U2 ──► U5（先止血，再根治）
U3 ──► U4（工具先有，流程再用）
U5、U6 互补：U5 去掉文本依赖，U6 在提供方守住契约
U10 独立（等你发 mcpp）；U11 在 U7 之后
```

- 交付：xim-pkgindex 一个 PR（U1，即 #1031 改造；U6 随后一个 PR 或并入）；xlings 一个 PR（U3–U5、U8，版本 2026.10.23.2）；
  luban-os-ci 一个 PR（U7 的 `after-part2`、U9 的每日任务）。
- 顺序：U1、U2 先（解锁 2026.10.23.1 与所有 PR）→ U7 → xlings 的 PR（U3–U5、U8）→ 发布 2026.10.23.2 时用新流程自动开索引 PR（这本身就是 U3、U4 的验证）。

## 7. 请你定

| 编号 | 问题 | 选项 | 建议 |
|---|---|---|---|
| R1 | 四个发布产物的版本由谁写 | A 发布流程（去掉 autoupdate）/ B 改 xpkgindex-update 接受 `XLINGS_RES` 并跳过客户端的候选安装 / C 全改为自动更新的形状（改索引测试与 xlings 的注册限制） | **A**：改动最小、与 #1027 "existing xlings-res versions remain intact" 一致；客户端本来就不能被候选安装；C 改的是不变量，不建议 |
| R2 | 跨仓库测试的数据来源 | 经客户端读实时索引 + 提供方测（§3.3）/ 固定索引版本 | **前者** |
| R3 | 镜像超时 | 按产物拆分、放宽上限 / 维持现状、人工补 | **拆分**：发布不再显示失败，索引 PR 自动开 |
| R4 | `LUBAN_OS_CI_TOKEN` | 配置（fine-grained，仅派发 luban-os-ci 的 workflow）/ 不配，只靠 luban-os-ci 的每日任务 | **配置**：xlings 的 PR 能看到 L2 的结果 |
| R5 | mcpp 发布与 K5 | mcpp 现在发；K5 在 U7 之后写第四部分 | 同左 |
