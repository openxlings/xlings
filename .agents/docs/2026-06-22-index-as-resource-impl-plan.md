# Index-as-Resource 实施计划(Y-asset 落地)

> **For agentic workers:** 配合 superpowers:executing-plans 逐任务实现。步骤用 `- [ ]` 跟踪。
> 设计依据:`2026-06-21-pkgindex-redesign-proposal.md` §7.7–7.9。

**Goal:** 把 xim 包索引从"运行时 git clone GitHub"改为"作为版本化资源工件从 `xlings-res` 下载"
(Y-asset),`xlings update` 走 HTTP 资源路径(自动获得延迟重排+卡顿看门狗,根治 G2),git 仅作兜底。

**Architecture:** 索引在 CI 构建为 `xim-index-<ver>.tar.gz` + `manifest.json`,发布到
`xlings-res/xim-index` 的 GitHub Release(`gh`)并镜像到 GitCode(`gtc`)。运行时用既有
`Config::resource_server()`(GLOBAL/CN + 延迟探测 + fallback)拼出工件 URL,经 tinyhttps 下载、
校验 sha256、原子解压到 `data/xim-pkgindex`。

**Tech Stack:** C++23 modules(mcpp/gcc16 构建)、tinyhttps、nlohmann::json、bash/gh/gtc、GitHub Actions。

## Global Constraints

- 版本号:`0.4.51` → **`0.4.52`**(`src/core/config.cppm` VERSION + `mcpp.toml` version,二者必须一致)。
- **不考虑老用户 / 完全覆盖**:无需向后兼容旧 update 行为;但**保留 git 兜底**以保鲁棒性(非破坏)。
- **为 X-full 留余地**:`manifest.json` 带 `format_version` + 预留 `signature` 字段(现填 null);
  fetch 路径结构上允许将来加"指针 + ETag/304 + 签名校验"而不重写。
- 构建验证用隔离 HOME:`export XLINGS_HOME=$(mktemp -d)`、`export MCPP_HOME=$(mktemp -d)`,
  不得污染真实 `~/.xlings`(见 AGENTS.md)。
- 本地构建工具链:`xlings use gcc@16.1.0`(避免 glibc/musl 链接错误)。
- `xlings-res` 可用 `gh`(github)与 `gtc`(gitcode)操作。

---

## 架构总览(顶层设计)

```
事实源 (不变)            构建/发布 (新)                分发 (复用 xlings-res)        运行时 (改 update)
─────────────          ──────────────               ────────────────────        ──────────────────
d2learn/xim-pkgindex   CI: build_index →            xlings-res/xim-index         Config::resource_server()
  pkgs/*.lua (git)       catalog + tar.gz +           ├ GitHub Release (gh)        → {server}/xim-index/
  + awesome/scode/d2x    manifest.json (sha256,       └ GitCode  mirror  (gtc)        releases/download/<tag>/
                         format_version,                                            → tinyhttps 下载+校验
                         signature:null)                                           → 原子解压 data/xim-pkgindex
                                                                                   → 失败回退 git (兜底)
```

**工件契约(format_version=1):**
```
xim-index-<ver>.tar.gz        # 内含: pkgs/  .xpkgindex.json  xim-indexrepos.lua  .xlings-index-cache.json
manifest.json                 # 指针/清单, 与工件同发布, 也内嵌一份于工件
  { "format_version": 1,
    "index_version": "0.4.52",          # 现用 release ver; 未来可换单调整数
    "generated_at": "<iso8601>",
    "source_commit": "<xim-pkgindex sha>",
    "artifact": { "name": "xim-index-0.4.52.tar.gz", "sha256": "...", "size": 123 },
    "signature": null }                 # 预留: 未来 minisign
```

**子索引(awesome/scode/d2x):** 同构,各发 `xim-index-<name>-<ver>.tar.gz` + 各自 manifest;
P3 阶段接入,主索引先行。

---

## 阶段与任务

### Phase 0 — 版本与文档基线

#### Task 0.1: 版本号 bump + 计划文档落库
**Files:** Modify `src/core/config.cppm:16`、`mcpp.toml:3`;Add 本计划 + 两份设计文档(已存在,纳入提交)
- [ ] 改 `VERSION = "0.4.51"` → `"0.4.52"`(config.cppm:16)
- [ ] 改 `version = "0.4.51"` → `"0.4.52"`(mcpp.toml:3)
- [ ] `git add .agents/docs/2026-06-21-*.md .agents/docs/2026-06-22-*.md src/core/config.cppm mcpp.toml`
- [ ] commit: `chore(release): bump 0.4.52 + index-as-resource design/plan`

