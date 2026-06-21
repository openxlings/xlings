# xlings 索引与更新机制 — 重设计方案(深度分析 + 前沿调研)

> 日期: 2026-06-21
> 类型: 架构提案 (design proposal)
> 前置: `2026-06-21-pkgindex-mirror-analysis.md`(现状/问题/行业实践)
> 关联: mcpp 索引模型分析、前沿调研(OCI/Sigstore/TUF/Nix/边缘代理)
> 一句话: **要重设计,但是"靶向重设计"** —— 换掉索引的*获取与传输方式*(live git clone → 静态签名目录 + CDN + 多镜像),
>          保留 git 作为*事实源*,补上*内容寻址 + minisign 签名 + 锁文件*。**不做** OCI/TUF/IPFS 的大改。

---

## 0. 先回答那个问题:要不要重设计?

**要,但范围必须精确。** 把"重设计"拆成三个正交的轴,逐一给结论:

| 轴 | 现状 | 是否重设计 | 结论依据 |
| --- | --- | --- | --- |
| **A. 索引的获取与传输** | 对 GitHub 做 live `git clone`(1.7MB 仓库 + 全量历史 + 不可续传)只为读 13KB 目录 | ✅ **是,这是核心** | 行业无一例外:没有主流 PM 对 GitHub live-clone 取索引;且这正是用户卡顿的根 |
| **B. 索引的完整性与可复现** | 无锁文件、无内容寻址、仅 ~8% 配方有 sha256 | ✅ **是,中优先级** | mcpp 已 PoC(`mcpp.lock`+`fnv1a`);Nix/Go/apt 都做传输无关完整性 |
| **C. 索引的存储/分发协议范式** | git 仓库 of `pkgs/*.lua`(xpkg V1) | ❌ **不大改**(git 留作事实源) | **67 个包、13KB 目录** —— Cargo 稀疏/OCI/TUF 都是为 10万级+ 设计,对此体量是过度设计 |

**最关键的单一事实(决定一切):** xlings 整个索引 = **67 个配方、工作树 1.7MB、解析后目录仅 13KB**。
前沿调研结论一致:"git-as-index 只在**大规模**才崩"(Cargo/Homebrew/CocoaPods/Go 的迁移全发生在十万级)。
所以 xlings 不需要稀疏索引,更不需要 OCI/TUF —— 它需要的只是**别再 clone 一个仓库去读一个 13KB 文件**。
前沿正确解(content-addressed + 静态 CDN)和最简解(一个签名 JSON)在这个体量上**罕见地重合**。

---

## 1. 为什么 live-git-clone 是错的范式(而非实现细节)

1. **不可断点续传**:git-over-HTTPS 是单条 packfile 流,弱网断 = 从零重来(前沿调研 A3)。
2. **拿不到 CDN**:GitHub git 协议不可被 jsdelivr/CDN 缓存;只能整库代理(脆弱免费代理)。
3. **拿不到 0.4.49 的韧性**:延迟重排 + 卡顿看门狗只在 HTTP 下载器,git clone 收不到(见现状文档 G2)。
4. **为 13KB 付 1.7MB + 全量历史的代价**,且子仓库还没有国内 origin(G1)。

行业反证(全部从 git-index 迁走或从不用):
- **Homebrew 4.0**:tap git clone → `formula.jws.json`(JWS 签名)over CDN,更新频率 5min→24h。
- **Cargo 1.70**:`crates.io-index` git clone → 稀疏 HTTP 索引 + ETag/304。
- **Go**:GOPROXY 不可变静态文件 + GOSUMDB 传输无关校验。
- **mise/aqua/vfox**(与 xlings 最像的小型工具):索引**烘焙进二进制** / 单个 ref 锁定的静态文件 / GitHub Pages 静态索引。

---

## 2. 前沿调研:哪些是真前沿,哪些是陷阱

前沿调研的最大价值是**划清"该抄"与"别碰"的边界**:

### 值得采纳(便宜且对路)
- **内容寻址 = 配方里每个下载项带 sha256**(Nix/Zig 思路)。这是 OCI 真正有价值的部分(verify-by-digest),
  但**零协议成本**:任意镜像/代理/CDN 都能送字节,无需信任关系。mcpp 配方已经这么做了,xim-pkgindex 只有 ~8%。
- **条件请求**(ETag / If-Modified-Since)的静态索引:刷新近乎零成本。
- **可脚本化的镜像改写层**(xmake `mirror(url)` 钩子,2.5.4 起):xlings 配方本就是 Lua,天然契合。
- **Nix 二进制缓存模型(借思路不借机器)**:静态 HTTP + 按内容哈希命名 + 每缓存固定公钥 + 多 substituter 有序回退。
- **minisign 签名**(单 Ed25519 密钥、单静态二进制、纯离线):小团队的完整性最优解。

### 明确不做(对 xlings 是过度设计 / 对中国用户有害)
- **OCI-registry-as-index**:连 Homebrew 都无法让镜像代理其 OCI 路径(镜像一律回退到 flat-file);
  **中国 2024-07 后已无可靠的 ghcr.io 公共拉取缓存**(阿里公共加速器 2024-07-02 起仅限阿里云内网)。
  强行用 OCI = 强迫客户端讲 token-auth manifest/blob 协议,且国内更慢。
