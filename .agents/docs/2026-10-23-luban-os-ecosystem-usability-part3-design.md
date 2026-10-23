# Luban OS 的生态级可用性（第三部分）：发布产物的归属、跨仓库的契约、收敛为一个版本

接续第一部分（`2026-10-21-luban-os-ecosystem-usability-design.md`）与第二部分（`2026-10-23-…-part2-design.md`）。
第二部分已合入并发布为 xlings 2026.10.23.1（#698），GitCode 已补齐；本篇处理发布之后看到的问题，把它们与
#699、#700 收敛成**每个仓库一个 PR、一个 xlings 版本（2026.10.23.2）**，并给出发布与验证的闭环。

## 0. 核心点

1. **四个发布产物的版本只有一个写入者：xlings 的发布流程**（R1 已定）。xim-pkgindex#1027 的自动更新曾同时接管
   xlings、luban、luban-os、xdesk，与发布流程互相否定，2026.10.23.1 因此进不了索引。#1031 已去掉这四个包的
   `autoupdate`，并用静态测试守住。
2. **写索引指针要有正式的工具，由人在本地运行**（R3 已定：镜像超时是已知的，本地 gtc 补）。release 的 bump-index
   依赖已被 #1027 删除的脚本、又排在必然超时的镜像之后，从不起作用——删除；新增 `tools/bump_index.py`。
3. **跨仓库契约在两边都测**：xlings 的 rootfs 测试改为经客户端读已发布版本（不再用正则读 Lua 文本，取代 #700 的补丁）；
   索引改动这几个 recipe 时，consumers CI 跑 xlings 的这条测试（U6）。
4. **收敛**：xlings 一个 PR = #699（保留作者）+ U3/U4（指针工具、删失效作业、AGENTS.md）+ U5（契约测试）+ J47
   + 本文档，版本 2026.10.23.2；xim-pkgindex 一个 PR = #1031（等 23.2 的指针与 U6 再合）；luban-os-ci 一个 PR = `after-part2`。
5. **J47 要真试**：`self doctor --isolation` 在本机说可以，私有域却建不成——迁移前用一次真正的试建判断，而不是借用它的结论。

## 1. 现状

| 项 | 状态 |
|---|---|
| 第二部分（J32、J33、J40–J44、J48、J49） | #698 合入，2026.10.23.1 已发布、GitCode 已补齐；索引未指向它 |
| xlings main（bc52ad5b）与所有 PR | Linux 的 distro 分片红：#1027 改了 `xlings.lua` 的结构，测试的正则取错表（§2.3） |
| #699（你的，mcpp#832：script 包 alias 的路径加引号） | 自身通过，只红在同一个 distro 分片 |
| #700（distro 测试的正则补丁） | 止血用，将被 U5 取代 |
| xim-pkgindex #1031 | 四个包去 autoupdate + 2026.10.23.1 指针 + core/desktop/agent-workspace 2026.10.23.1（J44）+ gitee-sync |
| luban-os-ci `after-part2` | 删 J32、J33 的 known，加"别人看不到"的断言；本机 L2 全过 |
| `LUBAN_OS_CI_TOKEN` | 已配置：每个 xlings PR 的构建在 luban-os-ci 跑一次 L2（私有域路径） |

## 2. 问题与定因

### 2.1 一个包的版本由谁写（#1027 与发布流程）

| 写法 | 被谁拒绝 | 原因 |
|---|---|---|
| 发布流程：`url = "XLINGS_RES"` + 各架构 sha256 | `xpkgindex-update verify`："candidate resource architecture matrix differs from declaration" | 自动更新只认它自己的形状 |
| 自动更新：显式 GLOBAL/CN URL | 索引的 `test_xlings.py::test_recent_versions_use_xlings_res` | `XLINGS_RES` 是 xlings 的多镜像解析，新版本必须用它 |
| 同上 | 自动更新的候选安装 `xlings install local:xlings@…` | 只有名为 xlings 的正式包能注册共享的客户端——对客户端本身永远不成立 |

此外 luban、luban-os 的 autoupdate 把 macOS 资源写成 `macosx-aarch64`（发布的是 `macosx-arm64`）。

### 2.2 发布流程写索引的一步

release.yml 的 bump-index 运行 `tools/bump_index.sh` → 索引的 `.github/scripts/version-check.py`（#1027 已删除），且只写 xlings；
它依赖 mirror-binaries，而后者每次都因 GitCode 上传慢超时，bump-index 被跳过。实际的做法一直是：本地 gtc 补镜像，
再手写（我用一个本地脚本写）四个包的指针。