---

### Phase 1 — 发布管线(构建 + 发布工件)

#### Task 1.1: 索引工件构建脚本
**Files:** Create `tools/build_xim_index_artifact.sh`;Test `tests/e2e/build_xim_index_artifact_test.sh`
- 输入:已 clone 的 xim-pkgindex 目录 + 版本号;输出:`xim-index-<ver>.tar.gz` + `manifest.json`。
- 复用现有 `package_xim_index.sh` 的 clone 逻辑;新增:剥离 `.git`、用 xlings 生成
  `.xlings-index-cache.json`(`xlings` 已有 build_index;或保留运行时首次生成)、算 sha256/size、写 manifest。
- [ ] 写脚本:clone(GLOBAL/CN 由 `XLINGS_RELEASE_MIRROR`)→ 去 `.git` → tar.gz → sha256 → 写 manifest.json(format_version=1, signature:null)
- [ ] 写 e2e:跑脚本,断言产出 tar.gz + manifest.json、manifest.sha256 与文件实际 sha256 一致、tar 内含 `pkgs/`
- [ ] 跑 e2e 验证通过
- [ ] commit: `feat(release): build xim-index artifact + manifest`

#### Task 1.2: 发布到 xlings-res(gh + gtc)脚本
**Files:** Create `tools/publish_xim_index.sh`
- 用 `gh release create/upload` 推到 `xlings-res/xim-index`(github);用 `gtc release create/upload` 推到 gitcode 镜像。
- tag = `v<ver>`;额外维护一个 `latest` 资产(manifest 复制为 `xim-index-latest.json` 作指针)。
- [ ] 写脚本(幂等:release 已存在则只 upload --clobber)
- [ ] 干跑校验参数(不真正发布):`--dry-run` 分支打印将执行的 gh/gtc 命令
- [ ] commit: `feat(release): publish xim-index to xlings-res (gh+gtc)`

#### Task 1.3: 创建 xlings-res/xim-index 仓库(两端)
**(运维步骤,执行期做)**
- [ ] `gh repo create xlings-res/xim-index --public -d "xim package index artifacts for xlings"`
- [ ] `gtc repo create xim-index`(gitcode `xlings-res`)
- [ ] 记录两端 URL,确认与 `config.cppm` resource server 基址一致

---

### Phase 2 — 运行时:索引作为资源获取

#### Task 2.1: manifest 解析 + 工件 URL 构建(纯逻辑,可单测)
**Files:** Create `src/core/xim/indexfetch.cppm`;Test `tests/unit/test_indexfetch.cpp`
- **Produces:**
  - `struct IndexManifest { int format_version; std::string index_version, generated_at, source_commit; std::string artifact_name, artifact_sha256; std::size_t artifact_size; };`
  - `std::optional<IndexManifest> parse_manifest(std::string_view json);`
  - `std::vector<std::string> artifact_urls(std::string_view name, std::string_view mirror);`  // 基于 Config::resource_servers()
- [ ] 写单测:parse 合法 manifest;拒绝缺字段;`artifact_urls` 对 GLOBAL/CN 各拼出正确 `{server}/xim-index/releases/download/v<ver>/<name>`
- [ ] 跑测试看失败(未实现)
- [ ] 实现 `parse_manifest`(nlohmann::json,容错)+ `artifact_urls`(调 `Config::resource_servers(mirror)`)
- [ ] 跑测试通过
- [ ] commit: `feat(xim): index manifest parse + artifact url build`

#### Task 2.2: 下载 + 校验 + 原子解压
**Files:** Modify `src/core/xim/indexfetch.cppm`;Modify `src/core/xim/repo.cppm`
- **Produces:** `bool fetch_index_artifact(const std::filesystem::path& destIndexDir, std::string& err);`
  - 取 manifest(`xim-index-latest.json`,经 tinyhttps + mirror::expand + adaptive::reorder)→ parse
  - 下载 `artifact.name` 候选 URL(tinyhttps:续传+卡顿看门狗+逐候选)→ 校验 sha256 == manifest
  - 解压到 `destIndexDir.tmp` → 校验含 `pkgs/` → 原子 rename 覆盖 → 写 `.xlings-index-version`(记 index_version)
  - 任一步失败:清理 tmp,返回 false(由调用方决定是否 git 兜底)
