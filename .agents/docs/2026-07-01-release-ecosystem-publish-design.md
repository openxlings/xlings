# release → xlings 生态自动发布(打通 xim-pkgindex 源 bump + res 双镜像)

**日期**: 2026-07-01
**类型**: 方案 / 架构 (design)
**状态**: Design draft,待 review(决策已拍板见 §0.1)
**范围**: 给 **xlings** 与 **mcpp** 两个项目补齐"发版即自动发布到 xlings 生态"的能力。
一次 release 后幂等完成三件事:① 二进制镜像到 `xlings-res/<proj>`(gh+gtc);
② bump 索引**源** `openxlings/xim-pkgindex` 的 `pkgs/*/<proj>.lua`(PR + auto-merge);
③ 重建并发布索引 **artifact** + 指针到 `xlings-res/xim-index`。
**关联代码/脚本**: `.github/workflows/release.yml`、`tools/mirror_xlings_res.sh`、
`tools/build_xim_index_artifact.sh`、`tools/publish_xim_index.sh`、`tools/push_index_pointers.sh`、
`openxlings/xim-pkgindex/.github/scripts/version-check.py`
**关联文档**:
- `2026-06-24-pkgindex-publish-decoupling-ci.md`(索引 artifact 发布与 xlings 发版解耦)
- `2026-06-25-index-ecosystem-unification-plan.md`(主索引 + 默认子索引统一 artifact 化)
- `2026-06-30-index-artifact-git-regression-analysis.md`(artifact→git 退化,0.4.62 已修)
- `2026-06-22-index-as-resource-impl-plan.md`(index-as-resource)

---

## 0. TL;DR

release 侧的基础设施**大部分已就绪**,唯独缺**最关键的一步:没有任何自动化去 bump 索引"源"**
(`pkgs/x/xlings.lua` / `pkgs/m/mcpp.lua` 的 `latest.ref` + 新版本条目)。结果:

- **xlings**:二进制镜像(`mirror-binaries`)+ 索引 artifact 发布(`publish-index`)**已自动**,
  但 artifact 是 `git clone openxlings/xim-pkgindex main` **现建**的,源没 bump → **artifact 也 stale**,
  发了等于没发(0.4.61/0.4.62 就是这样卡在 0.4.60,见本轮修复 PR openxlings/xim-pkgindex#336)。
- **mcpp**:release **完全不碰**索引/res —— res 镜像 + 索引 bump PR **全靠手工**(如 index#335)。

补齐方案 = 一个可复用的 **`ecosystem-publish`** job(每仓 vendored 一份脚本),核心是新增
**`bump_index.sh`**(内核复用索引仓已有的 `version-check.py --apply`),配合把 `mirror_xlings_res.sh`
泛化成 `mirror_res.sh <proj> <ver>`。

### 0.1 已拍板决策

| 决策点 | 选择 |
|---|---|
| 索引源 bump 如何合入 main | **只开 PR 到 `openxlings/xim-pkgindex`,人工 review 合并**(不做 auto-merge、不加分支保护)。设计视角:可用性由 xlings-res 侧(二进制镜像 + 索引 artifact)保证,索引 git 源只是元数据对账,留人工过一眼即可 |
| 发布逻辑放哪 | **每仓各自 vendored 一份脚本**(`tools/` 或 `.github/tools/`),不做中央 workflow/跨 org dispatch |
| 本次交付 | **仅设计文档**(本文件),实现另起 |

---

## 1. 现状盘点(release → 生态)

| 环节 | xlings | mcpp | 载体 / 脚本 |
|---|---|---|---|
| 建 GitHub Release + 全平台产物 | ✅ `release.yml` `create-release` | ✅ `release.yml` | softprops/action-gh-release |
| 二进制镜像 → `xlings-res/<proj>`(gh+gtc) | ✅ `mirror-binaries` job | ❌ **手动** | `tools/mirror_xlings_res.sh`(仅 xlings 硬编码) |
| 索引 **artifact** → `xlings-res/xim-index`(gh+gtc)+ 指针 | ✅ `publish-index` job | ❌ 无 | `build_xim_index_artifact.sh` / `publish_xim_index.sh` / `push_index_pointers.sh` |
| 更新索引**源** `pkgs/*/<proj>.lua` | ❌ **无自动化** ← stale 根因 | ❌ **手动 PR** | (仅索引仓 `version-check` cron,且只 Phase-1 dry-run) |

### 1.1 两个关键事实(实测)
1. **mcpp `release.yml` 0 处引用 `xim-pkgindex`**:res 镜像 + 索引 bump 全手工(index#335 由 Sunrisepeak 本人开 PR,body 里的 GET-verify+sha256 靠手工)。
2. **xlings 索引 artifact 建自 `openxlings/xim-pkgindex` main**(`build_xim_index_artifact.sh:50-51` 默认 URL,`REF=main`)。源不 bump → artifact stale。

### 1.2 索引仓自动化的现状
`openxlings/xim-pkgindex` 的 `version-check` 工作流(cron `0 1 * * *`)**只跑 Phase-1 dry-run**:
`version-check.py --workspace .` 扫描 → 上传 `version-report.json` artifact,**无 apply/commit/PR**。
今天(07-01)那次 run 已正确检测 `xlings current=0.4.60 upstream=0.4.62 update-available`,但不落地。

**但脚本本身有 `--apply`(Phase-2)**:
- `res_versioned`(xlings):仅追加 `["<ver>"]="XLINGS_RES"` + 顶 `latest.ref`,**不下载**。
- `url_template`(mcpp):`--apply` 会**下载各平台产物、算 sha256、回写** `url+sha256`。
- → **一个工具覆盖两种包**,是本方案 bump 的现成内核。

---

## 2. 核心 gap 与设计目标

**gap**:release 时缺"把索引源 bump 掉"这一步;mcpp 缺整条生态发布。

**目标**:任一项目(先 xlings + mcpp)发版后调用一次 `ecosystem-publish`,**幂等**完成:

```
                        ┌──────────────────────────────────────────────┐
   GitHub Release   ──▶ │            ecosystem-publish job              │
   (create-release)     ├──────────────────────────────────────────────┤
                        │ ① mirror_res.sh <proj> <ver>                  │  gh + gtc → xlings-res/<proj>
                        │ ② bump_index.sh <proj> <ver>  (PR+auto-merge) │  → openxlings/xim-pkgindex
                        │ ③ publish-index (needs ②合并)  artifact+指针  │  → xlings-res/xim-index  gh+gtc
                        └──────────────────────────────────────────────┘
```

**原则**:
- **复用已有内核**:bump = `version-check.py --apply`;res 镜像 = `mirror_xlings_res.sh`;artifact = 现有三脚本。
- **每仓 vendored、参数化**:脚本以 `<proj>` 为参数,xlings/mcpp 各存一份,消除硬编码。
- **幂等 + 非阻塞**:各步失败互不回滚 release,可重跑。
- **有门禁**:索引源改动走 PR + index CI,green 后 auto-merge。

---

## 3. 脚本接口详规(vendored per-repo)

### 3.1 `mirror_res.sh <project> <version>` — 泛化 `mirror_xlings_res.sh`
现脚本已参数化 `SRC_REPO`/`GH_DST`/`GTC_DST`(默认 `openxlings/xlings`→`xlings-res/xlings`),
只需把默认值改为按 `$1` 推导:
```
SRC_REPO="${SRC_REPO:-<org>/<project>}"        # xlings: openxlings/xlings; mcpp: mcpp-community/mcpp
GH_DST="${GH_DST:-xlings-res/<project>}"
GTC_DST="${GTC_DST:-xlings-res/<project>}"
ASSETS=...                                       # 按项目 tag 的产物命名(见各仓 release 命名)
```
行为不变:`gh release download` 源产物 → `gh release upload` 到 GH res 仓 + `gtc release upload` 到 GitCode res 仓,
逐文件重试 + GET-verify 200 + sha256 对账。**mcpp 直接复用,消灭手工镜像。**

### 3.2 `bump_index.sh <project> <version>` — 新增(内核已存在)
```
1. git clone --depth 50 https://github.com/openxlings/xim-pkgindex   # 需可写 token
2. cd xim-pkgindex && git checkout -b bump/<project>-<version>
3. GITHUB_TOKEN=$PAT python3 .github/scripts/version-check.py --apply --only <project>
   - xlings(res_versioned): 追加 ["<ver>"]="XLINGS_RES" + latest.ref=<ver>,无下载
   - mcpp(url_template):    下载各平台产物算 sha256,回写 url+sha256 + latest.ref
4. git commit -am "bump(<project>): track <version> as latest"
5. git push origin bump/<project>-<version>
6. gh pr create --repo openxlings/xim-pkgindex --base main ...   # 到此为止,人工 review 合并
```
**幂等**:`--apply` 对已存在版本 no-op;分支已存在则 `--force-with-lease` 或复用。
**不自动合并**:脚本只负责把 PR 开出来(需 `XIM_PKGINDEX_TOKEN` 的 Contents+Pull requests 写权),
合并由维护者人工完成 —— 索引 git 源不进 release 关键路径。

### 3.3 索引 artifact:复用现有三脚本,**不改时序**
`build_xim_index_artifact.sh` / `publish_xim_index.sh` / `push_index_pointers.sh` **完全不动**。
因为 bump 只开 PR、由人工合并,release 时 main 尚未含新版本,故:
- release 的 `publish-index` 照常从 main 建 artifact(该次 artifact 对**被 bump 的包**尚是旧值,不影响其它包)。
- **新版本进 artifact 的时刻 = 维护者合并 bump PR 时**:合并 push 到 main → 触发索引仓自己的
  解耦发布(`publish-artifact.yml`,见 `2026-06-24` 文档 §2)重建并发布 artifact + 指针。
- git 回退路径同理:合并 → gitee-sync → 客户端 `xlings update` git pull 见新版本。

→ **release 与索引源 merge 解耦**:release 只保证 xlings-res 侧(二进制 + artifact 基础设施)就绪,
索引 git 源的落地(及其 artifact 刷新)由"人工合 PR → 索引仓自发布"这条独立链完成。

---

## 4. 各仓落地

### 4.1 xlings `release.yml`(改动小)
新增 `bump-index` job,与 `publish-index` / `mirror-binaries` **并列**(互不依赖):
```
create-release
   ├─▶ bump-index      (新: bump_index.sh xlings $VER —— 只开 PR,人工合)
   ├─▶ publish-index   (已有,不变;从 main 建 artifact)
   └─▶ mirror-binaries (已有,不变)
```
三者独立、非阻塞。`bump-index` 只把 PR 开出来;被 bump 的包进 artifact 由"人工合 PR →
索引仓自发布"完成(§3.3),不与 release 串行。

### 4.2 mcpp `release.yml`(补齐整条)
release 各平台 build 完、`create GitHub Release` 后,新增 `publish-ecosystem` job:
```
- name: mirror binaries to xlings-res/mcpp (gh+gtc)
  run: bash .github/tools/mirror_res.sh mcpp "$VER"
- name: bump index source (open PR, 人工合)
  run: bash .github/tools/bump_index.sh mcpp "$VER"
```
mcpp release 已 bootstrap xlings,可直接 raw 拉取或 vendored `tools/gtc` + 脚本。
mcpp 是 `url_template` 包,`bump_index.sh` 走下载算 sha256 路径,**替代手工 index#335 流程**。
> 注:mcpp 的索引 artifact 目前由 xlings release 的 `publish-index` 覆盖(主索引整体打包);
> mcpp 侧本轮只需 ①源 bump + ②res 镜像。artifact 刷新沿用索引仓解耦发布
> (见 `2026-06-24` 文档 §2 的 `publish-artifact.yml`)。

---

## 5. bump 落地:只开 PR,人工合并
- **开 PR**:`gh pr create`,标题 `bump(<proj>): track <ver> as latest`,body 附检测证据(current/upstream/tag)。
- **人工合并**:维护者 review 后手动合(与现有 mcpp index#335 的习惯一致)。
- **不需要**:`Allow auto-merge`、分支保护/required checks —— 人工 review 即门禁。
- **为何这样**:索引 git 源不在 release 关键路径,可用性已由 xlings-res 侧保证;
  索引改动保留一个人工确认点,最简单且留痕。
- **无值守兜底**:即使某次 release 漏开/漏合 PR,索引仓 `version-check` cron 仍会检测到并可补
  (见 §8 P2:把 cron 从 dry-run 升级为自动开 PR)。

---

## 6. Token / 权限 / 时序

### 6.1 Token(架构层:本流程用到哪些凭据)
| 用途 | 凭据 | 目标 |
|---|---|---|
| 写 res 仓(二进制镜像 + 索引 artifact) | `XLINGS_RES_TOKEN` | `xlings-res/*` |
| 开 bump PR | `XLINGS_RES_TOKEN`(需含该仓写权) | `openxlings/xim-pkgindex` |
| GitCode 上传 | `GITCODE_TOKEN` + vendored `tools/gtc` | `xlings-res/*`(GitCode) |

> gitee-sync(→ `sunrisepeak/xim-pkgindex`,本地 clone 跟踪源)在 bump 合并后由索引仓自身
> 已配的 `GITEE_TOKEN`/`GITEE_SSH_KEY` 自动触发。
> (具体 secret 落位与仓库设置是运维动作,不在本设计文档范围。)

### 6.2 端到端时序(xlings 为例)
```
【release 自动段】
tag v0.4.63 ─▶ build(4 平台) ─▶ create-release(GH)
                                   ├─▶ mirror-binaries ─▶ xlings-res/xlings (gh+gtc)   [产物立即可下]
                                   ├─▶ publish-index   ─▶ xlings-res/xim-index + 指针   [基础设施就绪]
                                   └─▶ bump-index      ─▶ 开 PR 到 pkgindex            [等人工]

【人工段 —— 维护者点一下 merge】
merge bump PR ─▶ pkgindex main 含 0.4.63
                    ├─▶ 索引仓 publish-artifact.yml ─▶ 重建 artifact(含新版)+ 指针 ─▶ xlings-res/xim-index
                    └─▶ gitee-sync ─▶ sunrisepeak/xim-pkgindex

用户 `xlings update` ─▶ 拉新指针/artifact(或 git 回退)─▶ 见 0.4.63
```
关键点:二进制在 release 段就到位;**"latest 指到 0.4.63" 在人工合 PR 后生效**(通常几分钟内)。

---

## 7. 幂等 / 失败 / 回滚
- **幂等**:version-check apply 版本已存在→no-op;res 镜像 sha256 匹配→跳过;job 可整体重跑。
- **失败隔离**:`mirror-binaries` / `bump-index` / `publish-index` 相互独立,单点失败不撤 release。
- **回滚**:索引 bump 是纯文本 PR,revert 即回退;res 产物可删 release asset;artifact 指针可回滚到上一个。
- **观测**:每步 `gh` 输出留在 Actions log;bump PR 号回填到 release job summary。

---

## 8. 分阶段落地计划(实现另起)
- **P0(先跑通 xlings 闭环)**:泛化 `mirror_res.sh`;新增 `bump_index.sh`(开 PR,不合并);xlings `release.yml` 加并列 `bump-index` job(用 `XIM_PKGINDEX_TOKEN`)。
- **P1(mcpp 接入)**:mcpp 仓 vendored `mirror_res.sh`/`bump_index.sh`/`gtc`;secrets 已配(`XLINGS_RES_TOKEN`/`XIM_PKGINDEX_TOKEN`/`GITCODE_TOKEN`);`release.yml` 加 `publish-ecosystem`;验证 url_template 的 sha256 回写路径。
- **P2(收尾)**:索引仓 `version-check` cron 从 Phase-1 升级为"检测到→自动开 PR"作为**兜底**(即使某次 release 漏发,cron 也能补);两仓脚本加漂移检测(diff 提醒保持同步)。

---

## 9. 未决 / 后续
- [x] token 已就位:`XIM_PKGINDEX_TOKEN`(openxlings/xim-pkgindex,Contents+PR 写)已建并配到 xlings + mcpp 仓;`XLINGS_RES_TOKEN` 已 regenerate 并同步三仓。
- [ ] mcpp 索引 artifact 是否需要独立于 xlings 主索引发布(当前主索引整体打包已覆盖 mcpp 条目)。
- [ ] 两仓 vendored 脚本的同步策略:定期 diff 告警 vs 用 git subtree/submodule。
- [ ] `version-check.py --apply` 对 mcpp 多平台 sha256 的 CI 网络稳定性(下载失败重试)。