- **TUF**:PyPI 的 PEP 458 接受多年仍未部署;RustSec 仍在 RFC 阶段。对 67 个包是纯负担。
- **Sigstore keyless**:依赖在线 Fulcio/Rekor + OIDC,适合"只从 CI 发布"的项目;签名事件公开、离线尴尬。
  aqua 实测:"支持 Cosign,但几乎没人发布签名"。**除非 xlings 改为纯 CI 发布,否则用 minisign。**
- **IPFS**:npm/pacman/Nix 全部弃用,实测 DHT 取记录平均 ~6s(最坏 ~1.5h),且最终仍走中心化网关。**不要重访。**

---

## 3. 目标架构

分层设计,每层可独立演进:

```
┌─ L0 事实源 (Source of Truth) ────────────────────────────────────────┐
│  git 仓库 xim-pkgindex (+ awesome/scode/d2x):pkgs/*.lua, xpkg V1     │
│  贡献者 PR / review 仍走 git —— 不变                                   │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ CI on push to main
┌─ L1 构建&发布 (Publish) ──────▼───────────────────────────────────────┐
│  CI 跑 xpkg::build_index → 产出 catalog.json(≈13KB,压缩后 ~3KB)      │
│  + minisign 签名 catalog.json.minisig + 版本号/commit 戳              │
│  发布到多个静态分发面(L2),内容按 commit/hash 不可变                  │
└──────────────────────────────┬───────────────────────────────────────┘
┌─ L2 分发面 (Distribution surfaces,全部静态/可 CDN/可续传) ───────────┐
│  GLOBAL: GitHub Release asset / GitHub Pages / jsdelivr(全球)        │
│  CN:     Gitee Pages / GitCode / Cloudflare R2(零出口费,含中国 PoP) │
│  可选:   烘焙进 xlings 二进制(mise 模式)作为离线/首启兜底           │
└──────────────────────────────┬───────────────────────────────────────┘
┌─ L3 客户端获取 (Client fetch) ▼───────────────────────────────────────┐
│  HTTP GET catalog.json(走 tinyhttps:已有 续传/卡顿看门狗/延迟重排)   │
│  → minisign 验签 → 写入本地 cache + ETag                               │
│  多镜像按延迟探测排序(adaptive::reorder,已存在)+ 失败 penalize     │
│  条件请求 If-None-Match → 304 则零体积                                 │
│  git clone 降级为 *兜底*(静态面全挂)和 *贡献者* 路径                 │
└──────────────────────────────┬───────────────────────────────────────┘
┌─ L4 完整性&可复现 ────────────▼───────────────────────────────────────┐
│  catalog 级:minisign 签名(传输无关,任意镜像可不可信)               │
│  包级:配方强制 sha256(verify-by-digest,逐候选校验,已有机制)       │
│  可选 xlings.lock(借 mcpp.lock 模型):项目级固定 commit + 解析哈希   │
└───────────────────────────────────────────────────────────────────────┘
```

### 3.1 关键设计决策

1. **索引 = 预构建的单个静态 `catalog.json`,不是被 clone 的 git 工作树。**
   `xpkg::build_index` 已存在(现在客户端本地跑);把它**移到 CI 发布期**跑一次,产物供所有人下载。
   13KB 目录 → 一次 HTTP GET,可 CDN、可续传、可 ETag。clone 假死问题从根消失。
2. **子索引(awesome/scode/d2x)同等对待**:各自发布自己的 `catalog.json`,主索引的
   `xim-indexrepos.json` 改为指向"各子索引 catalog 的多分发面 URL 列表",而非 git 仓库。**直接消除 G1**。
3. **git 仍是 L0 事实源**:贡献、review、历史不变;只是客户端**默认不再 clone**。
   "想要完整配方源码/本地改"的用户仍可 `xlings index clone`(显式)。
4. **完整性双层**:catalog 用 minisign 签名(替代"信任 git origin");包下载用 sha256
   (推动 xim-pkgindex 把 sha256 覆盖率从 8% 拉到接近 100%,可加 index lint 在 CI 拦截无 sha256 的配方)。
5. **中国策略是真正该投入工程的地方**(前沿调研明确:客户端延迟探测在别处已被 CDN anycast 淘汰,
   但在中国"异构、各自为政、随时挂的镜像"场景下**仍然正当**,只要保持便宜——周期探测/竞速一小撮,而非每次下载):
   - L2 把 catalog 同时放在 Gitee Pages / GitCode / R2(含中国可达面),CN 区域键直接选域内面。
   - 保留 `mirror::expand` + `adaptive::reorder`,但现在作用在**可被 CDN 缓存的 HTTP URL** 上(比代理 git 强得多)。
   - 代理表数据驱动 + 可热更新(随 catalog 一起下发 `github-mirrors.json`);清掉已死的 kkgithub。
   - 大学镜像(`ghcr.nju.edu.cn`/USTC 类)是最持久的一档;所有社区 ghproxy 视为随时失效。

---

## 4. 与 mcpp 的关系:复用而非重造