### 2.3 跨仓库的契约

`tests/e2e/rootfs_lib.sh` 用正则取索引 `pkgs/x/xlings.lua` 里第一个 `linux = {` 当版本表；#1027 在 `xpm` 之上加了 autoupdate
（里面也有 `linux = {`）。测试读的是索引 main 的实时内容，所以索引一改结构，xlings 的 main 与所有 PR 同时变红，而索引的 PR 看不到。

### 2.4 本地覆盖不到的路径

本机建不了私有命名空间，私有域路径（J37、J39 一类）只有托管 runner 能测；token 配好后，每个 xlings PR 都会在 luban-os-ci 跑 L2。

### 2.5 J50：有自己用户的机器里没有 xlings（实现中发现）

#701 第一次 CI 派发的 luban-os-ci L2 与 luban-os-ci#2 自己的 CI（都用 2026.10.23.1 的行为）：core-machine 的 12 个场景全部
`xlings: exit 127`，desktop-machine 全过。原因：机器启动时 guest 先挂系统的 home（在前缀里的系统，是主机上的路径——runner 上
`/home/runner/work/_temp/…`，普通主机上 `/home/<user>/.xlings`），再把 userhome 挂到 `/home`，后者把前者盖住了。J32
（2026.10.23.1）让"有自己用户"的机器都挂 userhome，于是前缀里的机器只要有用户就没有 xlings；`persist home` 的机器早就有同样的问题。
desktop 在私有域（`/xlings`），不受影响；本机跑 L2 用 `/tmp` 做工作目录，看不到。

修复：先挂 userhome，再挂 home。两个 share 是主机上同一个目录（`<root>/home`），home 的挂载点就建在 J32 之前它所在的地方，
行为与 J32 之前一致。验证：#701 的 L2 派发（initramfs 用被测的 luban）。

## 3. 设计

### 3.1 发布产物归发布流程（已在 #1031）

- 四个包去掉 `extensions.autoupdate`（与 #1027 之前逐字相同，只留 #1027 对 `ci` 的改动）。
- `tests/test_release_owned.py`：这四个包不得声明 autoupdate；各平台的 `latest` 必须是 `XLINGS_RES` 条目。在 main 的写法上失败，改后通过。
- #1027 的 verify 对 #1031 不再有候选可查。

### 3.2 写索引条目：`tools/bump_index.py`（U3、U4）

名字不用 `index_pointers`：仓库里"索引指针"已指 `xim-index-pointers.json`（`tools/push_index_pointers.sh`，索引 artifact 的清单）。

- `tools/bump_index.py <version> <index-dir>`：从发布的 `.sha256` 资源写 xlings、luban、luban-os（linux x86_64/aarch64、macosx aarch64
  ← `macosx-arm64` 资源、windows x86_64）与 xim:xdesk（linux x86_64）的 `XLINGS_RES` 条目与 `latest`。
  形状与现有条目逐字相同；缺一个资源就整体失败（不写半套）；同一版本已存在就不重复写。
- 单元测试：用固定的 recipe 片段与 sha256，输出逐字比对；再跑一遍不变（幂等）；缺资源时不写。
- 删除 release.yml 的 bump-index 作业与 `tools/bump_index.sh`（它们从不起作用，留着误导）。
- AGENTS.md 的发布说明：release 绿 → 本地 `tools/mirror-latest.sh` 用 gtc 补 GitCode（xlings、luban、luban-os、xdesk、luban-cursors；
  luban-os、xdesk 等 CI 的镜像作业建好 GitHub 端的 release）→ 本地 `tools/bump_index.py` → 开 xim-pkgindex PR。

### 3.3 跨仓库契约：经客户端读，两边都测（U5、U6）

- **U5（xlings，取代 #700）**：rootfs 测试的 fixture 不再用正则。它先让测试里的 xlings（被测的候选）在隔离 home 里读测试自己的索引副本，
  用 `xlings interface package_info` 得到 `xim:xlings` 已发布的版本，选出前一个版本，再在副本末尾追加候选条目。
  recipe 怎么排版归客户端解析；测试只关心数据。`package_info` 原先只给 `version`（`latest` 解析到的那个），候选等于
  `latest` 时（索引已指向本版本后重跑 main）选不出前一个——所以 `package` 加 `versions`：该平台的具体版本，新的在前，
  不含别名（协议 1.18，feature `package_versions`；程序要列版本也用它）。
