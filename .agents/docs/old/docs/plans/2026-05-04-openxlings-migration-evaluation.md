# openxlings 组织迁移评估报告

**日期**: 2026-05-04
**目标**: 评估将 `d2learn` 组织下的 xlings 生态仓库迁移到 `openxlings` 组织，并同步内部生态引用，使迁移后安装、索引、CI、发布、文档入口全面可用。

## 结论

迁移可行，但不能只复制代码或改 README。建议采用 GitHub 原仓库 transfer，而不是新建仓库后 force push，因为 transfer 会保留 issues、PR、releases、stars、watchers、wiki、webhooks/secrets/deploy keys 的关联，并保留 git/网页旧地址跳转。GitHub 官方约束也明确要求目标账号不能已有同名仓库或同一 fork network 中的 fork。

当前阻塞点:

- `openxlings/xlings` 当前重定向到 `openxlings/xlings-tmp`；该仓库是旧 fork，`main` 停在 `3207d8e...` / `v0.4.2`，没有 release。需要先删除或继续保留为 `xlings-tmp`，并确认 `openxlings/xlings` 名称可用于 transfer。
- 除 `xlings-tmp` 外，`openxlings/xim-pkgindex`、`openxlings/xim-pkgindex-scode`、`openxlings/xim-pkgindex-fromsource`、`openxlings/xpkgindex`、`openxlings/xim-pkgindex-skills`、`openxlings/xim-pkgindex-awesome`、`openxlings/xim-pkgindex-template` 当前查询均为 `NOT_FOUND`。
- `d2x` 系列仓库明确保留在 `d2learn`，包括 `d2learn/d2x` 和 `d2learn/xim-pkgindex-d2x`；迁移后应作为 external/d2learn 依赖声明和验证。
- GitHub Pages 不随仓库 transfer 自动重定向，因此 `https://d2learn.github.io/xim-pkgindex` 必须有新的 Pages/DNS 方案。

总体风险: **中等**。如果按 transfer + 分阶段改配置 + 三平台验证执行，风险可控；如果用新仓库替换或只改文档，风险为高。

## 仓库清单和现状

| 仓库 | 当前 d2learn 状态 | openxlings 状态 | 迁移建议 |
| --- | --- | --- | --- |
| `d2learn/xlings` | 活跃；`main=1594d3e...`；latest release `v0.4.14`，3 个资产；约 320 files | `openxlings/xlings` 当前 301 到 `openxlings/xlings-tmp`；旧 fork `main=3207d8e...`，无 release | 先释放/确认 `openxlings/xlings` 名称，再 transfer 原仓库 |
| `d2learn/xim-pkgindex` | 活跃；`main=c72b6b...`；86 个包；6 个 workflow；GitHub Pages 开启 | `NOT_FOUND` | transfer；迁移后立即重跑 pkgindex site deploy |
| `d2learn/xim-pkgindex-scode` | 活跃；15 个包；无 workflow | `NOT_FOUND` | transfer；同步 README 和注册入口 |
| `d2learn/xim-pkgindex-fromsource` | 活跃；45 个包；2 个 workflow | `NOT_FOUND` | transfer；CI 依赖 quick_install，需和 xlings 同步切换 |
| `d2learn/xpkgindex` | 静态站点生成工具；Python/HTML；20 files | `NOT_FOUND` | transfer；`xim-pkgindex` workflow 的 `pip install git+...` 要改 |
| `d2learn/xim-pkgindex-skills` | 仅 LICENSE/README；1 commit | `NOT_FOUND` | transfer 或归档后保留重定向 |
| `d2learn/xim-pkgindex-awesome` | 7 个索引包；公开索引仓库列表 | `NOT_FOUND` | transfer；它是子索引发现入口，必须优先修 |
| `d2learn/xim-pkgindex-template` | template repo；README 含 template_owner | `NOT_FOUND` | transfer；改 GitHub template 创建链接 |
| `d2learn/xim-pkgindex-d2x` | d2x 系列子索引；明确保留在 d2learn | 保留为 `d2learn/xim-pkgindex-d2x` | 不 transfer；在 openxlings 生态中标记为 external/d2learn |

取证摘要:

- GitHub API / `gh repo view` 显示 `d2learn/xlings` latest release 为 `v0.4.14`，发布时间 `2026-05-03T14:26:33Z`，3 个 assets。
- `git ls-remote` 显示 `openxlings/xlings-tmp` 的 `main` 为 `3207d8e...`，只到 `v0.4.2`；`d2learn/xlings` 的 `main` 和 `v0.4.14` 为 `1594d3e...`。
- GitHub API 对 `openxlings/xlings` 返回 301 到 repository id `1210660337`，实际 full name 为 `openxlings/xlings-tmp`；网页 `https://github.com/openxlings/xlings` 也 301 到 `https://github.com/openxlings/xlings-tmp`。

## 生态引用面

浅克隆扫描的旧生态引用数量:

| 仓库 | 文件数 | 包文件数 | workflow 数 | `d2learn` / 旧站点 / 旧镜像引用数 |
| --- | ---: | ---: | ---: | ---: |
| `xim-pkgindex` | 194 | 86 | 6 | 104 |
| `xim-pkgindex-awesome` | 12 | 7 | 0 | 38 |
| `xim-pkgindex-fromsource` | 110 | 45 | 2 | 5 |
| `xim-pkgindex-scode` | 19 | 15 | 0 | 5 |
| `xim-pkgindex-skills` | 2 | 0 | 0 | 0 |
| `xim-pkgindex-template` | 3 | 1 | 0 | 5 |
| `xlings` | 320 | 0 | 6 | 123 |
| `xpkgindex` | 20 | 0 | 0 | 2 |

必须改的功能性引用:

- `config/xlings.json` 默认 GLOBAL index repo 仍是 `https://github.com/d2learn/xim-pkgindex.git`，CN 仍是 `https://gitee.com/sunrisepeak/xim-pkgindex.git`。
- `src/core/config.cppm` 的 `Info::REPO` 和默认 index repo 仍指向 `d2learn/xlings` / `d2learn/xim-pkgindex`。
- `tools/other/quick_install.sh`、`tools/other/quick_install.ps1` 的 `GITHUB_REPO` 仍是 `d2learn/xlings`；banner 也输出 d2learn/forum。
- release package fallback JSON 仍写入 `d2learn/xim-pkgindex`、`gitee.com/sunrisepeak/xim-pkgindex`、`gitee.com/sunrisepeak/xlings`。
- xlings CI/release workflow 使用 `raw.githubusercontent.com/d2learn/xlings/.../quick_install.*` bootstrap 自身。
- `xim-pkgindex/.github/workflows/pkgindex-deloy.yml` 通过 `pip install git+https://github.com/d2learn/xpkgindex.git` 安装站点生成器。
- `xim-pkgindex/.github/workflows/gitee-sync.yml` 和 `xlings/.github/workflows/gitee-sync.yml` 的 `src` 为 `github/d2learn`，`dst` 为 `gitee/Sunrisepeak`。
- `xim-pkgindex/xim-indexrepos.lua` 把 `awesome`、`scode`、`d2x` 子索引注册到 `https://github.com/d2learn/...`。
- `xim-pkgindex/pkgs/x/xlings.lua` 中 `repo`、contributors、`0.4.4` 到 `0.4.14` release asset URL 都写死 `https://github.com/d2learn/xlings/...`。
- `xim-pkgindex-awesome` 的 README 和 `pkgs/*/*.lua` 是子索引发现入口，仍指向 `d2learn/xim-pkgindex-*`。
- `xim-pkgindex-template` README 的 `xim --add-indexrepo ...` 和 GitHub template owner 都仍是 `d2learn`。
- `xpkgindex` footer/about 模板仍显示 `https://github.com/d2learn/xpkgindex`。

可延后但需要决策的品牌/社区引用:

- `xlings.d2learn.org`、`forum.d2learn.org`、`d2learn.github.io/xim-pkgindex`。
- `d2learn/d2x`、`d2learn/xim-pkgindex-d2x`、`d2learn/xlings-project-templates`、`xlings-res/*` 资源组织明确不纳入本轮 transfer。
- Gitee/GitCode CN 镜像是否继续使用 `Sunrisepeak` / `xlings-res`，还是建立 openxlings 对应镜像。