调研确认:**mcpp 的索引存储和 xlings 完全相同(都是 xpkg V1 的 git 仓库),mcpp 的 registry 就是个嵌套的 xlings home。**
mcpp 真正多出来的是**可复现/锁定层**,而这层正是 xlings 运行时文档早已列为"待办"的 `xlings.lock`:

- `mcpp.lock`(`version=2`):每依赖 `source = index+ns@version` + `hash = fnv1a:...`(解析级)+ 配方里 `sha256`(字节级)。
- mcpp 配方**每个包都带 `GLOBAL`/`CN` 双 URL + sha256**(如 ftxui CN→`gitcode.com/mcpp-res/...`)——
  这正是 xim-pkgindex 该补的(当前仅 8% 有 sha256,且子仓库无 CN URL)。

**结论:L4 的可复现层直接照搬 mcpp 模型(`xlings.lock` + `index+ns@version` + 解析哈希),不必另起炉灶。**
两者共享同一索引引擎,统一是加一层 lock,而非换引擎。旧文档 `2026-05-01-ci-self-host-via-xlings-json.md:254`
已明确预留了这个 `xlings.lock`。

---

## 5. 迁移路线(分阶段,低风险优先,与现状文档的 O1–O7 对齐)

| 阶段 | 内容 | 风险 | 对应问题 | 价值 |
| --- | --- | --- | --- | --- |
| **P0 运维** | 给子索引配 CN 镜像 + GitHub→Gitee 自动同步(Action 推送) | 极低 | G1 | 立刻止血,覆盖最大 |
| **P1 补韧性到 git 路径** | 索引 clone 前加 `adaptive::reorder`+`penalize_host`;给 git 子进程加低速放弃/总超时;代理表去死链 | 低 | G2/G3 | 在不改范式前先把"假死"压到秒级 |
| **P2 静态 catalog(核心重设计)** | CI 发布 `catalog.json`+minisign;客户端改 HTTP 拉取+验签+ETag;git 降级为兜底/贡献者路径;子索引同构 | 中 | A/G1/G4 | 从根上消除 clone 假死;拿到 CDN/续传 |
| **P3 完整性收口** | 配方 sha256 覆盖率→~100%(CI lint 拦截);catalog 内嵌包级 digest | 中 | B/G5 | 任意镜像可不可信 |
| **P4 可复现(借 mcpp)** | 引入 `xlings.lock`(`index+ns@version`+解析哈希) | 中 | B | 与 mcpp 统一,CI 可复现 |
| **P5 可选** | catalog 烘焙进二进制(离线首启);R2 自建边缘面 | 低-中 | A(离线) | mise 式零网络查找 |

**推荐:P0→P1 先上(止血,几乎纯收益),再做 P2(真正的范式切换),P3/P4 随后,P5 按需。**

---

## 6. 权衡与取舍(决策矩阵)

| 维度 | 现状 git-clone | 静态 catalog+CDN(本提案) | OCI-registry(否决) |
| --- | --- | --- | --- |
| 弱网/中国可靠性 | 差(假死、无续传、无 CDN) | **好**(续传、CDN、多域内面) | 差(国内无 ghcr 缓存) |
| 实现复杂度 | 低(已存在) | **中**(CI 发布 + 验签) | 高(token-auth 协议) |
| 完整性 | 弱(信 origin,8% sha256) | **强**(minisign + sha256) | 强(digest,但协议重) |
| 离线/首启 | 差(必须联网 clone) | **好**(可烘焙进二进制) | 差 |
| 贡献者体验 | 好(就是 git) | 好(L0 仍是 git) | 差(要推 registry) |
| 适配 67 包体量 | 杀鸡用牛刀(整库历史) | **正好** | 牛刀杀蚊子 |

---

## 7. 开放问题 / 风险

1. **catalog 的"最新指针"如何更新**:不可变内容(按 commit/hash)+ 一个短 TTL 的小 `latest.json` 指针;
   指针也要可验签,避免 CDN 缓存导致新包不可见。
2. **minisign 密钥保管**:单长期密钥的离线保管/备份/分发是 minisign 唯一的硬成本;公钥需随 xlings 二进制内置。
   若未来改纯 CI 发布,可再评估 Sigstore(届时才划算)。
3. **Gitee Pages 需人工审核、5min 同步间隔**:用 Action 主动推送 + R2 兜底,降低对 Gitee 自动同步的依赖。
4. **向后兼容**:老版本 xlings 仍 clone git;L0 保留即可继续工作,L2 是增量增强。
5. **catalog schema 版本化**:`.xlings-index-cache.json` 已有 `version:1`,沿用并加签名包裹。
6. **子索引发现的鸡生蛋**:主 catalog 内嵌子索引 catalog 的多面 URL 列表,客户端先取主 catalog 再并行取子。

---

## 7.5 索引更新机制(核心实现细节)

一旦索引从 git 仓库变为 CDN 静态文件,更新就不是 `git pull`,而要自己解决 5 件事:
**①便宜地发现变化 ②不可变负载与可变指针共存 ③验签防回滚 ④原子落盘 ⑤失败兜底**。

### 7.5.1 两文件模型(指针 + 内容寻址负载)

借 apt `InRelease`(签名清单哈希所有子索引)+ Go GOSUMDB(传输无关完整性)的思路:

```
index.json            ← 指针/清单:小、可变、短 TTL、必签名
  ├ schema_version: 1
  ├ index_version: 1234            # 单调递增整数(CI run / commit count)
  ├ commit: "<git sha>"            # 可追溯回 L0 事实源
  ├ generated_at: "<RFC3339>"      # 防冻结攻击(陈旧指针告警)
  ├ catalog: { hash: "sha256:ab…", size: 3012,
  │            urls: ["<面1>/catalog-ab….json.zst", "<面2>/…"] }
  ├ subindex: {                    # 一个指针覆盖主索引 + 全部子索引
  │   awesome: { hash, size, urls },
  │   scode:   { … }, d2x: { … } }
  ├ mirrors: { … }                 # 可选:热更新 github-mirrors.json(去死链)
  └ (detached) index.json.minisig  # minisign 签名

catalog-<hash>.json.zst  ← 负载:不可变、按内容哈希命名、Cache-Control: immutable
                            任意 CDN 永久缓存,无需单独签名(指针已为其哈希背书)
```

**关键点**:负载按 hash 命名 → 永不失效、可被任意 CDN/镜像缓存、无需逐个签名;
只有那个**小指针**是可变的、需短 TTL + 验签。这把"CDN 缓存导致新包不可见"的经典坑,
收敛成"只需正确管理一个 3KB 指针的缓存"。

### 7.5.2 客户端更新流程(替代 `git pull --ff-only`)

```
1. 按区域键解析指针的多分发面 URL 列表(CN→gitee/gitcode/R2,GLOBAL→pages/jsdelivr)
   → mirror::adaptive::reorder() 按延迟排序(已有)
2. 条件请求 GET index.json,带 If-None-Match: <本地ETag>
   ├ 304 Not Modified → 结束(零体积,这就是新的"freshness check")
   └ 200 → 继续
3. minisign 验签(公钥内置于 xlings 二进制)。失败 → 拒绝,换下一个面
4. 防回滚:if index_version <= 本地版本 → 忽略(除非 --force)
   防冻结:if now - generated_at > 阈值 → 告警(某个面在喂陈旧指针)
5. 比对 catalog.hash 与本地缓存 hash
   ├ 相同 → 仅指针变了(如 mirror 列表)→ 更新本地指针即可,不下负载
   └ 不同 → 从 catalog.urls(内容寻址,CDN 命中)下载 catalog-<hash>.json.zst
            走 tinyhttps(已有:续传 + 卡顿看门狗 + 逐候选回退)
6. 校验 sha256(下载内容) == catalog.hash。不符 → 弃用,换下一个 URL
7. 子索引:对 subindex 中 hash 变化的项,并行重复 5–6(只取变化的)
8. 原子落盘:写临时文件 → fsync → rename 覆盖;记录 {index_version, etag, hash}
   到状态文件。任何步骤失败 → 保留 last-good,降级继续可用
9. 全部静态面失败 → 回退到现有 git clone 路径(永不比今天更差)
```

### 7.5.3 为什么"不做增量/delta"是正确的

负载压缩后 ~3KB。**整文件重下比算/应用 delta 更便宜**——这是小体量带来的简化红利,
**刻意的非目标**。若未来涨到 MB 级再考虑 Cargo 式按包稀疏拉取(`subindex` 结构已为此预留)。

### 7.5.4 freshness / 节流(替代 7 天 git stamp)

- `xlings update`:总是走第 2 步条件请求(304 极廉价,可以频繁)。
- `xlings install`:最多每 N 小时查一次指针(stamp 节流);因为是 304 条件请求而非整库 clone,
  N 可远小于现在的 7 天(参考 Homebrew 24h,可设 6–24h,`--refresh` 强制)。
- 命中 304 = 几十字节;指针真变了才下 3KB 负载;负载没变只是指针动了则零负载。

### 7.5.5 发布侧(CI,merge 到 main 触发)

```
1. xpkg::build_index 产出 catalog.json(已有逻辑,从客户端移到此处)→ zstd 压缩
2. hash = sha256(catalog.json.zst);命名 catalog-<hash>.json.zst
3. 生成新 index.json:index_version+1、commit、各 subindex 的 hash/size/urls
4. minisign 签 index.json(私钥在 CI secret;或人工离线签更稳)
5. 上传到所有分发面:
   - 负载:Cache-Control: public, max-age=31536000, immutable
   - 指针:Cache-Control: public, max-age=300, must-revalidate(短 TTL)
6. 保留旧负载(不可变、便宜)→ 支持回滚 / xlings.lock 复现
```

### 7.5.6 与本地缓存 / xlings.lock 的衔接

- 现有 `.xlings-index-cache.json`(`version:1`,以 git HEAD 为缓存键)→ 缓存键改为
  **指针里的 catalog.hash**;HEAD 概念不再需要(已无本地 git 工作树)。
- `xlings.lock`(借 mcpp 模型)记录 `index_version` + `catalog.hash` + 各 `subindex.hash`
  → `xlings install --locked` / CI 可**精确复现**当时的索引状态(因负载不可变、永久保留)。

### 7.5.7 这套设计对应的攻击/故障防护

