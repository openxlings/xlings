# 索引生态全面打通 — 主索引 + 默认子索引统一 artifact 化方案

**日期**: 2026-06-25
**类型**: 方案 / 架构 (plan + architecture)
**范围**: 把**主索引 `xim-pkgindex` + 其默认子索引(`awesome`/`scode`/`d2x`)** 统一到同一套
index-as-resource(artifact)机制,git 仅作回退;打通发布侧(跨仓 CI)与客户端消费,
并修掉拖累体验的 CN 代理问题。**后加的非默认索引仓(如 `fromsource`)、用户自定义仓不在本轮目标内**
——它们自动落到 git 回退路径,保持兼容即可。
**决策(已拍板)**:
- 子索引传输形态 = **统一到 artifact,git 仅回退**(与主索引同构,一套代码)。
- 完整性 = **本轮只做传输完整性**(artifact sha256 已有 + 指针双发对账);
  端到端签名(minisign)/内容寻址留作后续阶段。
**关联**:
- `2026-06-21-pkgindex-mirror-analysis.md`(差距 G1–G6 的来源)
- `2026-06-21-pkgindex-redesign-proposal.md`、`2026-06-22-index-as-resource-impl-plan.md`(主索引 artifact 化)
- `2026-06-24-pkgindex-publish-decoupling-ci.md`(发布解耦,本方案承接其 P0 未尽部分)

---

## 0. TL;DR

主索引已经现代化(指针 + 内容哈希 artifact + gitcode/github 双源 + 延迟重排 + 卡顿看门狗 +
sha256)。**默认子索引的客户端消费、发版打包、多 key 组合指针其实都已经写好了** —— 缺的不是
"从零实现",而是 **4 个最后一公里缺口**:

1. **迁移闸**:已有 git 副本的子索引在 `auto` 模式下永不自动切 artifact(现网用户卡在 git)。
2. **子仓发布未解耦**:改默认子索引,artifact 只在"下次 xlings 发版"才刷新。
3. **CN 代理 403**:代理无条件套到 `gitcode.com`/`gitee.com` 等国内域名,白费尝试 + 降权。
4. **URL/org 不一致 + json 卫生**:`subDefaultOfficial` 判定要求 lua/json/release.yml 三处 URL 一致,
   现状不一致会让子索引**静默退回 git**。

闭上这 4 个缺口,即达成"主索引 + 默认子索引全面 artifact 化、git 仅回退、改即发布、弱网/被墙不卡"。

---

## 0.5 实施进度与范围调整(2026-06-25,实现期更新)

**范围调整(用户拍板)**:
- **C3(CN 代理直连白名单)→ 取消**。实测用户本地代理对 CN 域名已直连,403 非代理策略问题,不改 `tinyhttps`。
- **C2 → 简化**。不在各子仓加 `publish-artifact.yml`;只需 `xim-pkgindex` 仓一个**手动可触发
  (`workflow_dispatch`)** 的发布(发版 `release.yml` 已兜底"至少发版时刷新")。
- **幽灵 `fromsource` → 不 prune、不特殊处理**。它非 lua 默认,自然走 git 回退,互不影响。

**已完成并端到端验证(P0 客户端,本仓)**:
- **C1 迁移闸** + **C4 lua 权威合并(修 org 漂移)** 已实现,抽为纯函数
  `merge_sub_repos` / `sub_should_attempt_artifact`(`src/core/xim/repo.cppm`),并接入 `sync_all_repos`。
- 单测 8 个全绿(`tests/unit/test_main.cpp` 的 `XimSubReposTest`)。
- **实机验证**:用新二进制 `xlings update` —— `awesome`/`scode`/`d2x` 从 git **迁移到 artifact**
  (各写出 `.xlings-index-version`、删除 `.git`),`fromsource` 正确保持 git;
  `xim-indexrepos.json` 的 org 漂移(awesome/scode 旧 `d2learn`)被 lua 权威**自愈**为 `openxlings`。

