# xlings 包索引与镜像机制 — 深度分析与优化方向

> 日期: 2026-06-21
> 类型: 分析报告 (analysis)
> 范围: `xim-pkgindex` 主索引 + 子索引仓库 (awesome/scode/d2x) 的网络获取链路
> 关联: `2026-05-01-mirror-fallback-step1.md`、`2026-06-04-github-asset-adaptive-mirror.md`、
>       `2026-05-29-compact-git-bootstrap-plan.md`

## TL;DR — 一句话结论

xlings 已经做对了大方向（域内 origin + 多镜像回退 + 延迟探测），但**用户痛点恰好落在被这套机制覆盖最弱的那条路径上**：

1. **子索引仓库 (`awesome`/`scode`/`d2x`) 根本没有国内镜像** —
   `xim-indexrepos.lua` 里这三个仓库的 `CN` 与 `GLOBAL` 都指向同一个
   `github.com/d2learn/...`。CN 用户主索引能走 Gitee，子仓库却必然回到 GitHub。
2. **索引的 `git clone` 路径只享受了"静态顺序镜像回退"，没有享受 0.4.49 的延迟重排和卡顿看门狗** —
   后两者只在 HTTP 下载器 (`tinyhttps`) 里，`git clone` 走的是 `compact::git` CLI，
   GitHub 永远被第一个尝试，劣化网络下会先卡满超时再轮代理。
3. **编译进二进制的代理列表会"腐烂"** — `kkgithub` 已死、`jsdelivr` 在国内已无 ICP、
   `ghproxy` 系免费代理整体脆弱；而"远程热更新代理表"被显式推迟了。

行业共识（Homebrew/Cargo/Go/apt 全部走过同一条路）：**索引应当以"静态文件 + CDN +
传输无关的签名/哈希完整性"分发，而不是对 GitHub 做 live git clone**。下文给出分级优化路线。

---

## 1. 现状：xlings 到底怎么取索引的

### 1.1 两个都叫 "mirror" 的东西（务必区分）

代码里 "mirror" 一词被重载，是理解全局的前提：

| 概念 | 取值 | 作用 | 代码 |
| --- | --- | --- | --- |
| **区域键** `Config::mirror()` | `GLOBAL` / `CN` | 选择**上游主机**（github vs gitee/gitcode） | `config.cppm:90-103` |
| **代理改写** `mirror::` 命名空间 | jsdelivr/ghfast/ghproxy.net/kkgithub | 把 `github.com` URL **改写成第三方代理**，并做延迟重排 | `src/core/mirror/*.cppm` |

二者正交：区域键决定"原始 URL 指向哪个站"，代理改写决定"GitHub URL 失败后换哪些代理"。

### 1.2 主索引 `xim-pkgindex`

- `GLOBAL`: `https://github.com/openxlings/xim-pkgindex.git`
- `CN`: `https://gitee.com/sunrisepeak/xim-pkgindex.git` ✅ **有真实国内镜像**
- 机制：`git clone --depth 1` 到 `XLINGS_HOME/data/xim-pkgindex/`，更新用 `git pull --ff-only`
  →（失败）`git fetch + reset --hard`。非强制时 **7 天节流**（`.xlings-sync-stamp`）。
  代码：`repo.cppm:164-249`、`config.cppm:90-96`。
- 解析后的索引缓存：`.xlings-index-cache.json`，以 **git HEAD 哈希**做有效性校验
  （HEAD 变了才全量重建）。`index.cppm:167-198`。

### 1.3 子索引仓库（用户口中的"子仓库索引"）

- 在主索引根目录的 `xim-indexrepos.lua` 中声明，按区域键取 URL，缺省回退 `GLOBAL`：
  解析器 `repo.cppm:72-143`（手写状态机，不依赖 Lua 解释器/regex）。
- **现状（问题根因）**：
  ```lua
  ["awesome"] = { ["GLOBAL"] = ".../xim-pkgindex-awesome.git",
                  ["CN"]     = ".../xim-pkgindex-awesome.git" },  -- ⚠ CN == GLOBAL == github
  ["scode"]   = { ... 同上 ... },
  ["d2x"]     = { ... 同上 ... },
  ```
  → **CN 用户取子仓库时，区域键这层完全失效，必然落到 github.com。**
- 克隆位置：`XLINGS_HOME/data/xim-index-repos/<name>/`，成功后写回
  `xim-indexrepos.json`。同样走 `sync_repo()`（同一条 git clone 链路）。

