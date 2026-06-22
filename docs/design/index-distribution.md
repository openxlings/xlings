> 编写日期: 2026-06-22 | 版本: 0.4.53 | 关联: [package-index-ecosystem.md](./package-index-ecosystem.md)

# 包索引的分发与更新机制（git-clone → 工件 / Y-asset）

本文记录 xlings **包索引如何被获取与更新**的演进:从早期的"运行时 `git clone`"
到 0.4.52+ 的"**索引作为版本化资源工件**(Y-asset)"。包含旧机制、新机制与对比分析。

> 区分:[package-index-ecosystem.md](./package-index-ecosystem.md) 讲索引的**生态/格式/三层源模型**;
> 本文只讲索引内容**怎么传到用户机器上**(acquisition/transport)。

---

## 1. 旧机制(≤ 0.4.51):运行时 git clone

`sync_repo` / `sync_all_repos`(`src/core/xim/repo.cppm`):

- **首次**:`git clone --depth 1`(经 `mirror::expand` 生成代理候选 URL,静态顺序)。
- **更新**:`git pull --ff-only`,失败回退 `fetch + reset --hard`。
- **节流**:`.xlings-sync-stamp`,7 天内跳过非强制同步。
- 主索引 + 子索引(awesome/scode/d2x)+ 项目索引都走这条 git 路径。

### 旧机制的问题(为什么要改)

| 编号 | 问题 |
| --- | --- |
| **G1** | 子索引(awesome/scode/d2x)`CN` 与 `GLOBAL` 都指向 github,**无国内镜像** → CN 用户子索引必走 github |
| **G2** | 索引 `git clone` **拿不到** HTTP 下载器的延迟重排 + 卡顿看门狗(那是 0.4.49 给 tinyhttps 的);git 子进程无超时 → 劣化网络下"握手成功但 KB/s"会假死数百秒 |
| — | git 不可断点续传、拿不到 CDN、为读 13KB 目录要 clone 1.7MB 带全量历史的仓库 |

行业对照:没有任何主流包管理器对 GitHub 做运行时 `git clone` 取索引(Homebrew→JSON-over-CDN、
Cargo→稀疏 HTTP 索引、Go→GOPROXY 静态文件)。详见 `.agents/docs/2026-06-21-pkgindex-mirror-analysis.md`。

---

## 2. 新机制(0.4.52+):索引作为资源工件(Y-asset)

**索引不再被 clone,而是作为"有版本、带 sha256 校验"的工件,从 `xlings-res` 经 HTTP 下载**,
自动走上已有的资源下载链路 —— 于是**继承了延迟重排 + 卡顿看门狗(根治 G2)**,
并通过区域 origin + 兜底拿到国内可达(缓解 G1)。

### 2.1 工件契约

```
xim-index[-<sub>]-<ver>.tar.gz        # 索引树(pkgs/ .xpkgindex.json xim-indexrepos.lua ...,.git 已剥离)
xim-index[-<sub>]-latest.json         # 指针/清单(format_version, index_version, artifact{name,sha256,size}, signature:null)
```
发布到 `xlings-res/xim-index`(GitHub + GitCode),滚动 `latest` tag(客户端入口)+ `v<ver>` 归档 tag。

### 2.2 运行时获取流程(`src/core/xim/indexfetch.cppm`)

```
按区域键解析候选 URL(选中服务器优先 + 始终追加 GLOBAL/GitHub 兜底 + mirror::expand 代理)
  → adaptive::reorder 按延迟排序
  → 逐候选下载(显式穿透:任何失败含 HTTP 404 都试下一个)
  → 取指针 latest.json → 解析 → 取 artifact(sha256 锁定)→ 校验 → 原子解压换入 data/xim-pkgindex
  → 失败回退 git
```

### 2.3 来源选择 gate(关键设计)

`sync_all_repos` 用 `XLINGS_INDEX_SOURCE=git|artifact|auto`(默认 auto):

| 索引类型 | auto 行为 |
| --- | --- |
| **官方主索引**(remote,URL 含 openxlings/sunrisepeak xim-pkgindex)+ 新装/已工件托管 | **工件** |
| **官方默认子索引**(在主索引 `xim-indexrepos.lua` 里、URL 未被覆盖、非本地) | **工件** |
| 已存在的 **git** 主索引(老检出/fixture) | 保持 git(安全) |
| 用户额外添加 / URL 覆盖 / `file://` 本地 子索引 | git / 本地链接 |

> 默认子索引的判定信号是"**在 lua 默认里且 URL 与 lua 一致**",**不是**"不在 json 里"——
> 因为 `save_sub_repos_json` 每次同步都会把默认子索引写回 `xim-indexrepos.json`
> (0.4.52 曾因此误判,导致 CN 用户子索引仍走 github,0.4.53 修复)。

工件托管的索引目录带 `.xlings-index-version` 标记 → 不会再被 git clone。

### 2.4 捆绑(离线/首启)

release 包内的 `data/xim-pkgindex` 改为**工件托管**(剥 `.git` + 写 `.xlings-index-version`),
新装即走工件;`XLINGS_INDEX_BASE_URL`(本地目录 / `file://`)支持离线/私有镜像/测试。

---

## 3. 对比分析