| 威胁/故障 | 防护 | 出处范式 |
| --- | --- | --- |
| 镜像/代理篡改内容 | minisign 签指针 + sha256 校负载 | apt InRelease / Go GOSUMDB |
| 回滚到旧索引(降级攻击) | `index_version` 单调 + 拒绝更低 | TUF rollback 防护(最小版) |
| 冻结/喂陈旧指针 | `generated_at` + 过期告警 | TUF timestamp 角色(最小版) |
| CDN 缓存致新包不可见 | 负载内容寻址不可变 + 指针短 TTL | jsdelivr 不可变 URL + 短 TTL 指针 |
| 半更新/损坏 | 临时文件 + fsync + rename 原子替换 | 通用 |
| 全网失败/离线 | 保留 last-good + git 兜底 + 可烘焙进二进制 | Nix 多 substituter 回退 |

---

## 7.6 落地所需的全部工作(不止"合入代码发版本")

**结论先行:仅"合入 xlings 代码 + 发新版本"不够。** 那只覆盖下表 D 类(二进制改造)。
方案要真正生效,还依赖 A 密钥/信任根、B 分发面基础设施、C 索引仓库的发布管线、E 索引内容,
其中相当一部分是**密钥/账号/运维/内容**工作,写代码无法替代。涉及 **2 类共 5 个仓库**:
xlings 二进制仓(openxlings/xlings)+ 索引内容仓(xim-pkgindex 主仓 + awesome/scode/d2x 三个子仓)。

### 7.6.1 全部工作清单

| 类 | 事项 | 是代码? | 一次性/持续 | 所在仓库/位置 | 前置依赖 |
| --- | --- | --- | --- | --- | --- |
| **A 密钥/信任根** | 生成 minisign 长期密钥对(离线) | 否(运维) | 一次性 | 离线/安全环境 | 无 — 最先做 |
| A | 私钥保管/备份/轮换流程 | 否(运维) | 一次性+持续 | CI secret 或离线签名机 | A 密钥 |
| A | **公钥内置进 xlings 二进制** | 是 | 一次性 | xlings 源码 | A 密钥 |
| A | 定签名策略:CI 自动签 vs 人工离线签 | 否(决策) | 一次性 | — | A 密钥 |
| **B 分发面/基础设施** | 选定并开通静态面(GitHub Pages/Release、Gitee Pages、GitCode、Cloudflare R2) | 否(运维/账号) | 一次性+持续(托管) | 各平台账号 | 无(可并行) |
| B | 配置 Cache-Control(负载 immutable / 指针短 TTL) | 否(配置) | 一次性 | R2/CF 可配;**Gitee Pages 缓存不可控需评估** | B 开通 |
| B | (可选)自定义域名 + 中国 PoP CDN + DNS | 否(运维) | 一次性+持续 | DNS/CDN 商 | B 开通 |
| **C 发布管线** | 主索引 CI:build catalog→zstd→hash→签→上传所有面 | 是 | 一次性 | **xim-pkgindex 仓**(非 xlings 仓!) | A 私钥、B 面、D `build_index` 可独立运行 |
| C | 子索引 CI:同上 ×3 | 是 | 一次性 | awesome/scode/d2x 三仓 | 同上 |
| C | index_version 来源 + 旧负载保留策略 | 是 | 一次性 | 索引仓 CI | C 管线 |
| **D xlings 二进制** | 新增 `sync_via_catalog()`(指针拉取+验签+原子落盘),与 git 兜底并存 | 是 | 一次性 | xlings 源码 | A 公钥 |
| D | minisign 验签模块 | 是 | 一次性 | xlings 源码 | A 公钥 |
| D | 缓存键 HEAD→catalog.hash;状态文件;freshness 节流改造 | 是 | 一次性 | xlings 源码 | — |
| D | 区域键解析指针多面 URL(复用 config.cppm GLOBAL/CN) | 是 | 一次性 | xlings 源码 | B 面 URL 定稿 |
| D | (可选)`xlings.lock`、catalog 烘焙进二进制 | 是 | 一次性 | xlings 源码 | D 主体 |
| **E 索引内容** | 配方 sha256 覆盖率 8%→~100% | 否(内容) | 一次性+持续 | xim-pkgindex + 三子仓 | 无(可并行) |
| E | 子索引 `xim-indexrepos.lua` 的 CN 改域内镜像 + 自动同步(=P0) | 否(内容/运维) | 一次性+持续 | xim-pkgindex | 无 — 可立即做 |
| E | index lint:CI 拦截无 sha256 的配方 | 是 | 一次性 | 索引仓 CI | E sha256 补齐后 |
| **F 协调/发布顺序** | 灰度开关(默认 git,逐步切 catalog) | 是 | 一次性 | xlings 源码 | D |
| F | L0 git 永久保留(老客户端兼容) | 否(策略) | 持续 | 索引仓 | — |
| F | 回滚预案(指针/版本回退) | 否(运维) | 一次性 | 索引仓 CI + 文档 | C |
| **G 文档** | 镜像配置文档、贡献者 sha256 要求、密钥轮换文档 | 否(文档) | 一次性 | docs | — |

### 7.6.2 强制发布顺序(鸡生蛋问题)