### 1.4 三层网络韧性机制（关键在于谁覆盖谁）

| 机制 | 在哪条路径 | 谁享受了 |
| --- | --- | --- |
| **区域键 origin 切换**（github↔gitee/gitcode） | 主索引、`XLINGS_RES` 资源服务器 | 主索引 ✅ / **子仓库 ❌** |
| **代理改写 `mirror::expand`**（静态优先级顺序） | HTTP 下载 + **所有 git clone**（含索引） | 索引/子仓库 ✅（但仅静态顺序） |
| **延迟重排 `adaptive::reorder` + 卡顿看门狗**（0.4.49） | **仅 HTTP 下载器 `tinyhttps`** | 资源下载 ✅ / **索引 git clone ❌** |

**这张表就是结论**：用户最常失败的"取子索引"恰好在每一层都拿到的是最弱档：
没有 origin 镜像、只有静态顺序代理、没有延迟重排、没有卡顿检测。

证据：
- HTTP 下载器：`downloader.cppm` 先 `expand`（:362）再 `adaptive::reorder`（:374）再
  `tinyhttps::download_file`（:414，带卡顿看门狗 + sha256 校验 + `penalize_host`）。
- 索引 clone：`repo.cppm:177` 只调 `mirror::expand(url,{Git})`，然后
  `repo.cppm:184-209` **按静态顺序**逐个 `compact::git::clone_shallow` —
  **无 `adaptive::reorder`、无卡顿看门狗**（git CLI 子进程，也没设超时，依赖 git 默认值）。

### 1.5 劣化网络下的真实体验（为什么"很卡"）

引用 `2026-06-04` 设计文档自述的 tier-3 缺陷，在 git clone 路径上原样成立：

> 一个**握手成功但只有 KB/s** 的连接，既不触发 `connectTimeoutSec=30`，也不触发
> `maxTimeSec=600` 之外的任何东西；最坏情况要 `600s × (1+retry)` 才会尝试第一个镜像。

而卡顿看门狗（`StallDetector`，10KB/s × 15s 触发）只接到了 HTTP 下载器上，
**git clone 收不到**。于是子索引获取 = GitHub 优先 + 可能长时间假死 + 才轮到一串脆弱免费代理。

### 1.6 bootstrap 安装器（独立的一套，已较完善）

`tools/other/quick_install.sh`（commit `06fd09f`）与 `.ps1`：GitHub + GitCode 双源，
用 `curl -w '%{time_total}'` 做延迟探测，按"版本优先 → 延迟次之"排序，下载校验 gzip 魔数后回退。
这套逻辑**独立于运行时 C++ 链路**，不共享代码。它证明团队已认可"多源+探测+回退"范式 —
只是运行时索引路径还没补齐。

---

## 2. 行业最佳实践（索引分发，非二进制分发）

跨生态系统的结论高度一致，且**没有一个**是"对 GitHub 做 live git clone"。

### 2.1 索引分发形态的演进：git → 静态 HTTP

| 系统 | 旧 | 新 | 启示 |
| --- | --- | --- | --- |
| **Homebrew** | tap 全量 git clone，`brew update` = `git pull` | **4.0 (2023.02) 改为 JSON-over-CDN**：`formulae.brew.sh/api/formula.jws.json`（JWS 签名），自动更新频率 5min→24h | 单一巨型 blob（~31MB）在弱网下"全有或全无"反而易失败（issue #15443/#21622）→ **不要用单体索引** |
| **Cargo** | `crates.io-index` 全量 clone | **稀疏 HTTP 索引（1.70 默认）**：`sparse+https://index.crates.io/`，按名分片一包一文件，`ETag`+`If-None-Match`→`304` | 纯静态文件、可放 CDN、只取依赖树涉及的包；国内 `rsproxy.cn` 提供 `sparse+https://rsproxy.cn/index/` |
| **Go modules** | — | **GOPROXY 协议**（不可变静态文件 `/@v/*`）+ **GOSUMDB** 传输无关校验；`GOPROXY=https://goproxy.cn,direct` | "完整性独立于谁送的字节"——恶意镜像无法投毒。**金标准** |
| **npm/pip** | — | 逐包元数据 + 内容协商精简格式；国内 `npmmirror.com` / 清华 PyPI（bandersnatch 全量同步） | 纯主机名替换 + 保留原始哈希 = 镜像安全 |
| **Debian/apt** | — | **签名 `InRelease` 哈希所有子索引，子索引哈希所有 .deb**；一个签名验证整库 | 镜像可完全不可信；`by-hash` 让同步原子化 |

