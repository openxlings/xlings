# openxlings 仓库转移后引用迁移报告

**日期**: 2026-05-04
**范围**: 已 transfer 到 `openxlings` 的 xlings 生态仓库，以及明确保留在 `d2learn` 的 d2x 系列仓库。
**目标**: 找出仓库内仍指向旧 GitHub owner、旧 Pages、旧 badge、旧自举脚本的引用，并拆分后续 PR。

## 当前仓库归属

已迁移到 `openxlings`:

- `openxlings/xlings`
- `openxlings/xim-pkgindex`
- `openxlings/xim-pkgindex-scode`
- `openxlings/xim-pkgindex-fromsource`
- `openxlings/xpkgindex`
- `openxlings/xim-pkgindex-skills`
- `openxlings/xim-pkgindex-awesome`
- `openxlings/xim-pkgindex-template`

明确保留在 `d2learn`:

- `d2learn/d2x`
- `d2learn/xim-pkgindex-d2x`

本轮不处理:

- `d2learn/xlings-project-templates`: 当前在 `xim-pkgindex` 的包元数据中仍被引用；本轮不 transfer，按 external/d2learn 保留。
- `xlings-res/*`、`gitee.com/sunrisepeak/*`、`gitcode.com/xlings-res/*`: 当前属于资源镜像/CN 镜像范围，不随本次 GitHub transfer 自动迁移。

## 链接可用性取证

已确认:

- `https://d2learn.github.io/xim-pkgindex` -> HTTP 404
- `https://openxlings.github.io/xim-pkgindex/` -> HTTP 200
- `https://xlings.d2learn.org/documents/xim/intro.html` -> HTTP 200
- `https://forum.d2learn.org/category/9/xlings` -> HTTP 200
- `https://github.com/d2learn/xim-pkgindex/actions/workflows/ci-test.yml` 会跳转到 `openxlings/xim-pkgindex`，但 README/badge 仍应改成 canonical `openxlings` 地址。

结论:

- `d2learn.github.io/xim-pkgindex` 必须迁移到 `openxlings.github.io/xim-pkgindex/`。
- `xlings.d2learn.org` 和 `forum.d2learn.org` 目前可用；如果继续采用“最小品牌迁移”，暂不强制替换，但应在审计报告中归类为保留项。

## 待迁移引用统计

扫描规则只统计应迁移到 `openxlings` 的 GitHub/Pages 引用，不把 `d2x` 系列算作错误。

| 仓库 | 待改匹配数 | 涉及文件数 | 主要类型 |
| --- | ---: | ---: | --- |
| `openxlings/xlings` | 30 | 12 | 项目内历史/agent 文档、mcpp skill、旧 quick-install 示例、旧 PR/仓库链接 |
| `openxlings/xim-pkgindex` | 71 | 21 | README、workflow bootstrap、Pages URL、子索引 URL、包元数据、release asset URL |
| `openxlings/xim-pkgindex-scode` | 4 | 2 | README、`pkgindex-build.lua` |
| `openxlings/xim-pkgindex-fromsource` | 4 | 2 | README、AGENTS；另有 workflow bootstrap 需改 |
| `openxlings/xpkgindex` | 2 | 2 | HTML 模板 footer/about 链接 |
| `openxlings/xim-pkgindex-skills` | 0 | 0 | 无需 PR |
| `openxlings/xim-pkgindex-awesome` | 14 | 7 | README、template 创建链接、子索引包元数据 |
| `openxlings/xim-pkgindex-template` | 4 | 2 | README、模板包元数据 |

## 必须迁移的引用类型

### GitHub 仓库链接

这些应统一从 `d2learn` 改为 `openxlings`:

- `https://github.com/d2learn/xlings` -> `https://github.com/openxlings/xlings`
- `https://github.com/d2learn/xim-pkgindex` -> `https://github.com/openxlings/xim-pkgindex`
- `https://github.com/d2learn/xim-pkgindex-scode` -> `https://github.com/openxlings/xim-pkgindex-scode`
- `https://github.com/d2learn/xim-pkgindex-fromsource` -> `https://github.com/openxlings/xim-pkgindex-fromsource`
- `https://github.com/d2learn/xpkgindex` -> `https://github.com/openxlings/xpkgindex`
- `https://github.com/d2learn/xim-pkgindex-skills` -> `https://github.com/openxlings/xim-pkgindex-skills`
- `https://github.com/d2learn/xim-pkgindex-awesome` -> `https://github.com/openxlings/xim-pkgindex-awesome`
- `https://github.com/d2learn/xim-pkgindex-template` -> `https://github.com/openxlings/xim-pkgindex-template`

保留:

- `https://github.com/d2learn/d2x`
- `https://github.com/d2learn/xim-pkgindex-d2x`

### Raw install/bootstrap URL

这些会影响 CI 和一键安装，必须改:

- `https://raw.githubusercontent.com/d2learn/xlings/main/tools/other/quick_install.sh`
- `https://raw.githubusercontent.com/d2learn/xlings/main/tools/other/quick_install.ps1`

目标:

- `https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh`
- `https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1`

### GitHub Actions badge / workflow link

README 中的 badge 和 workflow URL 应改成 canonical openxlings 地址，避免依赖 redirect:

- `https://github.com/d2learn/xim-pkgindex/actions/...`
- `https://github.com/d2learn/<repo>/actions/...`

### GitHub Pages

必须改:

- `https://d2learn.github.io/xim-pkgindex` -> `https://openxlings.github.io/xim-pkgindex/`

目前 `openxlings/xim-pkgindex` Pages API 显示:

- `html_url`: `https://openxlings.github.io/xim-pkgindex/`
- `source`: `main` / `/`

## 按仓库拆分的建议 PR

### PR 1: `openxlings/xlings`

**建议标题**: `chore: update canonical openxlings references`

范围:

- 已完成的运行时和安装入口迁移保持在本仓库当前工作树中:
  - `src/core/config.cppm`
  - `config/xlings.json`
  - `tools/other/quick_install.sh`
  - `tools/other/quick_install.ps1`
  - `.github/workflows/*`
  - release scripts
  - README/docs/project skills
- 补充处理仍残留在 tracked 非计划文档中的旧链接:
  - `.agents/docs/changelog.md` 中历史 PR/commit 链接可选择保留为历史记录，或统一改为 redirect 后的新 canonical URL。
  - `.agents/docs/mcpp-version/*` 和 `.agents/docs/quick_start.md` 中 quick install / GitHub API 示例应改为 `openxlings`。
  - `.agents/skills/mcpp-style-ref/SKILL.md` 中 raw quick install 和 GitHub 链接应改为 `openxlings`。
- `tests/unit/test_main.cpp` 中 `../d2learn/xim-pkgindex` 是本地路径 fixture，不是 GitHub URL；建议保留或单独改名为 neutral path。

验证:

- `bash tests/e2e/github_owner_migration_audit.sh`
- `git diff --check`
- `bash -n tools/other/quick_install.sh tools/linux_release.sh tools/macos_release.sh`

### PR 2: `openxlings/xim-pkgindex`

**建议标题**: `chore: update openxlings repository and Pages links`

范围:

- `README.md`
  - 标题中的 `[xlings](https://github.com/d2learn/xlings)` 改为 `openxlings/xlings`。
  - `Package Index` 改为 `https://openxlings.github.io/xim-pkgindex/`。
  - workflow badges 改为 `https://github.com/openxlings/xim-pkgindex/actions/...`。
  - 表格中的 `xim-pkgindex-template`、`xim-pkgindex-fromsource` 改为 `openxlings`。
  - `xim-pkgindex-d2x` 保留 `d2learn`，并标注为 d2x external index。
- `.github/workflows/ci-test.yml`、`.github/workflows/ci-xpkg-test.yml`
  - quick install raw URL 改为 `openxlings/xlings`。
- `.github/workflows/pkgindex-deloy.yml`
  - `pip install git+https://github.com/d2learn/xpkgindex.git` 改为 `openxlings/xpkgindex.git`。
  - 注释中的 `d2learn/xpkgindex` 改为 `openxlings/xpkgindex`。
- `.github/workflows/gitee-sync.yml`
  - `src: github/d2learn` 改为 `github/openxlings`；`dst` 是否仍为 `gitee/Sunrisepeak` 需要确认镜像策略。
- `xim-indexrepos.lua`
  - `awesome` 和 `scode` 改为 `openxlings`。
  - `d2x` 保留 `d2learn/xim-pkgindex-d2x`。
- `docs/V0/*`、`docs/V1/*`、`docs/migrations/*`
  - 当前仍有 issue、PR、raw quick install、旧 repo 链接；历史 PR 链接可保留或改 canonical。
- `pkgs/x/xlings.lua`
  - `repo`、contributors、release asset URL 改为 `openxlings/xlings`。
- `pkgs/x/xvm.lua`
  - `repo`、contributors 改为 `openxlings/xlings`。
- `pkgs/*/*.lua`
  - 指向 `d2learn/xim-pkgindex` 的本仓库包元数据改为 `openxlings/xim-pkgindex`。
- `pkgs/d/d2x.lua`
  - 保留 `https://github.com/d2learn/d2x`。
- `pkgs/x/xlings-project-templates.lua`
  - 保留 `d2learn/xlings-project-templates`，本轮不处理。

验证:

- `rg -n "github.com/d2learn|raw.githubusercontent.com/d2learn|d2learn.github.io" README.md .github docs pkgs xim-indexrepos.lua`
- `xim --update index`
- `xlings install xlings@0.4.14 -y`
- Pages workflow 成功后访问 `https://openxlings.github.io/xim-pkgindex/`

### PR 3: `openxlings/xim-pkgindex-awesome`

**建议标题**: `chore: update openxlings sub-index references`

范围:

- `README.md`
  - GitHub template 创建链接的 `template_owner=d2learn` 改为 `template_owner=openxlings`。
  - 指向 `xim-pkgindex-awesome/blob/...` 的链接改为 `openxlings`。
  - xlings 工具和 xim 主索引链接改为 `openxlings`。
  - `xim-pkgindex-d2x` 保留 d2learn。