## 推荐迁移策略

### Phase 0: 冻结和备份

1. 冻结 `d2learn/xlings`、`d2learn/xim-pkgindex`、`d2learn/xim-pkgindex-fromsource` 的发布和合并窗口。
2. 导出仓库设置、Actions secrets、Pages 设置、branch protection/rulesets、webhooks、deploy keys。
3. 记录当前 `main` SHA、tag 列表、latest release、Pages URL、CI 状态。

### Phase 1: GitHub 仓库 transfer

优先用 GitHub transfer:

1. 处理 `openxlings/xlings-tmp`:
   - 保留为历史 fork，确认 `openxlings/xlings` 可用；或
   - 若 GitHub transfer 报同名/fork network 冲突，删除/重命名/断开该 fork 后重试。
2. 将原仓库 transfer 到 `openxlings`:
   - `d2learn/xlings` -> `openxlings/xlings`
   - `d2learn/xim-pkgindex` -> `openxlings/xim-pkgindex`
   - `d2learn/xim-pkgindex-scode` -> `openxlings/xim-pkgindex-scode`
   - `d2learn/xim-pkgindex-fromsource` -> `openxlings/xim-pkgindex-fromsource`
   - `d2learn/xpkgindex` -> `openxlings/xpkgindex`
   - `d2learn/xim-pkgindex-skills` -> `openxlings/xim-pkgindex-skills`
   - `d2learn/xim-pkgindex-awesome` -> `openxlings/xim-pkgindex-awesome`
   - `d2learn/xim-pkgindex-template` -> `openxlings/xim-pkgindex-template`
   - 不迁移 d2x 系列: `d2learn/d2x`、`d2learn/xim-pkgindex-d2x` 保留在 d2learn
3. 验证旧 URL 跳转仍有效，但不要把跳转作为长期配置依赖。

GitHub transfer 注意点:

- 需要源仓库 admin 权限，以及在目标组织创建仓库的权限。
- 目标账号不能已有同名仓库或同一 fork network 的 fork。
- GitHub 会转移 issues、PR、wiki、stars、watchers、releases、webhooks、secrets、deploy keys 等，但 GitHub Pages URL 不会自动重定向。

### Phase 2: 代码和配置同步

在 transfer 后提交一组同步 PR，建议按仓库拆分:

1. `openxlings/xlings`
   - `src/core/config.cppm`: `Info::REPO` 改为 `https://github.com/openxlings/xlings`，默认 GLOBAL index repo 改为 `https://github.com/openxlings/xim-pkgindex.git`。
   - `config/xlings.json`: GLOBAL index repo 改为 openxlings；`repo` 字段改为 GitHub/openxlings 或新的 CN 镜像。
   - `tools/other/quick_install.*`: `GITHUB_REPO=openxlings/xlings`，示例 raw URL 和 banner 更新。
   - `tools/*_release.*`: fallback `.xlings.json` 改为 openxlings。
   - `.github/workflows/*`: raw quick_install URL 改为 openxlings；release workflow 的说明和 pinned bootstrap 保持可追踪。
   - README、docs、i18n、AUR PKGBUILD、tests 默认 URL 同步。

2. `openxlings/xim-pkgindex`
   - `xim-indexrepos.lua`: `awesome`、`scode`、`d2x` 改为 openxlings。
   - `pkgs/x/xlings.lua`: `repo`、contributors、release URLs 改为 openxlings。历史版本可依赖 GitHub redirect，但建议统一改掉，避免未来旧 owner redirect 被破坏。
   - `.github/workflows/pkgindex-deloy.yml`: `pip install git+https://github.com/openxlings/xpkgindex.git`。
   - `.github/workflows/gitee-sync.yml`: 如果继续同步 Gitee，调整 `src`/`dst`/`static_list` 和 secrets。
   - README/docs/badges/issue links 改为 openxlings。

3. `openxlings/xim-pkgindex-awesome`
   - README 列表、GitHub template 创建链接、`pkgs/*/*.lua` 的 `repo`/`docs`。
   - 若 `xim-pkgindex-d2x` 暂不迁移，明确标记为 external/d2learn。

