# xim-pkgindex 发布与 xlings 发版解耦(独立发布 CI)

**日期**: 2026-06-24
**范围**: xlings 侧 —— 把 `xim-pkgindex` 的 artifact 发布从 xlings release 解耦,
让"改索引 → 自动发布",并配套客户端的离线优先刷新。
**配套**: mcpp 侧的离线优先 + mcpp-index 发布见 mcpp 仓
`.agents/docs/2026-06-24-offline-first-index-and-mcpp-index-publish.md`。
**前置**: `.agents/docs/2026-06-22-index-as-resource-impl-plan.md`(index-as-resource)、
`.agents/docs/2026-06-21-pkgindex-redesign-proposal.md`。

---

## 已就绪的基础设施(2026-06-24)
- **资源仓**:`xlings-res/xim-index`(github + gitcode 两端均有)—— 放 xim 索引 artifact + 指针。
- **secrets**:`openxlings/xim-pkgindex` 仓已配 `XLINGS_RES_TOKEN` + `GITCODE_TOKEN`(+ `GITEE_TOKEN`)。
- **Actions**:xim-pkgindex 仓已启用。
- **本仓自文档**:发布机制的逐仓说明见 `openxlings/xim-pkgindex` 仓
  `.agents/docs/2026-06-24-artifact-publish-mechanism.md`。
- 剩待办:在 xim-pkgindex 仓加 `publish-artifact.yml`(本文档 §2)。

## 0. 现状机制(双路径)

| 路径 | 载体 | 谁发布 | 客户端 |
|---|---|---|---|
| **artifact(默认 `auto` 优先)** | `xim-index[-<name>]-<ver>.tar.gz` + manifest + 指针 `xim-index-latest.json`,在 **xlings-res/xim-index**(GH+GitCode) | **仅 xlings release 的 `publish-index` job**(`build_xim_index_artifact.sh` + `push_index_pointers.sh`) | 拉指针→比 sha→下 artifact |
| **git(回退)** | github `openxlings/xim-pkgindex` + gitee `sunrisepeak/xim-pkgindex`(`gitee-sync.yml`) | push 即更新 | `xlings update` git pull |

- `XLINGS_INDEX_SOURCE=git\|artifact\|auto`,默认 **auto = artifact 优先、git 回退**。
- 索引仓自己的 `pkgindex-deloy.yml` **只建 GitHub Pages 文档站,不发 artifact**。
- artifact 发布脚本住在 **xlings 仓 `tools/`**,artifact 名按 **xlings 版本** 命名。

---

## 1. 核心 gap

**在 xim-pkgindex 改一个包后**:git 路(gitee 镜像)立即更新,但 **artifact 路(默认)不更新**
—— artifact 只在**下次 xlings release** 才重打包。于是 `auto` 客户端拉到上次 release 的旧
artifact,新版本(如新加的 `mcpp@x.y.z`)在 artifact 里**不存在** → `xlings install` 失败,
直到下次 xlings 发版。

**结论**:目前"改索引让默认客户端拿到"只能靠**发 xlings 新版**或手动跑
`build_xim_index_artifact.sh`。索引仓**没有 push/手动/定时触发的 artifact 发布 CI**。

> 注:发布脚本本来就是 **standalone** 的(`build_xim_index_artifact.sh` 只打包 `pkgs/` +
> `xim-indexrepos.lua` + manifest,不需要 xlings 构建),所以完全可以在索引仓 CI 里独立跑。

---

## 2. 方案:索引仓加独立 artifact 发布 CI

**在 `xim-pkgindex` 仓加 `publish-artifact.yml`**:

- **触发**:`push`(paths: `pkgs/**`, `xim-indexrepos.lua`, `.xpkgindex.json`)+
  `workflow_dispatch`(手动补发)+ 可选 `schedule`(nightly 兜底重对齐镜像)。
- **脚本来源**(三选一):① 把 `build_xim_index_artifact.sh`/`push_index_pointers.sh`
  **复制/移到索引仓**;② CI 里 `curl` 拉 xlings 仓的脚本;③ 抽成 pip 包(像 `xpkgindex`)。
  推荐 ① 或 ③ —— 索引仓自洽,不依赖 checkout xlings。