| 维度 | 旧:git clone | 新:工件 / Y-asset |
| --- | --- | --- |
| 传输 | git packfile(不可续传、无 CDN) | HTTP tar.gz(可续传、可 CDN) |
| 韧性 | 静态顺序代理,无卡顿检测(G2) | 延迟重排 + 卡顿看门狗 + 逐候选穿透 |
| 完整性 | 信任 git origin | **sha256 指针锁定**(任意镜像可不可信;滞后镜像自动拒绝回退) |
| 子索引 CN | 无镜像走 github(G1) | gitcode 原生 `.tar.gz` + github 兜底 |
| 体量代价 | 为 13KB 目录 clone 1.7MB+历史 | 取 ~95KB(主)/ 6–8KB(子)tar.gz |
| 离线/首启 | 必须联网 clone | 可捆绑工件托管 |
| 更新发现 | 7 天 stamp | 每次 `update`(未来可加 ETag/304) |
| 留余地 | — | manifest `format_version`/`signature` → 未来 minisign 签名 / 稀疏索引(X-full) |

---

## 4. CN(国内)设计与实测要点(0.4.54 定稿)

GitCode 平台约束(均实测):release 资产**只能新建**(gtc 不能覆盖、API 删除 405,且大文件上传偶发
`obs_callback EOF` 变成"列出但 404 的幽灵资产");raw 仓库文件**可 git push 覆盖**但 `.json` 需用
`raw.gitcode.com/<o>/<r>/raw/main/<f>` 形式(`/main/` 形式返回 HTML),且**短时间多次请求会被限流**。

由此定稿设计:

- **指针 = 合并的单文件 `xim-index-pointers.json`(仓库文件,git push 覆盖)**,内含所有索引
  `{"format_version":1,"indexes":{"xim":{...},"awesome":{...},"scode":{...},"d2x":{...}}}`。
  - 一次 raw 取回**全部**索引指针 → 避免 gitcode raw 限流(之前一版一取会触发 403)。
  - 仓库文件可 git push 覆盖 → 绕开 gitcode release 不能覆盖/删除的限制。
- **工件 = 版本号唯一的 release 资产**(只新建,从不覆盖)。
- **索引候选不挂 github 代理**(ghfast/ghproxy/kkgithub):它们 TCP 连得上但常不服务该资源,
  延迟低被排前 → 每个 ~30s 超时,正是 CN "fetching package index" 卡死主因。索引只用
  **gitcode + github 直连**(分区排序);gitcode 未命中即快速回退 github。
- 防御:对**超时/无响应**主机 session 内 penalize(404 不罚)。
- **可配置(为自建服务器留口)**:`XLINGS_INDEX_BASE_URL`(env)或 `.xlings.json` 的 `xim.index-base`
  (字符串或 `{GLOBAL,CN}`)指定指针+工件的基地址;自建服务器只需在该 base 下提供
  `xim-index-pointers.json` + `xim-index[-sub]-<ver>.tar.gz` 静态文件即可。

实测(CN,无 VPN-绕过配置):`xlings update` 主+3 子索引全走工件、12s、无卡顿、无回退 git。

**向后兼容(pre-0.4.54 客户端):** 0.4.53 及更早读的是 **release 资产** `xim-index[-sub]-latest.json`
(不是仓库文件合并指针)。0.4.54 起发布脚本仍用 `gh --clobber` 把该 release-asset 指针更新到最新
(仅 github;gitcode 不能覆盖,老 CN 客户端回退 github 读它),这样存量 0.4.53 用户 `xlings self update`
才能看到新版并升级。升级到 0.4.54+ 后即改读合并仓库文件指针。
- `Config::resource_servers("CN")` 只含 GitCode(对二进制包是对的);**索引获取始终追加 GLOBAL/GitHub 兜底**,
  CN 解析链:gitcode(原生)→ github → github 代理(ghfast/ghproxy)。
- sha256 由 github 指针锁定 → 即便某镜像 tarball 滞后(如发布顺序导致的旧内容)也会被拒绝并回退正确源,
  **正确性恒成立**。

---

## 5. 发布流程(每个版本,顺序很重要)

> 教训:同一版本的索引工件若被发布两次(内容不同、文件名相同),GitCode 无法覆盖(`gtc` 无 clobber、
> API 不暴露资产 id)→ 滞后。**索引工件须在内容定稿后一次发布。**

```
1. bump VERSION + mcpp.toml
2. 先更新 xim-pkgindex pkgs/x/xlings.lua:加 ["<ver>"]="XLINGS_RES" + latest.ref=<ver>(三平台)
3. 触发 release.yml:建三平台二进制 + create-release + publish-index
   (publish-index 此时从已更新的 xim-pkgindex 构建索引 → latest 正确,一次写对)
4. 镜像二进制 openxlings/xlings v<ver> → xlings-res/xlings tag <ver>(bare,gh+gtc)
```
secrets:`XLINGS_RES_TOKEN`(对 xlings-res 写)、`GITCODE_TOKEN`;CI 用 vendored `tools/gtc`。

---

## 6. 相关文件 / 文档

- 代码:`src/core/xim/indexfetch.cppm`、`src/core/xim/repo.cppm`(`sync_all_repos`)、`src/core/config.cppm`(resource servers)
- 工具:`tools/build_xim_index_artifact.sh`、`tools/publish_xim_index.sh`、`tools/gtc`
- 设计/调研:`.agents/docs/2026-06-21-pkgindex-mirror-analysis.md`(现状深析)、
  `2026-06-21-pkgindex-redesign-proposal.md`(方案 §7.5 X-full / §7.7 Y-asset / §7.8 索引即包资源)、
  `2026-06-22-index-as-resource-impl-plan.md`(实施 + 进度)