### 2.2 镜像选择：服务端 CDN vs 客户端探测

- **Debian `deb.debian.org`**：Fastly/CloudFront CDN 前置 + DNS SRV 就近，**客户端零配置**。
- **Ubuntu `mirror://`**：GeoIP 列表 + 故障转移（只看地理，不测速）。
- **Arch `reflector`**：主动排名，`--sort rate`（真实下载测速）vs `--sort age`（新鲜度）。
  **关键洞察：延迟 ≠ 新鲜度**——快但陈旧的镜像会过哈希/`Valid-Until` 校验失败。
- **Happy Eyeballs (RFC 8305)**：先连第一个，~250ms 后并行连下一个，先连上者胜。
  与探测互补：**探测排序 + 竞速前 N 个**。

### 2.3 中国 GitHub 绕行方案：每一个免费的都死过

> 规律：免费服务 → 流量/滥用爆炸 → 成本/政策压力 → 关停或封禁大仓。**不要硬编码任何单一免费代理。**

| 方案 | 状态/权衡 |
| --- | --- |
| **ghproxy 系**（`ghproxy.net`/`gh-proxy.com`…） | 原 `ghproxy.com` 已死；2022 起封禁 top 仓库（曾 ~1TB/天）；整体高churn |
| **gh-proxy 自建**（Cloudflare Worker） | **可持续的答案**：自己控制，免费层 ~10万请求/天；无不可信中间人 |
| **jsDelivr** | 全球优秀且可锁版本，**但 ~2021-12 失去中国 ICP** → 国内 DNS 污染/TCP reset，**国内不可依赖** |
| **FastGit** | **已关停**（注意：最终告别公告是 **2024-07-06**，非 2022） |
| **Gitee 镜像** | ICP 备案、域内稳定；但强制覆盖、**最小 5min 同步间隔/30min 超时/无 LFS**、自动同步实践中不稳、Pages 需人工审核 |
| **TUNA/USTC/阿里/腾讯 镜像站** | 最持久（机构、ICP、rsync）；**但只镜像策展过的发行版/语言上游，不是任意 GitHub 仓库**。可申请收录，但偏好成熟项目 |

xlings 现用 **Gitee + GitCode**（都是 ICP 备案的域内 git 站）作为 CN origin —— 这是**正确**的
"稳定域内 origin"选择，优于脆弱的免费代理。

### 2.4 完整性：让所有镜像都可以不被信任（最缺的一层）

一旦从不受控的镜像/代理取内容，**TLS 不够**，需要端到端签名/哈希元数据：

- **校验清单 + minisign**（Ed25519，极简，仅签名）→ 最低门槛、最契合
- **sigstore/cosign**（无长期密钥，OIDC 身份 + Rekor 透明日志）→ 中等
- **完整 TUF**（root/targets/snapshot/timestamp，防回滚/冻结/混搭）→ 最复杂
- **内容寻址**（用哈希命名索引文件）→ 完整性内生、缓存天然安全；只需对一个小的"根指针"签名

xlings 通过不受信代理拉取的是**可执行的 Lua `install()` 配方** —— 完整性缺失的安全敞口比一般索引更大。

---

## 3. xlings 的具体差距（按研究映射）

| # | 差距 | 证据 | 对应行业实践 |
| --- | --- | --- | --- |
| **G1** | **子索引仓库无国内镜像**（CN==GLOBAL==github） | `xim-indexrepos.lua` | 主机名替换镜像（npm/pip/apt 国内站） |
| **G2** | **索引 git clone 不做延迟重排、无卡顿看门狗** | `repo.cppm:177-209` 只 `expand` 无 `reorder`；watchdog 仅在 `tinyhttps` | Arch reflector `--sort rate` / Happy Eyeballs |
| **G3** | **代理列表编译进二进制会腐烂**（kkgithub 已死/jsdelivr 国内失效） | `registry.cppm:36-69`；热更新被推迟 | 数据驱动 + 远程可更新镜像表 |
| **G4** | **git clone 不可断点续传**，弱网下断 = 从零重来 | `compact::git::clone_shallow` | tarball Range / Cargo 稀疏文件 |
| **G5** | **索引未签名/未内容寻址**（配方是可执行 Lua） | 无对应代码 | apt InRelease / Go GOSUMDB / minisign |
| **G6** | 7 天节流 + HEAD 哈希缓存，无 ETag 条件请求 | `repo.cppm:215-227`、`index.cppm:177` | Cargo `If-None-Match`→304 |