这是最容易踩坑的地方——**信任根必须先于被签名的内容存在**:

```
① A 生成密钥 + 公钥进二进制 + B 开通分发面            ← 基础设施,可并行
② 发一版 xlings:带验签能力,但默认仍走 git(F 灰度=off)  ← 公钥先铺到用户手里
③ C 在索引仓建管线,发布"首个"签名 catalog + 指针到各面
④ E 把 sha256 补齐(否则包级完整性形同虚设)
⑤ 灰度:F 开关切到 catalog 优先(git 兜底);观察各面命中/失败
⑥ 默认 catalog 优先;git 降级为兜底/贡献者路径
   E 的 P0(子索引 CN 镜像)可在 ①–⑥ 任意时刻独立先上(纯收益)
```

要点:
- **步骤②先于③**:若先发签名 catalog 再发带公钥的二进制,老客户端无法验签;反之公钥先铺开,
  老客户端忽略 catalog 继续走 git,新 catalog 出现时已有验签能力。
- **发布管线在索引仓(C),不在 xlings 二进制仓(D)**——这是"只发 xlings 版本不够"的根本原因:
  catalog 的产出与上传由 xim-pkgindex / 子仓的 CI 负责。
- **B 的分发面 URL 必须在 D 编译期可知**(区域键里写死或可配),否则二进制不知道去哪取指针。

### 7.6.3 "纯代码即可" vs "需额外工作"一句话区分

| 如果只… | 结果 |
| --- | --- |
| 合入 D 代码 + 发 xlings 版本 | 客户端**有了验签和 catalog 拉取能力,但无 catalog 可拉** → 仍走 git 兜底,用户无感 |
| 再做 A 密钥 + B 面 + C 管线 | 首个签名 catalog 上线 → 客户端可切 catalog,**clone 假死问题消失** |
| 再做 E sha256 + P0 子索引镜像 | 包级完整性闭环 + 子仓库国内可达 → 方案完整生效 |

---

## 7.7 替代方案 Y:索引随发布捆绑(复用安装器渠道,大幅省步骤)

**问题:既然索引才 13KB,能不能不建独立的 catalog 更新机制,直接让"更新索引 = 重装/取最新发布"?**
**答案:能,而且省掉一整类基础设施工作。** 关键依据:

1. bootstrap 安装器(`quick_install.{sh,ps1}`,commit 06fd09f)**已有**在中国跑通的
   多源(GitHub+GitCode)+ 延迟探测 + 回退 + gzip/zip 魔数校验——这条路**已验证可用**。
2. xlings release **本就同时发在** `github.com/openxlings/xlings` 与 `gitcode.com/xlings-res/xlings`。
3. 索引体量极小 → 不需要 304 增量、不需要稀疏拉取;"整取一个小资源"已经最优。
4. mise 已用此模式(索引烘焙进二进制),aqua 用 ref 锁定的单文件。

### 7.7.1 三档形态

| 档 | 机制 | 更新方式 | 新鲜度 |
| --- | --- | --- | --- |
| **Y-min** | 索引**烘焙进二进制** | 重装 xlings(`self update`) | = 发布节奏;首启/离线零网络 |
| **Y-asset**(推荐) | 索引烘焙进二进制 **+** 同一 release 附带一个独立小资源 `xim-index-<ver>.tar.zst` | `xlings update` 只取那个小资源(走安装器式多源探测),验签后原子替换 | 可独立于二进制发布(见下) |
| **X-full**(§7.5) | catalog + 签名指针 + ETag/304 + 独立 CDN 面 | 指针轮询 | 轮询最省字节,但 13KB 体量收益甚微 |

**推荐 Y-asset**:把索引同时(a)烘焙进二进制做离线/首启兜底,(b)作为一个独立小资源发在
**已有的 GitHub/GitCode Releases 渠道**上。`xlings update` 内部 = "用安装器那套探测逻辑取最新
索引资源 → 校验 → 原子替换本地索引",用户仍只敲 `xlings update`,**无需真的手动重装二进制**,
也**不会**为 13KB 索引去重下整个二进制。

### 7.7.2 新鲜度解耦(Y-asset 的关键)

索引资源在哪发,决定新鲜度是否绑死二进制版本:
- **绑定**:xlings release 管线拉取 pin 的 xim-pkgindex commit,构建 catalog,作为 release 资源附上 →
  新包要等下次 xlings 发版。简单,但索引和二进制版本耦合。
- **解耦(推荐)**:xim-pkgindex / 子仓各自发"索引 release"(把构建好的 `xim-index-<ver>.tar.zst`
  作为 release 资源,镜像到 gitcode `xlings-res`)。`xlings update` 取**最新索引 release**,与二进制版本无关。
  —— 这本质就是把 §7.5 的"分发面"换成**已有的 Releases 渠道**,把"签名指针"换成 **release tag**
  (release 资源天然不可变、安装器探测已会处理),省掉自建 CDN/Pages 与指针机制。

### 7.7.3 Y-asset 相对 X-full(§7.6 清单)省掉/简化了什么