- **U6（xim-pkgindex，并入 #1031）**：改动 `pkgs/x/xlings.lua`、`pkgs/luban/{luban,luban-os}.lua`、`pkgs/x/xdesk.lua` 时，索引的 CI
  用最新发布的 xlings 做 fixture 做的那次读取（`interface package_info`，每个平台）：读得出、`latest` 在 `versions` 里、有前一个版本。
  整条 rootfs 自更新测试要 docker 与候选构建，留在 xlings 的 CI；U5 之后它只依赖这次读取，索引一侧守住读取即守住契约。
  它依赖 23.2（`versions`）发布，所以 #1031 在 xlings 的 PR 之后合入。
- 不改为固定索引版本：固定版本会过时，别的测试也读实时索引。

### 3.4 J47：迁移前真的试一次（U8）

- 旧布局系统在 `luban upgrade` 时迁入自己的用户态：先导出整台系统（几 GB）、删除、再导入；建不了私有域的主机上导入落回前缀，
  系统与之前一样，归档却一直留着。
- 改为：迁移前在一个临时的私有域里真的初始化一次（与 `subos new --domain-if-possible` 判断"建不建得成"用同一条路径，几秒）；
  建不成就不导出、不删除，只记 `.stays`，说一次原因。`self doctor --isolation` 的结论不够：本机它说可以，域却建不成。
- 已经导出的归档：`luban status <name>` 说出大小与释放办法（`luban rollback <name> --to relayout`，或确认后删除）。
- 实现：xlings 加 plumbing `subos __domain-check <home>`，与 `subos new --domain-if-possible` 共用一段（准备域、在其中 `self init`、
  以"域里的 home 有 default"判断）；成功 0，否则一条错误说原因（协议 1.18，feature `domain_check`，旧 xlings 没有它时 luban 照旧迁移）。
  luban 的迁移先问它：不行就写 `.stays`（原因写进去）、说一次、继续原地升级。`luban status` 对"住在前缀里却留着归档"的系统
  （迁移落回前缀，或回滚到迁移前）说它不需要这个归档，第一条提示是 `rm <归档>`。

### 3.5 #699（mcpp#832）

cherry-pick 那一个提交，保留作者；它的 e2e（第 14 节：路径含空格的 home 里运行 script 包的 shim）一起带上。PR 开出后关闭 #699、#700 并注明去向。

## 4. 发布与验证（2026.10.23.2）

1. xlings PR：CI 全绿（Linux 含 asan、macOS、Windows、aarch64，以及 luban-os-ci 的 L2）→ 自审 → squash 合入 → 取消 main 的 CI → release。
2. 资源一出来就本地 gtc 补 GitCode；重新触发 main 的 CI。
3. `tools/bump_index.py 2026.10.23.2` 写进 #1031（同时验证工具本身）；#1031 加入 U6；CI 绿后合入；发布后等 CDN。
4. 真实验证：

| 验证 | 在哪里 |
|---|---|
| `xlings self update` 到 2026.10.23.2；d1、d7 升级到 desktop 2026.10.23.1；`luban status --check` 无常驻提示；machine-id、时区 | 本机 rv3 |
| d7 里 `adduser alice`，重启后 alice 的 home 归她，`xlings use node` 只对她生效，root 看不到 | 本机 rv3（machine） |
| luban-os-ci 全部 L2（`after-part2` 的场景） | GitHub 托管 runner（私有域）+ 本机 |
| luban fresh 套件、fresh-install | xlings CI（发布后派发） |
| 一个路径含空格的 home 里，gcc 的 `gcc-specs-config` 经 shim 运行（#699） | xlings CI（script 包 e2e） |

5. 通过后合入 luban-os-ci `after-part2`。

期间：#1031 合入前用户仍在 2026.10.22.4（part2 的修复晚到几个小时）；自动更新若在此期间为这四个包开出 PR，直接关闭（#1031 合入后不会再开）。

## 5. 兼容与无感升级

| 什么 | 影响 | 处理 |
|---|---|---|
| 四个包去掉 autoupdate | 只是不再由机器人写；已发布的条目不变 | 无 |
| `tools/bump_index.py` | 写出的条目与手写的逐字相同 | 单元测试逐字比对 |
| 删除 bump-index | 它从未生效 | 无 |
| rootfs 测试经客户端读 | 测试内部；用到的 `interface package_info` 在已发布的客户端里都有 | 无 |
| #699 的引号 | 已记录的 alias 在重装前保持原样（#1035 已让 gcc 的 hook 不依赖 alias） | 无 |
| J47 先试建域 | 建不了域的主机上，旧布局系统不再迁移、不再占几 GB；建得了的照旧迁移 | 已迁移过的不动，只说明归档 |
| 用户从 22.4 直接到 23.2 | 23.1 的条目也在索引里，`self update` 取 `latest` | 无 |