**已完成(发布闭环,跨仓)**:
- **C2 手动发布工作流**已合入 `xim-pkgindex`(`publish-sub-indexes.yml`,#311),实测发布成功。
- **发版 0.4.60**:版本 bump(#343)+ `release.yml` 发布 4 平台二进制 + 主/子索引 artifact 到
  `xlings-res`(GitHub + GitCode)。
- **`xlings` recipe bump 到 0.4.60**(#313),并**根治自动升级**:给 `version-check.py` 加
  `res_versioned` 支持(#314),让 XLINGS_RES 风格的包(含 xlings 自身)也能被 version-bump 机器人自动升级
  (此前 `latest` 卡在 0.4.55 达 5 个版本)。
- **全链路实测**:真实安装的 0.4.55 → `self update` → **0.4.60** → `xlings update` 子索引迁移 artifact。

**待办**:C5 指针双发对账(P2);端到端签名 minisign(后续阶段)。

---

## 1. 现状(精确核实)— 已建成 vs 缺口

### 1.1 已经建成的(不要重复造)

| 能力 | 状态 | 证据 |
|---|---|---|
| 主索引 artifact 拉取(指针→tarball→sha256→原子替换) | ✅ | `indexfetch.cppm:317-392` |
| **默认子索引 artifact 拉取(客户端)** | ✅ **已实现** | `repo.cppm:489-516`,`fetch_index_artifact(repoDir, ferr, repo.name)` |
| 多 key 组合指针 `xim-index-pointers.json`(`indexes.{xim,awesome,scode,d2x}`) | ✅ | `indexfetch.cppm:308-311`;`push_index_pointers.sh:52` 合并不覆盖 |
| **发版时打包+发布默认子索引 artifact** | ✅ | `release.yml:383-411`(`--name awesome/scode/d2x`,GH+GitCode 双发) |
| 主索引 push 即发布(解耦 CI) | ✅ | `xim-pkgindex` 仓 `publish-artifact.yml`(decoupling-ci.md §4.5) |
| 延迟重排 + 卡顿看门狗 + host 惩罚(artifact 路径) | ✅ | `adaptive.cppm`、`tinyhttps.cppm`(10KiB/s×15s) |
| 子索引 artifact 失败 → git 回退 | ✅ | `repo.cppm:511-513` |

> 即:**一个全新安装(主&子索引目录都不存在)今天就会全程走 artifact**。问题只出在"已装过的存量用户"和"发布新鲜度"。

### 1.2 实测当前机器状态(为什么你的 log 子索引走了 git)

```
主索引 xim-pkgindex:        .xlings-index-version=303aa7f, 无 .git  → artifact-managed ✅
子索引 awesome/scode/d2x/fromsource: 无 marker, 有 pkgs/         → git-managed ❌
```

`auto` 判定(`repo.cppm:499-500`):
```cpp
subAttemptArtifact = subDefaultOfficial && (subManaged || !exists(repoDir/"pkgs"));
```
子索引**有 pkgs/、无 marker** → `(false || false)` → **不切 artifact**,继续 git。这与主索引同款的
"存量 git 副本不自动迁移"保护一致 —— 主索引早先被重装过所以已迁,子索引没有,于是卡在半迁移态。

---

## 2. 目标架构

**一句话**:主索引与默认子索引**完全同构**——同一份多 key 指针、同一个 `fetch_index_artifact`、
同一套韧性机制;git 退化为纯回退(被墙/artifact 不可用/非默认仓/本地源时)。

```
                    xlings-res/xim-index   (GitHub + GitCode 双端)
                    ├─ xim-index-pointers.json   ← 组合指针(多 key, 仓库文件, 可覆盖)
                    │     { format_version:1, indexes:{
                    │         xim:{artifact,sha256,...},
                    │         awesome:{...}, scode:{...}, d2x:{...} } }
                    └─ releases/<tag>/xim-index[-<name>]-<hash>.tar.gz  ← 内容哈希命名, immutable

客户端 xlings update (repo.cppm:sync_all_repos)
  1. 取组合指针(CN: raw.gitcode → raw.github 回退)            [无 sha,双发对账]
  2. 主索引:    fetch_index_artifact(mainDir)                  [sha256 pin]
  3. 默认子索引: fetch_index_artifact(subDir, name)  × {awesome,scode,d2x}
        └ 失败 / 非默认仓(fromsource/自定义/本地)→ git 回退 (sync_repo)
  4. catalog.rebuild → 合并主+子为单一 catalog (index.cppm:244)

发布侧(改即发布,内容哈希解绑 xlings 版本)
  xim-pkgindex         push pkgs/** → publish-artifact.yml → 发 xim   artifact + 合并 xim   key
  xim-pkgindex-awesome push pkgs/** → publish-artifact.yml → 发 awesome ...  + 合并 awesome key   ← 新增
  xim-pkgindex-scode   push pkgs/** → publish-artifact.yml → ...                                 ← 新增
  xim-pkgindex-d2x     push pkgs/** → publish-artifact.yml → ...                                 ← 新增
  xlings release       仍发"已知好"的整套配对(保留, 非唯一通道)
```

**为什么"统一 artifact"顺带解决了无 CN 镜像(G1)**:子索引 artifact 本就发到 `xlings-res/xim-index`
的 **GitCode 端**,CN 用户取子索引 = 取 gitcode artifact,不再被迫 git-clone github。git 路径上
"子索引无国内镜像"的痛点在默认路径上自然消失(git 仅在 artifact 全挂时才回退)。

---

## 3. 多仓库角色与协作(本节为多仓协作说明,落地需跨仓改动)

| 仓库 | 角色 | 本方案需要的改动 | 谁来改 |
|---|---|---|---|
| **`openxlings/xlings`**(本仓) | 客户端 C++ + 发布脚本/CI 源 | 迁移闸(C1)、CN 代理白名单(C3)、URL 规范化(C4)、指针对账(C5)、`release.yml` 子索引命名对齐 | 本仓 PR |
| **`openxlings/xim-pkgindex`** | 主索引内容 + 默认子索引声明 `xim-indexrepos.lua` | 规范化默认子仓 URL 到唯一 org(C4) | 跨仓(已有 vendored `tools/`) |
| **`openxlings/xim-pkgindex-awesome`** | 默认子索引内容 | **新增 `publish-artifact.yml`**(C2)+ vendored `tools/` | 跨仓 |
| **`openxlings/xim-pkgindex-scode`** | 默认子索引内容 | **新增 `publish-artifact.yml`**(C2) | 跨仓 |
| **`d2learn/xim-pkgindex-d2x`** | 默认子索引内容(注意属 `d2learn` org) | **新增 `publish-artifact.yml`**(C2) | 跨仓(跨 org,需协调 d2learn) |
| **`xlings-res/xim-index`** | 资源仓:artifact 资产 + 组合指针 | 无需代码,作为发布目标;指针文件 = 各仓"合并自己 key" | secrets 已配 |

**跨仓一致性约束(关键,不满足则子索引静默退回 git)**:同一个默认子索引,其
**① `xim-indexrepos.lua` 的 URL、② 客户端持久化的 `xim-indexrepos.json` URL、③ `release.yml` /
各子仓 CI 发布时用的 repo URL,以及 ④ 组合指针里的 key 名** 必须全部对齐:

- key 名(`awesome`/`scode`/`d2x`)是客户端 `repo.name` → 指针 key 的唯一纽带(URL 仅用于 git 回退)。
- `subDefaultOfficial` 判定(`repo.cppm:492-494`)要求 `luaUrl[name] == repo.url`;若 lua 与 json
  org 不一致(实测:home 副本 lua 用 `openxlings/awesome`,json 用 `d2learn/awesome`),判定为 false,
  该子索引**永不走 artifact**。必须收敛到唯一 canonical org(见 C4)。

> **GitCode 资产命名注意**:GitCode release 资产不可覆盖,故 artifact 用**内容哈希 immutable 命名**
> (`xim-index-<hash>.tar.gz`),指针(`xim-index-pointers.json`)走**仓库文件 push + GH release asset
> 双发**(可覆盖/可 raw 读)。已在 decoupling-ci.md §4.5 验证;本方案沿用。

---

## 4. 缺口与改造(逐项)

### C1 — 迁移闸:让存量 git 子索引自动切 artifact 【客户端,小】
**问题**:`auto` 下有 pkgs/无 marker 的默认子索引永不迁移(§1.2)。
**改动**(`repo.cppm:499-500`):当**主索引已是 artifact-managed**时,把默认子索引视作"同一索引单元",
即使已有 pkgs 也尝试 artifact:
```cpp
else subAttemptArtifact = subDefaultOfficial
         && (subManaged || !exists(repoDir/"pkgs") || mainArtifactManaged);
```
失败仍自动回退 git(`repo.cppm:511-513`),零风险。效果:存量用户**下次 `xlings update` 即完成迁移**
(主已 artifact → 子跟进),无需 `--force` 或重装。
**验证**:本机主索引已 artifact(`303aa7f`),改后一次 update 应使 awesome/scode/d2x 写出
`.xlings-index-version` 且不再打印 `updating index repo:`。

### C2 — 默认子索引发布(简化版:`xim-pkgindex` 一个手动工作流)【CI】
**问题**:子索引内容变更后,artifact 只在下次 xlings 发版刷新。
**改动(简化,用户拍板)**:**不**在各子仓加 CI;在 `xim-pkgindex` 仓加一个
**`workflow_dispatch` 手动可触发**的发布工作流即可,需要时主动重打包默认子索引并移指针:
- 触发:`workflow_dispatch`(可带 `name` 参数选 awesome/scode/d2x 或 all)。
- 脚本:复用本仓 `tools/build_xim_index_artifact.sh` + `publish_xim_index.sh`(`--name <sub>`),
  发到 `xlings-res/xim-index`(GH+GitCode),`push_index_pointers.sh` 只 update 自己 key(合并不覆盖)。
- **发版兜底**:`xlings release` 的 `release.yml:383-411` 仍在发版时整套发布默认子索引,保证"至少发版时刷新"。
**(原"各子仓各加 publish-artifact.yml"方案取消)**

### C3 — ~~CN 代理直连白名单~~ 【取消】
**决策:不做**。实测用户本地代理对 CN 域名(gitcode/gitee)已直连,日志里的 403 非 xlings 代理策略
问题,而是该次代理出口的偶发。不在 `tinyhttps` 内置 CN 直连名单,保持默认 `NO_PROXY` 语义即可。

### C4 — URL/org 规范化(lua 权威自愈)【已实现,本仓】
**问题**:lua / json / release.yml 三处 org 不齐(实测 json 把 awesome/scode 钉在旧 `d2learn`,
lua 已是 `openxlings`)→ `subDefaultOfficial` 为 false → 子索引静默退回 git。
**改动(已实现)**:`merge_sub_repos` 令 **lua 默认对其自身 name 权威**,json 只补 lua 没有的
用户自定义仓 → org 漂移的默认仍判定为 official 并迁移 artifact,陈旧 json 在下次 `save_sub_repos_json`
**自愈**为 lua 的 canonical org。**不 prune 幽灵 `fromsource`**(它非 lua 默认,留作 json-only 走 git)。
> 跨仓收尾(可选 P1):把 release.yml / lua 的默认子仓 org 固定下来不再漂移,减少自愈发生。
**问题**:lua(home: `openxlings/awesome` vs dev: `d2learn/awesome`)、json(`d2learn/*` + 幽灵 `fromsource`)、
`release.yml`(`openxlings/awesome`+`openxlings/scode`+`d2learn/d2x`)三处 org 不齐 → `subDefaultOfficial`
可能为 false → 子索引静默退回 git。
**改动**:
1. 选定每个默认子仓的 **canonical org**(建议 awesome/scode = `openxlings`,d2x = `d2learn`,与 release.yml 对齐),
   统一写入 `xim-pkgindex/xim-indexrepos.lua` 的 GLOBAL 与 CN(CN 此后由 artifact 承载,git 回退 URL 与 GLOBAL 同)。
2. 客户端:`save_sub_repos_json` 在写回时**以 lua 默认为准 prune 掉已不在 lua 的默认条目**
   (清掉 `fromsource` 这类幽灵;用户自定义条目保留)。
3. 一次性:文档说明存量用户 `xim-indexrepos.json` 里 org 漂移会被下次 update 以 lua 为准纠正。

### C5 — 指针传输完整性(本轮档位) 【客户端 + 发布,小】
**问题**:组合指针自身 `obtain_file(..., {})` 无 sha(`indexfetch.cppm:301`);artifact 有 sha256 但指针没有。
**改动(本轮只做传输完整性)**:
- 发布侧:指针**双发对账**——GH release asset 与 raw 仓库文件两路发布,客户端取到后比对两路一致
  (已天然双源,补一次交叉校验日志即可);或在指针里加 `pointers_sha256` 自描述字段供二次校验。
- 不引入私钥/签名。**端到端签名(minisign)、内容寻址列入后续阶段(§7)。**

---

## 5. 分阶段落地

| 阶段 | 内容 | 仓库 | 收益 |
|---|---|---|---|
| **P0(客户端先行,纯本仓,立即见效)** | C1 迁移闸 + C3 CN 代理白名单 + C4 客户端 json prune | xlings | 存量用户子索引一次 update 即迁 artifact;消除 403;体验立刻变好 |
| **P1(发布解耦,跨仓)** | C2 三个默认子仓加 `publish-artifact.yml` + C4 规范化 lua/release.yml org | xim-pkgindex-{awesome,scode,d2x} + xim-pkgindex | "改子索引即发布",不再等发版 |
| **P2(完整性 + 收尾)** | C5 指针双发对账;统一 mcpp-index 路径(若需) | xlings + xlings-res | 传输完整性加固 |
| **后续(本轮不做)** | minisign 端到端签名 / 内容寻址 / 稀疏索引 | 全生态 | 镜像/代理不可信下的投毒防护 |

**推荐顺序**:**P0 → P1 → P2**。P0 不依赖任何跨仓改动,可独立先发一版,马上改善体验;
P1 跨仓(含 d2learn 协调)单独推进;P2 收尾。

---

## 6. 兼容性 / 回滚 / 风险

- **回退保留**:`XLINGS_INDEX_SOURCE=git` 一键回到全 git;artifact 任一步失败自动 git 回退,**不破坏现有 e2e/本地 fixture**(本地/file:// 源永不走 artifact,`repo.cppm:441`、`494`)。
- **非默认/自定义仓不受影响**:不在 lua 默认集、URL 被 override、或本地源的子索引继续 git(含 `fromsource`)。
- **C1 风险**:几乎为零(失败回退 git)。需 e2e 覆盖"主 artifact + 子存量 git → 迁移"场景。
- **C3 风险**:特殊网络下国内站也需代理 → `XLINGS_PROXY_CN_DIRECT=off` 兜底。
- **C2 风险(跨仓)**:d2x 跨 org,需 d2learn 配合 secrets/Actions;未配齐前 d2x 仍靠 xlings 发版刷新(降级可接受)。
- **C4 风险**:org 选错会让 `subDefaultOfficial` 全 false → 全退 git(功能不丢,只是没迁)。改动后必须验证三处一致。

---

## 7. 文件级改动清单

**`openxlings/xlings`(本仓,P0/P2)**
- `src/core/xim/repo.cppm`
  - `:499-500` 迁移闸:auto 条件加 `|| mainArtifactManaged`(C1)
  - `save_sub_repos_json` 调用处 `:518`:写回时按 lua 默认 prune 幽灵默认条目(C4)
- `src/libs/tinyhttps.cppm`
  - `env_proxy_for_ :175-197`:内置 CN 直连名单 + `XLINGS_PROXY_CN_DIRECT` 开关(C3)
- `src/core/xim/indexfetch.cppm`
  - 指针获取处 `:301`:双发对账 / `pointers_sha256` 二次校验(C5)
- `.github/workflows/release.yml`
  - `:383-411`:子索引命名改内容哈希、org 与 lua 对齐(C4/C2 对齐)
- 测试:e2e 增"存量 git 子索引迁 artifact"、"CN 直连无代理"两用例

**`openxlings/xim-pkgindex`(P1)**
- `xim-indexrepos.lua`:默认子仓 URL 收敛到 canonical org(C4)

**`xim-pkgindex-awesome` / `xim-pkgindex-scode` / `d2learn/xim-pkgindex-d2x`(P1,跨仓)**
- 新增 `.github/workflows/publish-artifact.yml` + vendored `tools/`(C2)
- 仓库 secrets:`XLINGS_RES_TOKEN`、`GITCODE_TOKEN`

---

## 8. 后续阶段(本轮明确不做,留档)

- **端到端签名**:对组合指针(或校验清单)做 **minisign**(Ed25519),客户端内置公钥验签 →
  任意镜像/CDN/代理可不被信任(对应 mirror-analysis G5;配方是可执行 Lua,安全敞口大)。
  代价:跨仓公钥分发 + 各发布 CI 私钥 secret 管理。
- **内容寻址**:用哈希命名指针,只对一个小"根指针"签名,缓存天然安全。
- **稀疏索引**:包数量持续增长后,参考 Cargo sparse / Homebrew 逐包 JSON,按需取元数据。
- **ETag/304 条件请求**:替代当前 7 天节流,新鲜度更精准(G6)。

---

## 9. 验收标准

1. 全新安装:主 + awesome/scode/d2x **全部 artifact-managed**(各有 `.xlings-index-version`,无 `.git`)。
2. 存量用户(主 artifact、子 git):一次 `xlings update` 后子索引完成迁移,日志不再有 `updating index repo:`。
3. CN + 本地代理:`xlings update --verbose` **无 gitcode 403、无 penalize**;gitcode 直连命中。
4. 改任一默认子索引 push:对应 artifact 在 `xlings-res/xim-index`(GH+GitCode)自动刷新,组合指针仅该 key 变更,兄弟 key 不丢。
5. `XLINGS_INDEX_SOURCE=git` 全程回退 git 正常;`fromsource`/自定义/本地子索引继续 git。