| §7.6 类 | X-full | **Y-asset** |
| --- | --- | --- |
| **B 分发面/CDN/Pages** | 需开通 Gitee Pages/R2/自定义域名 + 配 Cache-Control | **整类省掉** —— 复用已有 GitHub/GitCode **Releases** 渠道 |
| **C 发布管线** | 每个索引仓建 catalog+签名+上传多面 CI | **简化** —— 在已有 release 流程加一步"构建索引资源";无独立 CDN 上传 |
| **D 客户端更新逻辑** | 指针拉取/ETag-304/防回滚/防冻结/比对 hash | **大幅简化** —— "取资源→校验→原子替换";新鲜度=看有无更新 release |
| **A 密钥/签名** | 需对指针单独 minisign 签名层 | **简化/可缓** —— 索引随 release,被 release 的签名/sha256 一并覆盖(本就该签 release) |
| **F 发布顺序(鸡生蛋)** | 公钥必须先于签名 catalog | **缓解** —— 索引与验证它的二进制原子同发,无跨工件时序坑 |
| **E sha256(包级)** | 需要 | **仍需要**(这是包*内容*下载的完整性,与索引分发正交) |
| **E P0 子索引 CN 镜像** | 需要 | **仍建议**;但若子索引也走 release 资源,CN 镜像即 gitcode release,问题一并解决 |

### 7.7.4 唯一真正的代价 + 取舍

- **代价:新鲜度**。捆绑式下索引的"实时性"弱于指针轮询。但:① 13KB,`xlings update` 整取毫无压力;
  ② 用 Y-asset 解耦发布后,新鲜度回到"有没有发新索引 release",与二进制版本无关,代价基本消除;
  ③ 唯一残留:从不 `xlings update` 的用户索引会旧 —— 加一句"索引已 N 天未更新"的提示即可。
- **不解决的(本就正交,两方案都要做)**:包*内容*下载仍走运行时下载器 + 镜像(那条路已有
  延迟重排 + 卡顿看门狗,已 OK);配方 sha256 覆盖率(E)仍要补。
- **子索引(awesome/scode/d2x)**:默认主索引可捆绑;用户*显式*添加的第三方子索引仍需某种取回路径,
  但走同样的 release-资源模式即可,不必为它单独建 CDN。

### 7.7.5 结论:省步骤,且更稳

**Y-asset 用"复用已验证可用的 Releases + 安装器探测"换掉了"自建 CDN 面 + 指针/304/签名"整套机制**,
省掉 §7.6 的 B 类、简化 C/D/A/F,只保留与方案无关的 E。对 13KB、67 包、用户重度在中国的现状,
这是**性价比最高**的形态:工程量小、复用已跑通的中国可达渠道、首启/离线还能零网络。
X-full(§7.5)只在"索引涨到 MB 级 + 需要分钟级实时新鲜度"时才值得回头采用——目前是过度设计。

> 落地顺序相应简化:P0(子索引 CN,可立即)→ 给 release 加"构建索引资源"一步 →
> 客户端 `xlings update` 改为"取索引资源 + 校验 + 原子替换",git 留作兜底 → 补 sha256(E)。
> 不再需要独立 CDN 面、指针轮询、跨工件签名时序。

---

## 7.8 本质:把索引当作"包资源"来分发(统一管线)

**Y-asset 的本质,就是把索引从"一个被 clone 的 live git 仓库"重新定义为"一个有版本、带 sha256、
走镜像分发的可下载工件"——即用和包资源完全相同的方式分发索引。** 这不只是省步骤,而是一次概念统一。

### 7.8.1 一套管线服务两类东西

| 维度 | 包资源(现状已有) | 索引(本方案) |
| --- | --- | --- |
| 分发渠道 | GitHub Release / `xlings-res`(gitcode) | **同上** |
| 版本 | 多版本管理("everything is a package") | **同上**(`xim-index-<ver>`) |
| 完整性 | 配方里 sha256,下载后校验 | **同上** |
| 镜像/容错 | HTTP 下载器 + `adaptive::reorder` + 卡顿看门狗 | **同上** |
| 更新语义 | 安装/升级一个包 | `xlings update` = 取最新索引工件 |

→ **一条分发+完整性+镜像管线同时服务"包"和"索引",概念更少、代码更省。**
这与 xlings 自身哲学("everything is a package")、以及 xim/mcpp 既有实践一致:
`xim:git`、`xim:mcpp@0.0.36` 这些 bootstrap 工具链本就是当作**包**来交付的;`xlings-res` 本就在
当作资源分发 xlings 自己的二进制。把索引也归入这套,是顺理成章的延伸。

### 7.8.2 这恰好治好了根因 G2(优雅闭环)

回顾现状文档:索引走 `git clone`,**收不到** HTTP 下载器上的延迟重排 + 卡顿看门狗(G2),
所以它是韧性最弱的一条路。**而"把索引当包资源下载"会自动把它挪到那条已经具备韧性的 HTTP 路径上**——
G2 不是"再补一遍机制"解决的,而是"换条路走"顺带消失的。这正是 Y-asset 比给 git-clone 打补丁更优的深层原因。

### 7.8.3 唯一不可消除的差异:bootstrap 悖论