4. `openxlings/xim-pkgindex-scode`、`openxlings/xim-pkgindex-fromsource`、`openxlings/xim-pkgindex-template`
   - README 的 `xim --add-indexrepo` 命令改为 openxlings。
   - CI quick_install raw URL 改为 openxlings。
   - package metadata 的 `repo` 字段改为 openxlings。

5. `openxlings/xpkgindex`
   - footer/about 模板链接改为 openxlings。
   - 可考虑发布 PyPI 或 GitHub release，减少 `xim-pkgindex` Pages workflow 对 git URL 的耦合。

### Phase 3: Pages、域名和镜像

建议选择一个主入口:

- 最小变更: 保留 `xlings.d2learn.org` 和 `forum.d2learn.org`，只把 GitHub owner 改为 openxlings。风险最低，但品牌不完全迁移。
- 完整迁移: 启用 `xlings.openxlings.org` 或 `openxlings.github.io/xlings`，`xim-pkgindex` 使用 `openxlings.github.io/xim-pkgindex` 或 `pkgindex.openxlings.org`。需要 DNS、Pages、文档、README、i18n 同步。

无论选择哪种，`d2learn.github.io/xim-pkgindex` 不会自动跳到新 owner，需要:

1. 在 `openxlings/xim-pkgindex` 启用 Pages。
2. 更新 README、官网、i18n、package index portal 链接。
3. 如需旧 URL 兼容，在 d2learn 侧保留 Pages stub 做跳转说明。

CN 镜像建议单独决策:

- 若继续用 `gitee.com/sunrisepeak/*` 和 `gitcode.com/xlings-res/*`，配置中保留 CN 但文档标注为镜像服务。
- 若迁移到 openxlings 命名，应新增 Gitee/GitCode 组织或仓库，并更新 `gitee-sync` secrets。

### Phase 4: 验证清单

迁移后必须通过这些检查，才算“全面可用”:

1. GitHub 仓库:
   - `gh repo view openxlings/xlings` 显示非 fork 原仓库，latest release 为当前最新 tag。
   - 8 个原清单仓库均存在于 `openxlings`；`d2learn/d2x` 和 `d2learn/xim-pkgindex-d2x` 仍存在于 `d2learn`。
   - `git ls-remote https://github.com/openxlings/xlings.git HEAD refs/tags/v0.4.14` 正常。
   - 旧 `https://github.com/d2learn/...` URL 仍跳转，且 d2learn 旧位置没有新建同名仓库破坏 redirect。

2. 安装和自举:
   - Linux/macOS:
     ```bash
     XLINGS_NON_INTERACTIVE=1 curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
     xlings -h
     xlings config
     xim --update index
     ```
   - Windows:
     ```powershell
     $env:XLINGS_NON_INTERACTIVE='1'
     irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
     xlings -h
     xlings config
     xim --update index
     ```

3. 包索引:
   - `xlings search xlings`
   - `xlings install xlings@0.4.14 -y`
   - `xim --add-indexrepo scode:https://github.com/openxlings/xim-pkgindex-scode.git`
   - `xim --add-indexrepo fromsource:https://github.com/openxlings/xim-pkgindex-fromsource.git`
   - `xlings install awesome:scode -y` 或等价的子索引安装/搜索 smoke。

4. CI/发布:
   - `openxlings/xlings` 三平台 CI 和 Release workflow 通过。
   - `openxlings/xim-pkgindex` 的 `ci-test.yml`、`ci-xpkg-test.yml`、`pkgindex-deloy.yml` 通过。
   - `openxlings/xim-pkgindex-fromsource` 的两个 CI 通过。
   - GitHub Pages 新 URL 可访问，About 页面 build info 指向 openxlings commit。

5. 引用审计:
   - 对所有迁移仓库运行:
     ```bash
     rg -n "github.com/d2learn|raw.githubusercontent.com/d2learn|d2learn.github.io|xlings.d2learn.org|forum.d2learn.org|gitee.com/sunrisepeak"
     ```
   - 所有结果必须归类为 `legacy compatibility`、`external project`、`CN mirror` 或已修复。