---

## 4. 优化方向（分级，按"性价比/侵入性"排序）

### 第 0 级：运维即可修，零代码，立即见效
- **O1（最高优先）给子索引仓库配置真实 CN 镜像**：把 `xim-indexrepos.lua` 中三个子仓库的
  `CN` 改为 Gitee/GitCode 镜像（与主索引同源策略），并建立 GitHub→Gitee 的自动同步
  （GitHub Action 推送，规避 Gitee 自动同步的 5min/不稳问题）。
  —— 这一步直接消除 G1，是覆盖面最大、改动最小的修复。

### 第 1 级：小改动，把已有能力补到索引路径（消除"最弱档")
- **O2 让索引 clone 享受延迟重排 + 卡顿检测**：
  - 在 `repo.cppm` clone 循环前调用 `mirror::adaptive::reorder(urls, /*has_sha256=*/false)`，
    失败时 `penalize_host()`（`reorder` 已是通用函数，几乎零新代码）。消除 G2 的"静态顺序"。
  - git CLI 无法接 `StallDetector`，但可给 clone 子进程加**总超时 + 低速放弃**
    （`git -c http.lowSpeedLimit=… -c http.lowSpeedTime=…`，或外层 watchdog 杀进程后换下一个候选），
    把"600s 假死"压到秒级。消除 G2 的"无卡顿检测"。
- **O3 代理表数据驱动 + 可远程更新**：把 `registry.cppm` 默认表标注"易腐烂"，
  并允许从已成功取到的索引仓库里附带一份 `github-mirrors.json` 做热更新（先有 origin 才更新代理表，
  规避鸡生蛋）。清掉已死的 kkgithub，国内场景下调 jsdelivr 优先级。消除 G3。

### 第 2 级：中等架构改动，对齐 Cargo/Go 范式（弱网根治）
- **O4 索引改为可断点续传的传输**：对"一次性、克隆完即可丢历史"的索引，用
  **commit 锁定的 tarball**（`/archive/<commit>.tar.gz`，HTTP Range 可续传、可走 CDN）替代
  非续传的 git clone；用 commit ID（而非 tarball 哈希，后者不稳定）锁定完整性。消除 G4。
- **O5 引入 ETag/`If-None-Match` 条件请求**：索引/资源的 HTTP 路径加条件请求，命中 304 零体积，
  比 7 天节流更精准且实时。补强 G6。

### 第 3 级：长期，安全与可扩展性
- **O6 索引签名/内容寻址**：对索引（或一份校验清单）做 **minisign** 签名（最低门槛），
  使任意镜像/CDN/代理都可不被信任；这是把整个镜像生态"安全地"放开的前提。消除 G5。
- **O7（远期）稀疏索引化**：若包数量持续增长，参考 Cargo 稀疏 HTTP 索引/Homebrew 逐包 JSON，
  按需取元数据 + 静态 CDN，彻底摆脱"整库 clone"。

### 推荐落地顺序
**O1 → O2 → O3**（覆盖绝大多数当前抱怨，改动小、风险低）→ 再评估 O4/O5 → O6 长期排期。

---

## 5. 诚实标注的核查注意点
- **FastGit** 最终关停为 **2024-07-06**（非 2022，早期有 2021–22 中断）。
- **Gitee** "全局禁用自动更新"无法证实；可证实的是限速（最小 5min）+ 自动同步不稳 + Pages 人工审核。
- **jsDelivr** 国内失效（ICP ~2021-12 吊销）证据充分；全球仍稳健。

## 关键文件索引
- 索引/子仓库同步：`src/core/xim/repo.cppm`（`sync_repo` :164、子仓库解析 :72-143、clone 循环 :177-209）
- 区域键 origin / 资源服务器：`src/core/config.cppm:90-103,346-380`
- 代理改写：`src/core/mirror/{registry,forms,expand,adaptive,types}.cppm`
- HTTP 下载器（已含 reorder+watchdog）：`src/core/xim/downloader.cppm:362,374,414`
- 解析索引缓存：`src/core/xim/index.cppm:167-198`
- 子索引配置：`<index>/xim-indexrepos.lua`
- bootstrap 安装器多源探测：`tools/other/quick_install.{sh,ps1}`