索引是**发现一切的根**,所以它和普通包有一处本质不同:**它不能"通过索引来发现自己"**。
因此它需要一个**约定俗成的固定地址**(固定的 release 渠道 URL)或**烘焙进二进制**作为信任/发现锚点——
正如 apt 内置 keyring、cargo 硬编码 crates.io、minisign 公钥内置二进制。
换言之:索引"作为工件"与包资源同构,但"作为发现根"必须有一个不依赖索引的入口。这是唯一的特例。

> 准确边界:这里说的是"索引用包资源的**分发/完整性/镜像管线**来发布",
> 不是"把索引建模成索引里的一条配方"(那会循环)。发现根仍需固定入口。

---

## 7.9 发现锚点:需要固定地址吗?——复用已有的 resource-server,基本不用新建

§7.8.3 的 bootstrap 悖论意味着:索引工件**需要一个不依赖索引、编译期可知的固定地址**。
**但 xlings 已经有这个锚点了** —— 就是现成的 `xlings-res` 资源服务器,因此**不必新发明一套**。

### 7.9.1 现状已存在的固定锚点

`src/core/config.cppm:98-103` 里已经硬编码了一个区域化的资源服务器表:

```cpp
{ "GLOBAL", { "https://github.com/xlings-res" } },
{ "CN",     { "https://gitcode.com/xlings-res" } },
```

而且这套机制(`selected_resource_server_for_`, `config.cppm:346-380`)**已经**:
- 按区域键(GLOBAL/CN)选不同主机 —— **索引的国内镜像免费拿到**;
- 支持**每区多个服务器的 fallback 列表**(`lookup_resource_servers_` 返回列表);
- 对候选服务器做**延迟探测选最快**(≤100ms 早退,memoized)。

它现在已经在干的事:分发 xlings **自己的二进制 release**。把索引工件也放进来,就是同构延伸。

### 7.9.2 所以三个"地址"要分清

| 角色 | 是什么 | 状态 |
| --- | --- | --- |
| **L0 内容事实源** | `d2learn/xim-pkgindex` 等 git 仓(配方源码、PR/review) | 已存在,**不变** |
| **工件分发地址** | 把构建好的 `xim-index-<ver>.tar.zst` 发布到的固定位置 | **复用 `xlings-res`**(github + gitcode 双区) |
| **二进制内的发现锚点** | 编译期写死的 resource-server 表(可被 env/config 覆盖) | `config.cppm:98-103` **已存在** |

### 7.9.3 真正"新建"的极少

- **不需要**:新发现锚点 / 新约定 / 新 CDN 面 —— 锚点、区域化、fallback、延迟探测都已具备。
- **需要**:在 `xlings-res`(github + gitcode 两边)下,给索引工件开一个**固定的发布位**
  —— 一个 `xim-index` 仓库或一条 release 轨道,放 `xim-index-<ver>.tar.zst`;
- **需要**:CI 把构建好的索引工件推到这个发布位(两区同步,gitcode 即 CN 镜像);
- **需要**:在 `config.cppm` 加一个"索引工件 URL 模板"(基于 resource-server 拼出
  `{server}/xim-index/.../xim-index-<ver>.tar.zst`),并允许 env 覆盖做镜像/测试。

### 7.9.4 顺带又一个"免费"收益

因为索引工件走 resource-server 路径,它**自动继承**了 `config.cppm` 那套
"GLOBAL/CN 区域选择 + 多服务器 fallback + 延迟探测"。也就是说:**索引的"固定地址"问题和
"多镜像/国内镜像/择优"问题,被同一套既有机制一次性解决**——这与 §7.8.2(索引挪到 HTTP 路径顺带
拿到卡顿看门狗/延迟重排)是同一种"复用既有能力"的红利。

> 一句话:要固定地址,但 xlings 已经有了(`xlings-res` + `config.cppm` 锚点);
> 新建的只是"在该地址下给索引工件开一个发布位 + CI 推送 + 一个 URL 模板",不是一套新基础设施。

---

## 7.10 已选定方案与实施

**决策(2026-06-22):采用 Y-asset(§7.7),完全覆盖、不考虑老用户。** 索引作为版本化资源工件
发布到 `xlings-res/xim-index`(GitHub + GitCode),运行时经 resource-server 路径获取。
具体实施步骤见 **`2026-06-22-index-as-resource-impl-plan.md`**。目标版本 **v0.4.52**。
X-full(§7.5,指针+签名+CDN)作为未来演进保留余地(manifest 已预留 `format_version`/`signature`)。

---

## 8. 结论

- **要重设计的是"获取与传输范式"(A 轴)**:live git clone → 预构建静态签名 catalog + CDN + 多域内分发面 +
  已有的延迟探测/续传/卡顿看门狗。这是用户痛点的根治,也是行业一致方向,且在 67 包体量上恰好也是最简方案。
- **配套补"完整性与可复现"(B 轴)**:minisign 签 catalog + 配方 sha256 全覆盖 + 借 mcpp 的 `xlings.lock`。
- **明确不做(C 轴大改)**:OCI-registry-as-index、TUF、IPFS、Sigstore(纯 CI 发布前)——对此体量是过度设计,
  且 OCI/ghcr 对中国用户反而更差。git 留作事实源。
- **落地**:P0/P1 先止血(纯收益),P2 切范式,P3/P4 收口完整性与可复现。