- [ ] 实现 `fetch_index_artifact`(复用 tinyhttps download_file + libarchive 解压)
- [ ] commit: `feat(xim): fetch+verify+atomic-extract index artifact`

#### Task 2.3: 接入 update / install 路径(artifact 优先,git 兜底)
**Files:** Modify `src/core/xim/repo.cppm`(`sync_all_repos`)、`src/core/xim/commands.cppm`(cmd_update:919)
- `sync_all_repos(force)`:先 `fetch_index_artifact()`;成功即跳过 git;失败且未禁用则回退现有 git 同步。
- env 开关 `XLINGS_INDEX_SOURCE=artifact|git|auto`(默认 auto=先 artifact 后 git),`XLINGS_INDEX_ARTIFACT_URL` 覆盖(测试/镜像)。
- [ ] 改 `sync_all_repos` 加 artifact-first 分支 + env 开关
- [ ] commit: `feat(xim): index update via artifact, git fallback`

#### Task 2.4: e2e — 工件更新闭环(mock server)
**Files:** Create `tests/e2e/index_artifact_update_test.sh`
- 用 python http.server 起 mock 资源服务器,放 manifest + tar.gz;`XLINGS_INDEX_ARTIFACT_URL` 指向它;
  跑 `xlings update`;断言 `data/xim-pkgindex/pkgs` 出现且 sha256 校验生效(篡改→失败回退)。
- [ ] 写 e2e
- [ ] 跑通(隔离 HOME)
- [ ] commit: `test(e2e): index artifact update happy + tamper path`

---

### Phase 3 — 子索引 + 发布集成 + CI

#### Task 3.1: 子索引(awesome/scode/d2x)工件化
- [ ] `build_xim_index_artifact.sh` 支持 `--name <sub>`;`publish_xim_index.sh` 发各子工件
- [ ] 运行时 `discover_sub_repos` 改为优先取子工件(同 2.2 逻辑),git 兜底
- [ ] commit: `feat(xim): sub-index artifacts (awesome/scode/d2x)`

#### Task 3.2: release.yml 集成 + 新工件发布 job
**Files:** Modify `.github/workflows/release.yml`;可能 Add `.github/workflows/publish-index.yml`(在 xim-pkgindex 仓)
- 在 create-release 后新增 job:跑 `build_xim_index_artifact.sh` + `publish_xim_index.sh`(用 secrets 的 gh/gtc token)。
- 或在 xim-pkgindex 仓加独立 workflow(解耦新鲜度)。先在 xlings release 内集成(简单),X-full 再拆。
- [ ] 加 publish job + 所需 secrets(`GITCODE_TOKEN`)
- [ ] commit: `ci(release): build+publish xim-index artifact`

#### Task 3.3: 全 CI 绿
- [ ] push 分支,触发 `xlings-ci-linux/macos/windows/archlinux/linux-root`
- [ ] 逐平台修复编译/测试失败(C++23 模块、e2e)
- [ ] 全绿后开 PR

---

### Phase 4 — PR + 生态验证 + 汇报

#### Task 4.1: 开 PR(版本号)
- [ ] `gh pr create` 标题含 `v0.4.52`;正文链三份设计/计划文档 + 验收清单
#### Task 4.2: 触发 release(workflow_dispatch)发 0.4.52 + 首个 xim-index 工件
- [ ] 确认 `xlings-res/xim-index` 两端有 `v0.4.52` 工件 + manifest
#### Task 4.3: 端到端生态验证
- [ ] 全新环境 `quick_install.sh` 装 0.4.52 → `xlings update`(走 artifact)→ `xlings install <pkg>` 成功
- [ ] CN 路径(gitcode)同样验证
#### Task 4.4: 综合汇报(更新本计划完成态 + 总结)

---

## Self-Review 检查
- 覆盖:发布(P1)、运行时(P2)、子索引(P3.1)、CI(P3)、PR/生态(P4)、版本(P0) —— 对齐 §7.6 清单 B/C/D + E。
- 留余地:manifest `format_version`/`signature` 预留;fetch 经 manifest 间接寻址,未来可加 ETag/304/签名。
- 鲁棒性:sha256 校验 + 原子 rename + git 兜底 + env 逃生开关。
- 风险:C++23 模块跨平台编译(最大不确定性,P3.3 迭代);gtc token 在 CI 的可用性(P3.2 需配 secret)。