## 风险和缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 目标 org 已有同名 fork/network 冲突 | transfer 失败 | 先处理 `openxlings/xlings-tmp`，用 GitHub transfer 前做 dry-run/手动确认 |
| Pages 不自动跳转 | 包索引门户和文档入口断链 | 新 Pages 先上线；旧 Pages 保留跳转页；所有链接改为新 URL |
| quick_install 自举依赖旧 owner | CI/用户安装仍从 d2learn 拉包 | transfer 后第一批 PR 先改 quick_install 和 CI raw URL；验证 `XLINGS_VERSION` pin |
| `xim-pkgindex` release URL 写死旧 owner | `xlings install xlings` 依赖 redirect | 批量改 `pkgs/x/xlings.lua`；保留 checksum 不变；重跑 package index CI |
| gitee-sync secrets 不存在 | 镜像停止更新 | 迁移前导出/重建 secrets；如果不迁移 CN 镜像，显式暂停 workflow |
| `xim-pkgindex-d2x` 保留在 d2learn | openxlings 生态中可能被误判为漏迁 | 显式标注 external/d2learn，并在审计规则中允许 d2x 系列 |
| 用户本地 clone/remotes 仍指向旧地址 | 可用但混乱 | 发布迁移公告，建议 `git remote set-url origin https://github.com/openxlings/<repo>.git` |
| d2learn 旧位置新建同名仓库 | GitHub redirect 被永久破坏 | transfer 后锁定旧 owner 命名，避免创建同名 repo/fork |

## Go / No-Go 判断

可以执行迁移的前置条件:

- `openxlings/xlings` 名称可用于 transfer，且不会被 `xlings-tmp` fork network 阻塞。
- `openxlings` 组织 owner/admin 权限确认完毕。
- Actions secrets、Pages、branch protection/rulesets 迁移计划已确认。
- 已决定 `d2x`、`xim-pkgindex-d2x`、`xlings-project-templates` 保留在 d2learn；仍需决定 `xlings-res`、Gitee/GitCode、官网/论坛的保留或迁移边界。
- 已准备迁移后一批同步 PR 和验证脚本。

No-Go 条件:

- 只能创建空仓库/镜像，不能 transfer 原仓库。
- 未确认 Pages 新入口。
- 没有权限恢复 CI secrets 或发布 release。
- 不声明 `d2x` 系列为 external/d2learn，但仍要求 awesome/子索引全面可用。

## 建议执行顺序

1. 先转移 `xlings` 和 `xpkgindex`，发布 `openxlings/xlings` quick_install 可用版本。
2. 转移 `xim-pkgindex`，改默认 index repo、Pages workflow、`pkgs/x/xlings.lua`。
3. 转移 `xim-pkgindex-awesome`、`xim-pkgindex-scode`、`xim-pkgindex-fromsource`、`xim-pkgindex-template`、`xim-pkgindex-skills`。
4. 声明并验证 `d2x` 系列和 `xlings-project-templates` 保留在 d2learn。
5. 跑三平台安装/索引/CI/Pages 验证。
6. 发布迁移公告，保留旧 URL redirect 和旧 Pages 跳转说明至少一个 release 周期。

## 参考来源

- GitHub transfer 文档: https://docs.github.com/en/repositories/creating-and-managing-repositories/transferring-a-repository
- `d2learn/xlings`: https://github.com/d2learn/xlings
- `openxlings/xlings-tmp`: https://github.com/openxlings/xlings-tmp
- `d2learn/xim-pkgindex`: https://github.com/d2learn/xim-pkgindex
- `d2learn/xim-pkgindex-awesome`: https://github.com/d2learn/xim-pkgindex-awesome
- `d2learn/xim-pkgindex-scode`: https://github.com/d2learn/xim-pkgindex-scode
- `d2learn/xim-pkgindex-fromsource`: https://github.com/d2learn/xim-pkgindex-fromsource
- `d2learn/xpkgindex`: https://github.com/d2learn/xpkgindex
- `d2learn/xim-pkgindex-skills`: https://github.com/d2learn/xim-pkgindex-skills
- `d2learn/xim-pkgindex-template`: https://github.com/d2learn/xim-pkgindex-template
- `d2learn/xim-pkgindex-d2x`: https://github.com/d2learn/xim-pkgindex-d2x
- `d2learn/xlings-project-templates`: https://github.com/d2learn/xlings-project-templates