- `pkgindex-build.lua`
  - repo 改为 `openxlings/xim-pkgindex-awesome`。
- `pkgs/a/awesome.lua`、`pkgs/f/fromsource.lua`、`pkgs/s/scode.lua`、`pkgs/t/template.lua`、`pkgs/x/xim.lua`
  - repo 改为对应 `openxlings/*`。
- `pkgs/d/d2x.lua`
  - 保留 `d2learn/xim-pkgindex-d2x`。

验证:

- `rg -n "github.com/d2learn/xim-pkgindex-(awesome|fromsource|scode|template)|github.com/d2learn/xim-pkgindex([^A-Za-z0-9._-]|$)|template_owner=d2learn"`
- `xim --add-indexrepo awesome:https://github.com/openxlings/xim-pkgindex-awesome.git`

### PR 4: `openxlings/xim-pkgindex-scode`

**建议标题**: `chore: update openxlings links`

范围:

- `README.md`
  - add-indexrepo 命令改为 `https://github.com/openxlings/xim-pkgindex-scode.git`。
  - xlings 工具和 xim 主索引链接改为 `openxlings`。
  - 论坛链接保留 `forum.d2learn.org`，除非后续品牌策略决定迁移。
- `pkgindex-build.lua`
  - repo 改为 `openxlings/xim-pkgindex-scode`。

验证:

- `rg -n "github.com/d2learn|raw.githubusercontent.com/d2learn|d2learn.github.io"`
- `xim --add-indexrepo scode:https://github.com/openxlings/xim-pkgindex-scode.git`

### PR 5: `openxlings/xim-pkgindex-fromsource`

**建议标题**: `chore: update openxlings bootstrap and docs links`

范围:

- `README.md` 和 `AGENTS.md`
  - xlings / xim-pkgindex 链接改为 `openxlings`。
  - add-indexrepo 命令改为 `https://github.com/openxlings/xim-pkgindex-fromsource.git`。
- `.github/workflows/ci-test.yml`、`.github/workflows/ci-xpkg-test.yml`
  - quick install raw URL 改为 `openxlings/xlings`。

验证:

- `rg -n "github.com/d2learn|raw.githubusercontent.com/d2learn|d2learn.github.io"`
- `xim --add-indexrepo fromsource:https://github.com/openxlings/xim-pkgindex-fromsource.git`

### PR 6: `openxlings/xim-pkgindex-template`

**建议标题**: `chore: update openxlings template references`

范围:

- `README.md`
  - add-indexrepo 示例改为 `openxlings/xim-pkgindex-template.git`。
  - xlings 工具和 xim 主索引链接改为 `openxlings`。
- `pkgs/x/xpackage.lua`
  - repo 改为 `openxlings/xim-pkgindex`。

验证:

- 从 GitHub template 创建新仓库，确认默认 README 中不再带旧 owner。
- `rg -n "github.com/d2learn|raw.githubusercontent.com/d2learn|d2learn.github.io"`

### PR 7: `openxlings/xpkgindex`

**建议标题**: `chore: update openxlings xpkgindex links`

范围:

- `xpkgindex/templates/base.html`
- `xpkgindex/templates/about.html`

把 footer/about 中的 `https://github.com/d2learn/xpkgindex` 改为 `https://github.com/openxlings/xpkgindex`。

验证:

- `rg -n "github.com/d2learn/xpkgindex"`
- 在 `openxlings/xim-pkgindex` 的 Pages workflow 中用新 URL 安装并生成站点。

### 无需 PR: `openxlings/xim-pkgindex-skills`

当前扫描无旧 owner / Pages / badge 引用。

## 建议执行顺序

1. 先合并 `openxlings/xlings` 当前迁移 PR，使 quick install、release、默认 index repo 都指向新 org。
2. 合并 `openxlings/xpkgindex`，保证 `xim-pkgindex` Pages workflow 可从新 owner 安装生成器。
3. 合并 `openxlings/xim-pkgindex`，这是用户可见断链最多的仓库，尤其是 README、badge、Pages、package release URL。
4. 合并 `xim-pkgindex-awesome`、`xim-pkgindex-scode`、`xim-pkgindex-fromsource`、`xim-pkgindex-template`。
5. 最后重新跑全生态引用审计，允许项只应剩:
   - `d2learn/d2x`
   - `d2learn/xim-pkgindex-d2x`
   - `xlings.d2learn.org`
   - `forum.d2learn.org`
   - 明确保留的 CN/resource mirrors

## 统一审计命令建议

每个仓库 PR 后运行:

```bash
rg -n "github.com/d2learn|raw.githubusercontent.com/d2learn|d2learn.github.io|github/d2learn|template_owner=d2learn"
```

然后逐条归类:

- 必须改: 已迁移到 `openxlings` 的仓库、raw install、badge、Pages。
- 允许保留: `d2x` 系列、官网、论坛、CN/resource mirrors。
- 允许保留: `xlings-project-templates`。