- **artifact 命名改为内容哈希/时间戳**(`xim-index-<gitsha>.tar.gz`),**解绑 xlings 版本**
  —— 这是解耦的关键(否则还得和 xlings 版本对齐)。
- **发布目标**:`xlings-res/xim-index`(GH + GitCode)。artifact 走新名(immutable);
  **指针 `xim-index-latest.json` 是仓库文件**(GitCode release 资产不可覆盖,故指针走文件 push
  + GH release asset 双发)。
- **Secrets**:索引仓加 `XLINGS_RES_TOKEN` + `GITCODE_TOKEN`(同 xlings release 用的)。
- 子索引(awesome/scode/d2x)同理,各自仓加同款 CI,或在主索引 CI 里一并打包(沿用
  release.yml 现有的 `--name awesome/scode/d2x` 调法)。

**效果**:改 xim-pkgindex push → CI 自动重发 artifact + 移指针 → 默认(artifact)客户端
下次刷新即拿到,**不必发 xlings**。

**保留** xlings release 的 `publish-index`(发版时仍发一份"已知好"的 xlings+索引配对),
但它不再是唯一通道。

---

## 3. 客户端配套:离线优先 + 低成本刷新

避免再现"改了不刷新 / build 卡在 update"(Termux 实战,见 mcpp 仓
termux-android-adaptation §3):

1. **`xlings update` 优先走指针 sha 比对**:GET 轻量 `xim-index-latest.json`(几百字节)比本地
   sha,**命中→零下载/零 git**;未命中才下 artifact(带 sha 校验)。**绝不在主路径 git
   clone/pull**(慢、被墙易卡)。
2. **拉取失败静默降级用本地**(离线可用),不报错中止。
3. **子索引(github-only)不可达即跳过**(已在 0.4.58 `xim/repo.cppm` 用
   `probe_latency` 实现 —— 续用此模型,弱网/被墙不冻结)。
4. **软 TTL + 显式刷新**:把自动刷新 TTL 调到合理(几分钟~小时级),并保证
   `xlings update --force` / `--index <name>` 能显式强刷,绕过 TTL。
5. **git 回退保留**:artifact 不可用时回退 git(`XLINGS_INDEX_SOURCE` 已支持),但默认不主动走。

---

## 4. 落地顺序

1. **P0**:`xim-pkgindex` 加 `publish-artifact.yml`(push/dispatch/schedule),artifact 改内容
   哈希命名 → **改索引即发布**。子索引仓同款。
2. **P1**:客户端 `xlings update` 走"指针 sha 比对优先"(降低 build 联网成本,离线降级)。
3. **P2**:与 mcpp-index 统一(mcpp-index 也补 artifact + 发布 CI,见 mcpp 仓文档)。

---

## 4.5 进度 + 关键修复:指针**合并**而非覆盖(2026-06-24)

- **✅ 落地**:`xim-pkgindex`、`mcpp-community/mcpp-index` 两仓均已加 `publish-artifact.yml`
  + vendored `tools/`(含 `gtc`),push `pkgs/**` 即发 artifact(内容哈希命名)+ 移指针到
  `xlings-res/{xim-index,mcpp-index}`(GitHub + GitCode)。实测 CI 成功、两端 artifact 字节一致。
- **🐛→✅ 指针合并修复**:`push_index_pointers.sh` 原先 `cp` **覆盖**组合指针 —— 单个索引仓的
  per-repo CI 单独发布时会把**兄弟 key(awesome/scode/d2x)冲掉**。改为把本仓(重)建的 key
  `update` **合并**进既有指针(每仓只更新自己的 key)。先在 xim-pkgindex 的 vendored 副本修正,
  再同步回 xlings 源 `tools/push_index_pointers.sh`(本 PR;两份现已一致)。

## 5. 一句话

**把 artifact 发布脚本(本就 standalone)搬进 xim-pkgindex 仓的 push/手动/定时 CI,artifact
按内容哈希命名解绑 xlings 版本 —— 改索引就是"push → 自动发布",不再被迫发版;客户端刷新
走轻量指针 sha 比对、失败降级本地,离线优先。**