## 6. 多视角自审

| 视角 | 发现 | 处理 |
|---|---|---|
| 架构 | 一个字段两个写入者是这次冲突的根 | 每个包的版本只有一个写入者，归属由测试守住（§3.1） |
| 架构 | 测试依赖另一个仓库的文本格式 | 经客户端读数据，提供方的 CI 跑这条测试（§3.3） |
| 稳定性 | 指针工具写半套会让某个平台指向不存在的资源 | 缺一个资源就整体失败，不写任何东西 |
| 稳定性 | 迁移的判断借用 doctor 的结论会误判（本机即是） | 真试一次（§3.4） |
| 简洁 | 新增一个工具、两条测试 | 删掉失效的作业、脚本与本地临时脚本，总量不增 |
| 用户体验 | 发布到用户可用之间的人工步骤这次卡了一整轮 | 步骤写进 AGENTS.md，工具一条命令写完四个包 |
| 用户体验 | J47：用户看不到几 GB 的归档 | 不再产生；已有的在 `luban status` 里说明 |
| 兼容 | §5 | — |
| 跨平台 | 指针工具的平台与资源名（macOS 的 `arm64` 对 `aarch64`） | 映射写在工具里一处，测试覆盖 |
| 一致性 | 四个发布产物同一规则；luban-cursors 不在其中（它的条目随主题变化，人工） | 写进 AGENTS.md |
| 无感升级 | §5 | — |
| 可测性 | 私有域路径 | luban-os-ci 在每个 xlings PR 上跑 L2（token 已配） |
| 可测性 | 前缀里的系统、home 在 `/home` 下（J50 只在 runner 上出现） | 同上；本机跑 L2 时工作目录放在 `/home` 下 |
| 隐私 | 新工具与文档 | 无个人路径；lint 通过 |

## 7. 任务与依赖

| 编号 | 任务 | 仓库 / PR |
|---|---|---|
| U1 | 四个包去 autoupdate、`test_release_owned.py`、23.1 指针 | xim-pkgindex #1031（已做） |
| U3 | `tools/bump_index.py` + 单元测试 | xlings 合并 PR |
| U4 | 删除 bump-index 与 `bump_index.sh`；AGENTS.md 发布说明 | xlings 合并 PR |
| U5 | rootfs 测试经客户端读版本（取代 #700） | xlings 合并 PR |
| U6 | consumers CI 跑 rootfs 自更新测试 | xim-pkgindex #1031 |
| U8 | J47 真试后再迁移；已有归档的说明 | xlings 合并 PR |
| U12 | #699 cherry-pick | xlings 合并 PR |
| J50 | 机器先挂 userhome 再挂 home（§2.5） | xlings 合并 PR |
| U7 | 2026.10.23.2 的验证（§4）+ luban-os-ci `after-part2` | 本机、runner、luban-os-ci |
| U10 | mcpp 发布 → 索引收 `xim:mcpp` → core 新版本换 mcpp → L2-06 去 known | mcpp、xim-pkgindex、luban-os-ci（与 xlings 无关） |
| U11 | 第四部分：K5（`own` 建在私有域上） | 2026.10.23.2 发布之后 |

```
U3 ──► U4（工具先有，再删旧作业、改说明）
U5 ──► U6（xlings main 上先有新 fixture，索引 CI 才能跑它）
xlings 合并 PR（U3–U5、U8、U12）──► release 2026.10.23.2 ──► #1031（+23.2 指针 + U6）──► U7
U10 独立；U11 在 U7 之后
```

## 8. 已定

| 编号 | 结论 |
|---|---|
| R1 | 四个发布产物的版本由 xlings 的发布流程写，不走自动更新 |
| R2 | 跨仓库测试经客户端读实时索引，提供方的 CI 也跑 |
| R3 | 镜像超时维持现状，资源一出来本地 gtc 补；写指针用 `tools/bump_index.py` |
| R4 | `LUBAN_OS_CI_TOKEN` 已配置 |
| R5 | K5 在 2026.10.23.2 之后；mcpp 归 mcpp 仓库与索引 |
| C1 | #1031 等 2026.10.23.2 一起合入 |
| C2 | #699 并入合并 PR，保留作者 |
| C3 | U6 并入 #1031，在 xlings PR 之后合入 |
