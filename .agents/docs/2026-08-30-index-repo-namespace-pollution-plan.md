# `index_repos` 的特权语义落错了条目 —— 根因与修复方案

> 状态:**已实现**(2026.8.30.1,PR #575,分支 `fix/index-repos-peer-semantics`)。
> 落地结果与**被实测推翻的判断**见 §7,逐条标注而不是悄悄改掉。
> 所有数字都在真实 home(`/home/speak/.xlings`)或它的忠实复现上实测,
> 命令写在每节的「实测」里,可复现。

> **For agentic workers:** REQUIRED SUB-SKILL: 用 `superpowers:subagent-driven-development`
> (推荐)或 `superpowers:executing-plans` 逐任务实施。步骤是 `- [ ]` checkbox。

**Goal:** 让 `index_repos` 的条目**真正平级**。每个条目的 `name` 同时决定它的目录、
命名空间**和它下载哪个索引**——三者同源,一个条目的目录在构造上不可能收到别的
索引的内容。

**Architecture:** 删掉"第 0 个条目是官方主索引"这个特权语义,以及支撑它的
C++ 硬编码 URL 判定。pointer 本来就是 `name → manifest` 的平级映射
(`IndexManifest.index_name`),`xim.index-base` 本来就是配置项——特权是多余的。
顶层条目和子索引走**同一个同步函数**。

**Tech Stack:** C++23 modules;gtest(`mcpp test`);bash e2e。

## Global Constraints

- 构建/测试只用 `mcpp build` / `mcpp test`,**不要**裸 xmake,**不要** `mcpp clean`。
- 工具链 `gcc@16.1.0`;链接报 glibc/musl 错时先 `xlings use gcc@16.1.0`。
- **模块实现单元里禁止 range adaptor 管道**(`views::split`、`transform | ranges::to`);
  `std::ranges::any_of` / `find_if` 安全(已在用)。
- 新增 e2e 必须注册进 `tests/e2e/run_all.sh`。
- commit 一律 `git commit -F -` + 带引号 heredoc(`-m` 里的反引号会被执行)。
- 断言只断言不变量,不断言索引版本号 / pointer revision / 快照条数。
- 基线:HEAD `ed699c9`,version `2026.8.27.5`。

---

## 0. 核心:错位是什么

`index_repos` 的条目**设计上是平级的**,每条都有 `name`。但同步器里藏着一个特权语义:

> **"数组第 0 个条目 = 官方主索引"**

这个语义既不由 `name` 表达,也不由配置表达。它由**数组下标**决定,由一段
**编译进 C++ 的 URL 子串**放行。错位就是这个特权落到了错误的条目上。

具体三处,`src/core/xim/repo.cpp`:

```cpp
// 449  谁是主索引 —— 认下标,不认 name
return Config::repo_dir_for(repos[0], false);            // → data/scode

// 490  是不是官方 —— 认编译进来的 URL 子串,不认配置
globalRepos.front().url.find("openxlings/xim-pkgindex") != npos
//  "…/openxlings/xim-pkgindex-scode.git" 命中,因为官方子索引全都以它为前缀

// 507  往哪写 和 写什么 —— 两个独立参数,没有任何东西要求它们同源
fetch_index_artifact(mainDir, ferr, {}, nullptr, mainPin);
//                   ↑目录来自 scode      ↑ {} 写死 = "官方 xim 索引"
```

**`name` 被用了——用在目录和命名空间上;没被用在"往这个目录里装什么"上。**
这就是"每个索引都有 name,为什么还会串"的答案。

串一次需要两件事同时成立,缺一不可:

| | 是什么 | 代码位置 |
|---|---|---|
| ① scode 成了 `repos[0]` | 主索引认下标 | `repo.cpp:449` |
| ② `repos[0]` 的目录被灌入 xim 的内容 | 目录与内容解耦 | `repo.cpp:507` |

`repo.cpp:490` 的 URL 子串是**③闸门**,放行了 ①②。它不是核心 —— 就算判定精确,
①② 的结构依然是错的。

### 0.1 铁证(真实 home,不用读代码)

```
$ cat ~/.xlings/data/scode/.xlings-index-version
667e9b8

$ xlings index
  ◆ scode   > 2026.8.27.5 … 2026.8.22.3        ← scode 的 pointer 不含 667e9b8
  ◆ xim     available: 667e9b8, 7da752b, …     ← xim 的 pointer 含 667e9b8
```

写进 scode 目录的版本号,只存在于 **xim** 的发布列表里。

附带一条:用户把 `xim` pin 到 `2026.8.27.2`,而盘上是 `667e9b8`(latest)。
因为 `mainPin = globalRepos.front().version`(`repo.cpp:506`)读的是 **scode** 的
version(空)。**那个 pin 从来没生效过** —— 同一个下标 bug 的另一面。

---

## 1. 为什么以前没有,现在有了

**不是使用错误。** 两个独立的变更,各自都不足以致命,叠在一起才出事。

### 1.1 `xlings index use xim <ver>` 把 xim 挤下了 0 号位

同一件事(默认条目放哪)有两个写者,方向相反:

```cpp
// config.cpp:514   读配置时,文件里没有 xim 就补在开头
globalIndexRepos_.insert(globalIndexRepos_.begin(), std::move(def));

// index_cmd.cpp:206  index use 物化默认条目时,追加到末尾
json["index_repos"].push_back(entry);
```

`cmd_index_use` 的注释写得很清楚它在干什么:

> "The source exists in the effective config but not in the file — it is a
> built-in default. Materialise the entry so the pin has somewhere to live."

**实测**(隔离 home,发布版 2026.8.27.4):

```
$ cat .xlings.json        # 只写了 myrepo,没有 xim
  "index_repos": [ { "name": "myrepo", "url": "<fixture>" } ]
$ xlings index
  ◆ xim                   ← Config 把默认补在开头,repos[0] = xim,一切正常
  ◆ myrepo

$ xlings index use xim latest
index 'xim' follows the newest compatible snapshot

$ cat .xlings.json        # xim 被 push_back 到末尾
  "index_repos": [ { "name": "myrepo", … }, { "name": "xim", … } ]
$ xlings index
  ◆ myrepo                ← repos[0] 永久变成 myrepo
  ◆ xim
```

用户配置里 `xim` 带着 `"version": "2026.8.27.2"` 躺在数组**最后** —— 那个 pin 就是
`xlings index use xim 2026.8.27.2` 写的,写的同时把它挤下了 0 号位。

`xlings config --index-repo <ns>:<url>`(`cli.cpp:905-921`)同样是 `push_back`。
所以用户的配置极可能是这样长出来的,**每一步都是文档里的命令**:

| 步骤 | 文件里的 `index_repos` | 生效顺序 | 状态 |
|---|---|---|---|
| `config --index-repo scode:…` | `[scode]` | `[xim, scode]` | 正常 |
| `config --index-repo dsh:…` | `[scode, dsh]` | `[xim, scode, dsh]` | 正常 |
| `index use xim 2026.8.27.2` | `[scode, dsh, xim]` | `[scode, dsh, xim]` | **错位** |

### 1.2 `44faadb`(0.4.62,#348)把 auto 门控改成无条件

```cpp
- attemptArtifact = mainIsOfficialRemote && (mainArtifactManaged || !mainHasIndex);
+ attemptArtifact = mainIsOfficialRemote;
```

改之前:`data/scode` 是 git 克隆(有 `pkgs/`、无 marker)→
`mainArtifactManaged=false`、`mainHasIndex=true` → **不取制品**。
下标 bug 和子串 bug 那时都已经在,但是**休眠的**。改之后无条件取,休眠的醒了。

> `repo.cppm` 的注释留下了痕迹:"auto **no longer** needs them"。

---

## 2. 修复方向:平级化

### 2.1 特权语义是多余的

三件事已经就位,只是没被用上:

| 已有的东西 | 在哪 | 说明 |
|---|---|---|
| pointer 是 `name → manifest` 的映射 | `IndexManifest.index_name` | 注释原文:`"xim" for the main index, else sub name` |
| 制品来源是配置项 | `Config::index_base()`(`xim.index-base`) | 已支持按 region 解析 |
| 每仓库可声明自己的制品源 | `IndexRepo::artifactBase` / `source`(#377) | 已实现 |

`xlings index` 的输出就是这个平级映射的直接证据:`xim` 和 `scode` 各有各的快照列表,
`dsh` 是 `no pointer entry for 'dsh'`。**它们本来就是平的。**

所以正确模型是:

> **一个条目 = 一个 name = 一个目录 = 一个命名空间 = 一个 pointer key。**
> 顶层条目和子索引没有区别,走同一个函数。

`repos[0]` 不再有任何语义;`is_official_index_url` 整个删掉。
"走不走制品"由**配置**回答:pointer 里有没有这个名字 + 这个条目的 `source`。

### 2.2 平级化之后各条目的行为

| 条目 | pointer key | 结果 |
|---|---|---|
| `xim` | `xim` | 官方主索引制品 → `data/xim-pkgindex` |
| `scode` | `scode` | **scode 自己的**制品 → `data/scode`(用户本来想要的) |
| `dsh` | `dsh` | pointer 无此条目 → git 回退 |
| 声明了 `artifact` 的自定义仓库 | `custom->key` | 自己的源 |

**错位在构造上消失**:目录来自 `name`,内容也来自 `name`。

### 2.3 一个必须堵上的洞:同名的 fork

只按 name 取 key,会让 `{"name":"awesome","url":"<我自己的 fork>"}` 拿到**官方**
awesome 的制品 —— 这是同一个错位换了个方向。

现有代码在子索引路径上**已经**处理了这件事(`repo.cpp:598-600`):

```cpp
bool subDefaultOfficial = luaIt != luaUrl.end() && luaIt->second == repo.url && …;
//                        名字在声明里     且      URL 也对得上
```

把这条规则提升为通用规则,并且**来源来自配置**:

> 一个条目走制品路径,当且仅当它的 `url` 等于该 `name` 的**声明来源**:
> - `xim` → `xim.index-repo`(配置项,`xself/install.cpp` 已在写)
> - 子索引 → 主索引 `xim-indexrepos.lua` 里声明的 URL
> - 或者它自己声明了 `artifact`(#377,那就是它自己的来源)
>
> 都对不上 → 没有制品,走 git。

`xim.index-repo` 目前**只被写、从来没被读**(`grep -rn '"index-repo"' src/`:
只有 `install.cpp` 三处写入,零处读取),而 `default_global_index_repos_`
(`config.cpp:79-85`)把 URL 硬编码在 C++ 里。**让它读配置,是"官方索引不该硬编码"
这句话的落地。**

### 2.4 顺序还需要修吗

平级化之后 `repos[0]` 没有语义,`push_back` 就不再致命。但仍有两件事要定:

1. `xim-indexrepos.lua` 从哪读?→ 名为 `xim` 的条目的目录,而它的目录
   (`repo_dir_for`)**无条件**是 `data/xim-pkgindex`。所以
   `main_repo_dir()` 是**常量**,不是查找。
2. 两个写者方向相反本身就是 bug,顺手对齐(`insert(begin())`),
   否则 `xlings index` 的列表顺序会莫名其妙地跳。

---

## 3. 任务

| 任务 | 内容 | 消灭 |
|---|---|---|
| T1 | `sync_one_repo()`:顶层与子索引走同一函数,key = `repo.name` | 错位② |
| T2 | 声明来源检查 + `xim.index-repo` 真的被读;删掉 URL 硬编码 | 闸门③ + fork 洞 |
| T3 | `main_repo_dir()` 变常量;`repos[0]` 无语义 | 错位① |
| T4 | 两个写者对齐(`index use` / `config --index-repo` 插到开头) | 触发器 |
| T5 | e2e:错位的回归 | — |

依赖:T1 → T2 → T3 → T4 → T5。

> **不在本方案里**:我在调查中另外实测到三个**同类但与本 bug 无因果**的缺陷
> (两个条目共用一个目录、子索引 URL stem 撞车、`{"name":"xpkgs"}` 删掉所有已安装
> 载荷)。它们是真的,其中一个会丢数据,但把它们混进来会让这个方案讲不清楚。
> 见 §6,单独立项。

---

### Task 1: 顶层条目与子索引走同一个同步函数

**Files:**
- Modify: `src/core/xim/repo.cppm`(声明 `sync_one_repo`)
- Modify: `src/core/xim/repo.cpp:454-660`(`sync_all_repos`)
- Modify: `src/core/xim/indexfetch.cpp:512-514`(label 中性化)
- Create: `tests/unit/test_index_peer_sync.cpp`

**Interfaces:**
- Produces: `std::string index_pointer_key(const IndexRepo& repo)` —— T2 会用。

- [ ] **Step 1: 写失败测试**

新建 `tests/unit/test_index_peer_sync.cpp`:

```cpp
// index_repos entries are peers. Each one's `name` decides its directory, its
// namespace AND which index gets downloaded into it -- the three must come
// from one place.
//
// They did not. `main_repo_dir()` picked index_repos[0], and the main artifact
// was fetched with a hardcoded pointer key, so the destination directory and
// the downloaded content were independent parameters. Measured on 2026.8.27.4:
// data/scode held 185 packages of the official xim index, and its
// .xlings-index-version named a version only xim's pointer publishes.
#include <gtest/gtest.h>

#include <string>

import std;
import xlings.core.config;
import xlings.core.xim.repo;

namespace xim = xlings::xim;

namespace {
xim::IndexRepo repo_(std::string name, std::string url) {
    xim::IndexRepo r;
    r.name = std::move(name);
    r.url  = std::move(url);
    return r;
}
}  // namespace

// The key an entry is fetched under is its own name -- never a constant, and
// never another entry's. `{}` used to mean "the official index" regardless of
// which directory it was being written into.
TEST(IndexPeerSync, PointerKeyIsTheRepoName) {
    EXPECT_EQ(xim::index_pointer_key(
        repo_("xim", "https://github.com/openxlings/xim-pkgindex.git")), "xim");
    EXPECT_EQ(xim::index_pointer_key(
        repo_("scode", "https://github.com/openxlings/xim-pkgindex-scode.git")),
        "scode");
    EXPECT_EQ(xim::index_pointer_key(
        repo_("dsh", "https://github.com/Sunrisepeak/dsh-index.git")), "dsh");
}

// A repo that declares its own artifact base (#377) is fetched under that
// source's key, which is also derived from the repo -- still one origin.
TEST(IndexPeerSync, DeclaredArtifactSourceKeepsItsOwnKey) {
    auto r = repo_("mine", "https://example.com/o/mine.git");
    r.artifactBase = "https://example.com/o/mine-index";
    EXPECT_EQ(xim::index_pointer_key(r), "mine");
}
```

- [ ] **Step 2: 跑测试,确认失败**

```bash
mcpp test 2>&1 | tail -20
```

- [ ] **Step 3: 声明**

`src/core/xim/repo.cppm`,加在 `sync_repo_with_artifact` 声明之后:

```cpp
// Which entry of a published pointer describes this repo's tree.
//
// It is the repo's own name, always. The main index used to be fetched with a
// hardcoded key while its destination came from index_repos[0], so the
// directory and the content were chosen independently and a sub-index listed
// first received the official index. One origin, one answer.
std::string index_pointer_key(const IndexRepo& repo);

// Sync ONE index repo: artifact if a pointer describes it and the repo's
// source mode allows, else git/local. Top-level index_repos entries and
// discovered sub-indexes are peers and both go through here -- the split into
// "the main index" and "the others" is what let the privilege land on the
// wrong entry.
bool sync_one_repo(const IndexRepo& repo,
                   const std::filesystem::path& repoDir,
                   const std::string& globalIndexSource,
                   const std::string& mirror,
                   bool projectScope,
                   bool force);
```

- [ ] **Step 4: 实现**

`src/core/xim/repo.cpp`,加在 `sync_repo_with_artifact` 之后:

```cpp
std::string index_pointer_key(const IndexRepo& repo) {
    if (auto src = artifact_source_for(repo)) return src->key;
    return repo.name;
}

bool sync_one_repo(const IndexRepo& repo,
                   const std::filesystem::path& repoDir,
                   const std::string& globalIndexSource,
                   const std::string& mirror,
                   bool projectScope,
                   bool force) {
    const std::string mode = repo.source.empty() ? globalIndexSource : repo.source;
    auto custom = artifact_source_for(repo);

    // Does a published pointer describe an index under this repo's name? That
    // question is answered by the CONFIGURED pointer (xim.index-base), not by
    // a URL pattern compiled into xlings. A name no pointer carries -- `dsh`,
    // a private index -- simply has no artifact and uses git.
    if (mode != "git" && artifact_is_declared_for(repo, projectScope)) {
        const auto key = index_pointer_key(repo);
        auto& pointers = load_index_pointers(mirror, custom ? &*custom : nullptr);
        if (select_manifest(pointers, key, custom.has_value())) {
            std::string ferr;
            if (fetch_index_artifact(repoDir, ferr, key,
                                     custom ? &*custom : nullptr, repo.version)) {
                return true;
            }
            if (mode == "artifact") {
                log::error("[index] '{}' artifact fetch failed and git fallback is "
                           "disabled (source=artifact): {}", repo.name, ferr);
                return false;
            }
            log::warn("[index] '{}' artifact fetch failed ({}); falling back to git",
                      repo.name, ferr);
        }
    }
    if (mode == "artifact") {
        log::error("[index] '{}' has no artifact source and source=artifact "
                   "disables git", repo.name);
        return false;
    }

    if (Config::is_local_repo_source(repo, projectScope)) {
        auto sourceDir = Config::resolve_repo_source(repo, projectScope);
        if (!detail_::ensure_local_repo_link_(repoDir, sourceDir)) {
            log::warn("index repo '{}' skipped: local source has no pkgs/ ({})",
                      repo.name, sourceDir.string());
            return false;
        }
        return true;
    }
    auto url = detail_::sync_repo_url_(repo.url, mirror);
    if (!sync_repo(repoDir, url, force)) {
        log::warn("index repo '{}' skipped: sync failed ({})", repo.name, url);
        return false;
    }
    return true;
}
```

> `artifact_is_declared_for` 由 T2 提供。**做 T2 之前**这里先写
> `(!repo.artifactBase.empty() || repo.name == Config::DEFAULT_INDEX_REPO_NAME)`
> 并留一行 `// T2 replaces this with the declared-source check`,
> 让 T1 能独立编译并跑通测试。

- [ ] **Step 5: `sync_all_repos` 改用它**

删掉第 500-516 行整个"主索引制品"特权块。把 `syncRepos` lambda(526-563)替换成:

```cpp
    auto syncRepos = [&](const std::vector<IndexRepo>& repos, bool projectScope) {
        auto rootDir = projectScope ? Config::project_data_dir() : Config::global_data_dir();
        if (rootDir.empty()) return true;
        fs::create_directories(rootDir);
        int total = 0, ok = 0;
        for (auto& repo : repos) {
            ++total;
            if (sync_one_repo(repo, Config::repo_dir_for(repo, projectScope),
                              indexSource, mirror, projectScope, force)) ++ok;
        }
        return total == 0 || ok > 0;
    };
```

子索引循环(595-626)也替换成:

```cpp
    for (auto& repo : allSubRepos) {
        auto repoDir = sub_repo_dir_for(repo);
        if (sync_one_repo(repo, repoDir, indexSource, mirror, false, force))
            syncedSubRepos.push_back(repo);
        else
            log::warn("failed to sync sub-index repo: {} ({})", repo.name, repo.url);
    }
```

`mainArtifactManaged` / `mainHasIndex` / `attemptArtifact` /
`mainIsOfficialRemote` / `main_should_attempt_artifact` /
`sub_should_attempt_artifact` 的调用点随之删除。
`mainDir` 只剩 `discover_sub_repos_` 一个用途,保留。

> `main_should_attempt_artifact` / `sub_should_attempt_artifact` 这两个导出函数
> 在 `tests/unit/test_core_basics.cpp` 里有决策表测试。它们的语义已经被
> `sync_one_repo` 取代 —— **删函数,连同它们的测试一起删**,并在 commit message
> 里说明。留着一个没有调用者的门控函数,下一次就会有人接着用它。

- [ ] **Step 6: label 中性化**

`src/core/xim/indexfetch.cpp:512-514`:

```cpp
    // Every repo is fetched under its own name now, so there is no "the index"
    // versus "a sub-index" -- there is just which one.
    std::string label = std::format("index '{}'", key);
```

- [ ] **Step 7: 跑测试**

```bash
mcpp build && mcpp test 2>&1 | grep -E "IndexPeerSync|XimRepoTest|FAILED|PASSED"
```

- [ ] **Step 8: 提交**

```bash
git add src/core/xim/repo.cppm src/core/xim/repo.cpp src/core/xim/indexfetch.cpp \
        tests/unit/test_index_peer_sync.cpp tests/unit/test_core_basics.cpp
git commit -F - <<'EOF'
fix(index): every index repo is fetched under its own name

The main index was fetched with a hardcoded pointer key into a directory
derived from index_repos[0], so the destination and the content were
independent parameters. A sub-index listed first therefore received the
official index: data/scode held 185 packages of xim and served every one of
them under the `scode` namespace.

Top-level entries and sub-indexes now go through one function and one key.
The main-vs-sub artifact gates are deleted rather than left callerless.
EOF
```

---

### Task 2: 声明来源检查 —— 删掉 C++ 里的官方 URL

**Files:**
- Modify: `src/core/config.cppm` / `config.cpp:79-85`(读 `xim.index-repo`)
- Modify: `src/core/xim/repo.cppm` / `repo.cpp`(`artifact_is_declared_for`)
- Modify: `src/core/xim/repo.cpp:487-491`(**删除** `mainIsOfficialRemote`)
- Modify: `tests/unit/test_index_peer_sync.cpp`

**Interfaces:**
- Produces: `bool artifact_is_declared_for(const IndexRepo& repo, bool projectScope)`;
  `Config::declared_index_repo_url(std::string_view name)`

- [ ] **Step 1: 写失败测试**

追加到 `tests/unit/test_index_peer_sync.cpp`:

```cpp
// Whether a repo may be fetched as an artifact is a CONFIGURATION question,
// not a pattern compiled into xlings. It used to be
// `url.find("openxlings/xim-pkgindex") != npos` -- and every official
// sub-index URL contains that as a prefix, which is what let a sub-index
// listed first be treated as the official index.
//
// The rule: the entry's url must equal the DECLARED source for its name.
TEST(IndexPeerSync, ArtifactNeedsTheDeclaredSourceForThatName) {
    // `xim` declared by config (xim.index-repo). Matching url -> artifact.
    EXPECT_TRUE(xim::url_matches_declared_source(
        "xim", "https://github.com/openxlings/xim-pkgindex.git",
        "https://github.com/openxlings/xim-pkgindex.git"));

    // Same name, a fork. Must NOT receive the official artifact -- that is the
    // crossing, pointing the other way.
    EXPECT_FALSE(xim::url_matches_declared_source(
        "xim", "https://github.com/me/my-xim-fork.git",
        "https://github.com/openxlings/xim-pkgindex.git"));

    // No declaration for this name at all (a private index): git only.
    EXPECT_FALSE(xim::url_matches_declared_source(
        "dsh", "https://github.com/Sunrisepeak/dsh-index.git", ""));

    // Cosmetic differences must not read as a different repo.
    EXPECT_TRUE(xim::url_matches_declared_source(
        "xim", "https://github.com/openxlings/xim-pkgindex",
        "https://github.com/openxlings/xim-pkgindex.git/"));
}
```

- [ ] **Step 2: 跑测试,确认失败**

- [ ] **Step 3: 让 `xim.index-repo` 真的被读**

`config.cpp:79-85` 替换成:

```cpp
std::vector<IndexRepo> Config::default_global_index_repos_(const std::string& mirror) {
    // The default index URL is CONFIGURATION -- xself/install.cpp has been
    // writing xim.index-repo (and xim.mirrors.index-repo) since it shipped, and
    // until now nothing read it: `grep -rn '"index-repo"' src/` found three
    // writes and zero reads. The literals below are the last-resort default for
    // a home whose config predates that, not the source of truth.
    std::string url = defaultIndexRepoUrl_;
    if (url.empty()) {
        url = mirror == "CN" ? "https://gitee.com/sunrisepeak/xim-pkgindex.git"
                             : "https://github.com/openxlings/xim-pkgindex.git";
    }
    return { IndexRepo{std::string(DEFAULT_INDEX_REPO_NAME), url} };
}
```

`config.cppm` 的 private 段加 `std::string defaultIndexRepoUrl_;`,public 段加:

```cpp
    // The URL declared for an index name: xim.index-repo for the default index,
    // empty for a name nothing declares. This is what "is this repo entitled to
    // the artifact published under its name" is decided against.
    [[nodiscard]] static std::string declared_index_repo_url(std::string_view name);
```

`config.cpp` 的加载处(第 487 行 `mirror` 之后)读入:

```cpp
                if (json.contains("xim") && json["xim"].is_object()) {
                    const auto& xim = json["xim"];
                    if (xim.contains("index-repo") && xim["index-repo"].is_string())
                        defaultIndexRepoUrl_ = xim["index-repo"].get<std::string>();
                    if (xim.contains("mirrors") && xim["mirrors"].is_object()) {
                        const auto& m = xim["mirrors"];
                        if (m.contains("index-repo") && m["index-repo"].is_object()) {
                            const auto& byRegion = m["index-repo"];
                            const auto key = mirror_.empty() ? std::string("GLOBAL") : mirror_;
                            if (byRegion.contains(key) && byRegion[key].is_string())
                                defaultIndexRepoUrl_ = byRegion[key].get<std::string>();
                        }
                    }
                }
```

> **顺序要紧**:这段必须在 `mirror_` 已经读到之后、
> `globalIndexRepos_ = parse_index_repos_json(...)` 之前。`mirror_` 在第 487-488 行
> 读入,`parse_index_repos_json` 在 494 行 —— 插在两者之间。

实现:

```cpp
[[nodiscard]] std::string Config::declared_index_repo_url(std::string_view name) {
    auto& self = instance_();
    if (name != DEFAULT_INDEX_REPO_NAME) return {};   // sub-indexes: see repo.cpp
    if (!self.defaultIndexRepoUrl_.empty()) return self.defaultIndexRepoUrl_;
    return default_global_index_repos_(self.mirror_).front().url;
}
```

- [ ] **Step 4: URL 归一化比较**

`src/core/xim/repo.cppm`:

```cpp
// Do these two URLs name the same repository? Compares after stripping a
// trailing `.git` and trailing slashes -- a config that omits `.git` is the
// same repo, and reading it as a different one would silently drop the entry
// to git.
bool url_matches_declared_source(std::string_view name,
                                 std::string_view repoUrl,
                                 std::string_view declaredUrl);

// May this repo be fetched as a versioned artifact? Configuration decides:
//   - it declares its own artifact base (#377), or
//   - its url equals the declared source for its name (xim.index-repo for the
//     default index; the main index's xim-indexrepos.lua for a sub-index).
// Nothing else. There is no URL pattern compiled into xlings any more.
bool artifact_is_declared_for(const IndexRepo& repo, bool projectScope);
```

`src/core/xim/repo.cpp`:

```cpp
namespace detail_ {
std::string normalize_repo_url_(std::string_view url) {
    std::string s{url};
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.ends_with(".git")) s.resize(s.size() - 4);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}
}  // namespace detail_

bool url_matches_declared_source(std::string_view name [[maybe_unused]],
                                 std::string_view repoUrl,
                                 std::string_view declaredUrl) {
    if (declaredUrl.empty() || repoUrl.empty()) return false;
    return detail_::normalize_repo_url_(repoUrl)
        == detail_::normalize_repo_url_(declaredUrl);
}

bool artifact_is_declared_for(const IndexRepo& repo, bool projectScope) {
    if (!repo.artifactBase.empty()) return true;
    if (Config::is_local_repo_source(repo, projectScope)) return false;
    auto declared = Config::declared_index_repo_url(repo.name);
    if (declared.empty()) declared = declared_sub_index_url(repo.name);
    return url_matches_declared_source(repo.name, repo.url, declared);
}
```

`declared_sub_index_url(name)` 读主索引的 `xim-indexrepos.lua`:

```cpp
// The URL the main index declares for a sub-index name. This is the same test
// `subDefaultOfficial` already made inline (repo.cpp) -- it was right, it just
// only applied to sub-indexes. Now it is the general rule.
std::string declared_sub_index_url(std::string_view name) {
    auto mirror = Config::mirror();
    for (auto& r : detail_::discover_sub_repos_(main_repo_dir(),
                                                mirror.empty() ? "GLOBAL" : mirror)) {
        if (r.name == name) return r.url;
    }
    return {};
}
```

- [ ] **Step 5: 删掉 URL 硬编码**

`src/core/xim/repo.cpp` 第 487-491 行(`mainIsOfficialRemote`)**整块删除**。
全仓库 `grep -rn 'openxlings/xim-pkgindex' src/` 只应剩 `README.md` 和
`config.cpp` / `install.cpp` 里的**最后兜底默认值**。

- [ ] **Step 6: 把 T1 Step 4 的临时条件换成真的**

`sync_one_repo` 里 `// T2 replaces this` 那行 → `artifact_is_declared_for(repo, projectScope)`。

- [ ] **Step 7: 跑测试 + 真机验证**

```bash
mcpp build && mcpp test 2>&1 | grep -E "IndexPeerSync|FAILED|PASSED"
grep -rn 'openxlings/xim-pkgindex' src/ --include=*.cpp --include=*.cppm
```

- [ ] **Step 8: 提交**

```bash
git add src/core/config.cppm src/core/config.cpp src/core/xim/repo.cppm \
        src/core/xim/repo.cpp tests/unit/test_index_peer_sync.cpp
git commit -F - <<'EOF'
fix(index): entitlement to an artifact comes from config, not a URL pattern

"Is this the official index" was `url.find("openxlings/xim-pkgindex")`, and
every official sub-index URL contains that as a prefix. It is now: does this
entry's url equal the source declared for its name -- xim.index-repo for the
default index, xim-indexrepos.lua for a sub-index.

xim.index-repo had three writers and zero readers since it shipped. It is
read now, so the default index URL is configuration.
EOF
```

---

### Task 3: `main_repo_dir()` 变常量,`repos[0]` 不再有语义

**Files:**
- Modify: `src/core/config.cppm:141-142`(常量移到 public)
- Modify: `src/core/xim/repo.cpp:446-452`
- Modify: `tests/unit/test_index_peer_sync.cpp`

- [ ] **Step 1: 常量移到 public**

`src/core/config.cppm`:第 141-142 行从 `private:` 剪切,粘到 `public:`(第 83 行)之后:

```cpp
    // Public because they ARE the public contract of repo_dir_for(): the
    // default entry is the one named DEFAULT_INDEX_REPO_NAME and it lives in
    // DEFAULT_INDEX_REPO_DIR, not in a directory named after itself.
    static constexpr std::string_view DEFAULT_INDEX_REPO_NAME = "xim";
    static constexpr std::string_view DEFAULT_INDEX_REPO_DIR  = "xim-pkgindex";
```

- [ ] **Step 2: 写失败测试**

```cpp
// Where xim-indexrepos.lua is read from is the LAST thing that still needs to
// know which repo is the default index, and it is not a search: the default
// entry is the one named DEFAULT_INDEX_REPO_NAME, and repo_dir_for maps that
// name onto DEFAULT_INDEX_REPO_DIR unconditionally. So no config a user writes
// can move it.
//
// It used to be repo_dir_for(index_repos[0]).
TEST(IndexPeerSync, MainRepoDirIsAConstantNotALookup) {
    auto expected = xlings::Config::global_data_dir()
                  / xlings::Config::DEFAULT_INDEX_REPO_DIR;
    EXPECT_EQ(xim::main_repo_dir(), expected);
}
```

- [ ] **Step 3: 实现**

`src/core/xim/repo.cpp` 第 446-452 行整体替换:

```cpp
std::filesystem::path main_repo_dir() {
    // Not a lookup -- a constant. The default index is the entry named
    // DEFAULT_INDEX_REPO_NAME (Config guarantees one exists, config.cpp), and
    // repo_dir_for maps that name onto DEFAULT_INDEX_REPO_DIR unconditionally.
    //
    // It used to be repo_dir_for(repos[0]). That is how `xlings index use xim
    // <ver>` -- which appends the materialised entry to the END of the array --
    // silently handed the default index's identity to whatever happened to be
    // first.
    //
    // Only sub-index discovery still asks: xim-indexrepos.lua ships in the
    // default index and nowhere else.
    return Config::global_data_dir() / Config::DEFAULT_INDEX_REPO_DIR;
}
```

- [ ] **Step 4: 跑测试 + 提交**

```bash
mcpp test 2>&1 | grep -E "IndexPeerSync|FAILED|PASSED"
git add src/core/config.cppm src/core/xim/repo.cpp tests/unit/test_index_peer_sync.cpp
git commit -F - <<'EOF'
fix(index): the default index's directory is a constant, not index_repos[0]

Position decided which repo shipped xim-indexrepos.lua and whose `version`
became the main pin. The user's pin on `xim` was read off `scode` (empty) and
never took effect, which is the same defect seen from the other side.
EOF
```

---

### Task 4: 两个写者对齐

**Files:**
- Modify: `src/core/xim/index_cmd.cpp:197-208`
- Modify: `src/cli.cpp:905-921`
- Create: `tests/e2e/index_repo_order_test.sh`(T5 一并写)

- [ ] **Step 1: `cmd_index_use` 插到开头**

`src/core/xim/index_cmd.cpp`,把第 206 行替换成:

```cpp
                // insert, not push_back. Config materialises a missing default
                // at begin() (config.cpp), so appending here made the two
                // writers of "where does the default entry go" disagree -- and
                // `xlings index use xim <ver>` silently moved the default index
                // out of position 0. Position no longer carries meaning, but
                // two writers with opposite answers is a defect either way, and
                // it still reorders what `xlings index` prints.
                json["index_repos"].insert(json["index_repos"].begin(), entry);
```

- [ ] **Step 2: `--index-repo` 保持追加,但加注释说明为什么这里可以**

`src/cli.cpp` 第 919 行之前:

```cpp
            // Appending is correct HERE: this is a repo the user is adding, not
            // a built-in default being materialised, so there is no other
            // writer with an opinion about where it goes. (index_cmd.cpp's
            // materialise path is the one that must match Config.)
```

- [ ] **Step 3: 构建 + 手动验证**

```bash
mcpp build
T=$(mktemp -d); mkdir -p "$T/subos/default/bin"
cp "$(ls -t target/*/*/bin/xlings | head -1)" "$T/xlings"
cat > "$T/.xlings.json" <<EOF
{ "mirror": "GLOBAL", "index_repos": [ { "name": "myrepo", "url": "$PWD/tests/fixtures/xim-pkgindex" } ] }
EOF
XLINGS_HOME="$T" "$T/xlings" index use xim latest
XLINGS_HOME="$T" "$T/xlings" index | grep "◆"     # xim 必须仍在第一行
```

- [ ] **Step 4: 提交**

```bash
git add src/core/xim/index_cmd.cpp src/cli.cpp
git commit -F - <<'EOF'
fix(index): materialising the default entry matches where Config puts it

Config inserts a missing default at begin(); `index use` appended it. So
pinning the default index moved it out of position 0 -- and position used to
decide which repo was the default index.
EOF
```

---

### Task 5: e2e —— 错位的回归

**Files:**
- Create: `tests/e2e/index_repo_order_test.sh`
- Modify: `tests/e2e/run_all.sh`

> **能证明什么**:顺序不再决定身份(子索引发现、目录归属)。
> **不能证明什么**:制品下载路径 —— 本地 fixture 是 `file://`,
> `is_local_repo_source` 为真,永远走不到 `fetch_index_artifact`。
> 那一半由 T1/T2 的纯函数单测覆盖。**这一条要写在脚本头部**,免得后来者
> 把这个 e2e 的绿色读成"制品路径被覆盖了"。

- [ ] **Step 1: 写测试**

```bash
#!/usr/bin/env bash
# E2E: the order of index_repos must not decide which repo is the default
# index.
#
# Reproduced on 2026.8.27.4: `xlings index use xim <ver>` materialised the
# default entry at the END of the array, index_repos[0] became a sub-index, and
# the official index artifact was downloaded into that sub-index's directory --
# 185 packages served twice, under two namespaces.
#
# What this canNOT prove: the artifact download itself. Local fixtures are
# file:// URLs and is_local_repo_source() short-circuits before any pointer is
# consulted. That half is tests/unit/test_index_peer_sync.cpp.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

HOME_DIR="$(runtime_home_dir index_repo_order_home)"
SUB_DIR="$ROOT_DIR/tests/fixtures/xim-pkgindex-ordersub"
errors=()

cleanup() {
  rm -rf "$HOME_DIR" "$SUB_DIR"
  rm -f "$FIXTURE_INDEX_DIR/xim-indexrepos.lua"
}
trap cleanup EXIT
cleanup

assert_home_is_isolated "$HOME_DIR"

# A sub-index with one package of its own, declared by the main fixture.
mkdir -p "$SUB_DIR/pkgs/o"
cat > "$SUB_DIR/pkgs/o/onlysub.lua" <<'LUAEOF'
package = {
    name = "onlysub",
    description = "Exists only in the sub-index",
    authors = "test",
    license = "MIT",
    repo = "https://example.com/onlysub",
}
LUAEOF
(cd "$SUB_DIR" && git init -q && git add -A && git commit -q -m init)

cat > "$FIXTURE_INDEX_DIR/xim-indexrepos.lua" <<LUAEOF
xim_indexrepos = {
    ["ordersub"] = {
        ["GLOBAL"] = "file://$SUB_DIR",
    }
}
LUAEOF

# The damaged shape: a non-default entry FIRST, the default entry last.
mkdir -p "$HOME_DIR/subos/default/bin"
cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "ordersub", "url": "file://$SUB_DIR" },
    { "name": "xim",      "url": "$FIXTURE_INDEX_DIR" }
  ]
}
EOF

run_xlings "$HOME_DIR" "$ROOT_DIR" update

# THE differential. Sub-index discovery reads xim-indexrepos.lua out of the
# DEFAULT index. With the default picked by position it was looked for in
# data/ordersub, which has no such file, so every declared sub-index silently
# disappeared. Assert the DIRECTORY, not xim-indexrepos.json: save_sub_repos_json
# writes `{}` when nothing was discovered, so the file exists either way -- that
# is exactly the assertion that would pass against the broken build.
[[ -d "$HOME_DIR/data/xim-index-repos/xim-pkgindex-ordersub/pkgs" ]] \
  || errors+=("declared sub-index was never discovered — default index picked by position?")

# The default index's own directory is its name's, not position 0's.
[[ -d "$HOME_DIR/data/xim-pkgindex/pkgs" ]] \
  || errors+=("the default index did not land in data/xim-pkgindex")

# No package of the default index is served under the other namespace.
SEARCH_OUT="$(run_xlings "$HOME_DIR" "$ROOT_DIR" search libpng 2>&1)"
echo "$SEARCH_OUT"
if echo "$SEARCH_OUT" | strip_ansi | grep -q "ordersub:libpng"; then
  errors+=("libpng is served under the ordersub namespace")
fi

# `index use` on the default entry must not move it.
run_xlings "$HOME_DIR" "$ROOT_DIR" index use xim latest >/dev/null 2>&1 || true
FIRST="$(run_xlings "$HOME_DIR" "$ROOT_DIR" index 2>&1 | strip_ansi \
           | grep -m1 "◆" | sed 's/.*◆ *//' | tr -d ' \r')"
[[ "$FIRST" == "xim" ]] \
  || errors+=("after 'index use xim', the first source is '$FIRST', expected 'xim'")

if [[ ${#errors[@]} -gt 0 ]]; then
  printf '%s\n' "${errors[@]}" >&2
  fail "${#errors[@]} ordering defect(s)"
fi
log "PASS: index_repos order does not decide the default index"
```

- [ ] **Step 2: 注册**

`tests/e2e/run_all.sh`,在 `"E2E-07b|install_refresh_on_missing_test.sh||"` 之后:

```bash
    "E2E-07c|index_repo_order_test.sh||"
```

- [ ] **Step 3: 单跑**

```bash
chmod +x tests/e2e/index_repo_order_test.sh
XB="$(ls -t target/*/*/bin/xlings | head -1)"
XLINGS_BIN="$PWD/${XB#./}" bash tests/e2e/index_repo_order_test.sh
```

> `XLINGS_BIN` 必须绝对路径。`find … | head -1` 按目录序挑,会稳定挑到旧构建。

- [ ] **Step 4: 确认修复前失败**

```bash
git stash && mcpp build
XB="$(ls -t target/*/*/bin/xlings | head -1)"
XLINGS_BIN="$PWD/${XB#./}" bash tests/e2e/index_repo_order_test.sh; echo "rc=$?"
git stash pop && mcpp build
```

预期 `rc != 0`,stderr 至少包含
`declared sub-index was never discovered` 和
`after 'index use xim', the first source is 'ordersub'`。
**只有一条失败就停下来查** —— 另一条没测到东西。

- [ ] **Step 5: 提交**

```bash
git add tests/e2e/index_repo_order_test.sh tests/e2e/run_all.sh
git commit -F - <<'EOF'
test(e2e): index_repos order must not decide the default index

Covers both halves of the regression: the default index keeps its identity
wherever it sits, and `index use` on it does not move it.
EOF
```

---

## 4. 覆盖矩阵

| 缺陷 | 变成不可能,还是被检测? | 挡住它的 |
|---|---|---|
| 目录与内容解耦(错位②) | **不可能**(key = name,同源) | `PointerKeyIsTheRepoName` |
| 下标决定主索引(错位①) | **不可能**(目录是常量) | `MainRepoDirIsAConstantNotALookup` + e2e |
| URL 子串硬编码(闸门③) | **删除** | `ArtifactNeedsTheDeclaredSourceForThatName` |
| 同名 fork 反向错位 | 检测(声明来源不匹配 → git) | 同上 |
| 两个写者方向相反 | 对齐 | e2e 最后一条 |

**没有被任何测试挡住的**:

1. **真实制品下载路径**。e2e fixture 全是本地路径,永远走不到
   `fetch_index_artifact`。"配置的 pointer → 按 name 取 → 写进哪个目录"
   这条链只有纯函数单测。真覆盖需要 HTTP fixture 服务器 + 假 pointer,
   独立立项。
2. **`declared_sub_index_url` 依赖主索引已在盘上**。首次同步时
   `main_repo_dir()` 可能还没有 `xim-indexrepos.lua`,子索引那一轮拿不到声明 →
   退回 git。**这是安全的方向**(退回而不是误取),但意味着首次安装时子索引
   走 git、第二次 `update` 才走制品。行为可接受,**没有测试**。
3. **`xim.index-repo` 从"只写不读"变成"被读"是行为变更**。老 home 里那个键是
   install 写的、和硬编码默认一致,所以正常情况下无差别;但被手工改过的 home
   会开始生效。见 §5。

---

## 5. 迁移与发布注意

1. **`xim.index-repo` 开始生效**。它自 install 起就在写,从未被读。手工改过这个
   键的 home,默认索引 URL 会从硬编码值变成他们写的值 —— 这正是它本该有的语义,
   但对他们是行为变更。发布说明要写。

2. **用户 `/home/speak/.xlings` 的处置**。T1-T3 之后:
   - `data/scode` 仍然是那份 xim 副本,`sync_repo` 的非破坏性护栏
     (`repo.cpp:208-228`)会 `return true` 而什么也不做 —— **代码修好不会自愈**。
     手工 `rm -rf ~/.xlings/data/scode` 一次即可;之后 `scode` 会正确地拿到
     scode 自己的制品。
   - `xim` 上的 `"version": "2026.8.27.2"` 是死 pin(pointer 不提供该版本)。
     **修复前它从未生效**(pin 读的是 `repos[0]` = scode 的空 version);
     T3 之后 `xim` 的 pin 才真的会被读到,制品取失败→ warn → 回退 git。
     用 `xlings index use xim latest` 清掉。

3. **第 2 点的后半段是本方案唯一一处"修好之后行为变差"**:一个一直被忽略的
   字段开始生效,用户什么都没改却看到新 warning。发布说明必须写。

---

## 6. 单独立项:目录与命名空间没有唯一性约束

调查过程中另外实测到三个缺陷。它们**和本 bug 没有因果关系** —— 不修本 bug 它们
也在,修了本 bug 它们还在 —— 但其中一个会丢数据,应当单独开 issue。

全部在隔离 home 上用发布版 `2026.8.27.4` 复现。

**A. 两个条目共用一个目录**。`repo_dir_for` = `data/` +
`(name=="xim" ? "xim-pkgindex" : name)`,所以 `{"name":"xim-pkgindex"}` 和
`{"name":"xim"}` 落到同一个目录:

```
$ xlings install libpng
[error] package 'libpng' is ambiguous, candidates:
        1. xim-pkgindex:libpng@1.6.43 from global repo 'xim-pkgindex'
        2. xim:libpng@1.6.43 from global repo 'xim'
```

**B. 子索引 URL 的 stem 撞车**。`sub_repo_dir_for` = `url_to_dirname(url)` =
`path(url).stem()`,`org1/idx.git` 和 `org2/idx.git` 都得到 `idx`:

```
$ xlings search only
  ◆ alpha:onlyb  from repo B
  ◆ beta:onlyb   from repo B          ← alpha 在提供 B 的包,A 的 onlya 消失了
```

**C. 保留目录名 —— 会丢数据**。`{"name":"xpkgs"}`:

```
$ ls data/xpkgs/     # before:mypkg
$ xlings update      # 退出码 0
$ ls data/xpkgs/     # after:LICENSE pkgs README.md template.lua …
```

所有已安装包的载荷被 git clone 覆盖。`{"name":"xim-index-repos"}` 同理,
子索引根目录连同 `xim-indexrepos.json` 一起没。

> 共同的根:目录名从用户可控字符串推导,两套规则(顶层看 name、子索引看 URL stem)
> 散在两处,都不检查唯一性,也不知道 `data/` 下哪些名字是别人的
> (`xpkgs` / `runtimedir` / `xim-index-repos` / `xim-pkgindex-local`)。
> 修法是一个纯函数把配置解析成无冲突布局,在碰文件系统**之前**拒绝。
> 那是另一个方案。

---

## 7. 落地结果(2026.8.30.1,PR #575)

### 7.1 实际改了什么

| 文件 | 改动 |
|---|---|
| `src/core/xim/repo.cpp/.cppm` | 新增 `index_pointer_key` / `url_matches_declared_source` / `declared_sub_index_url` / `artifact_is_declared_for` / `sync_one_repo`;**删除** `main_should_attempt_artifact`、`sub_should_attempt_artifact`、`sync_repo_with_artifact`、`mainIsOfficialRemote` 整块;`main_repo_dir()` 变常量 |
| `src/core/config.cpp/.cppm` | `DEFAULT_INDEX_REPO_NAME/_DIR` 转 public;新增 `resolve_default_index_repo_` + `declared_index_repo_url`;`xim.index-repo` **首次被读取** |
| `src/core/xim/index_cmd.cpp/.cppm` | `collect_index_sources` 改用 `artifact_is_declared_for`;`IndexSourceView` 加 `gitManaged`;`cmd_index_use` 物化改 `insert(begin())` |
| `src/core/xim/indexfetch.cpp` | 日志 label 中性化(不再有 "sub-index 'xim'") |
| `src/cli.cpp` | `--index-repo` 保持追加,加注释说明为何此处可以 |
| 测试 | 新增 `tests/unit/test_index_peer_sync.cpp`、`tests/e2e/index_repo_order_test.sh`;删除失去被测对象的两组决策表 |
| 文档 | `docs/spec/xlings-json-schema.md`(条目平级 + `xim` 段)、`docs/quick-start/custom-index.md` |

### 7.2 实测:用户故障场景

复现 home = 真实 home 的 `.xlings.json` + `data/{xim-pkgindex,scode,xim-index-repos}`。

```
初始(= 真实 home 的状态)
  data/scode/pkgs  185 个 .lua        marker 92660f5   ← 92660f5 是 xim 的版本
  $ 旧二进制 install mcpp-hooks-audioplayer
  [error] package 'mcpp-hooks-audioplayer' is ambiguous, candidates:
          1. scode:mcpp-hooks-audioplayer@0.0.1
          2. xim:mcpp-hooks-audioplayer@0.0.1

新二进制跑一次 xlings update(无需人工干预)
  data/scode/pkgs   15 个 .lua        marker 2026.8.27.5  ← scode 自己的版本
  data/xim-pkgindex 185 个 .lua
  $ search mcpp-hooks-audioplayer
    ◆ xim:mcpp-hooks-audioplayer          ← 一条
```

`data/scode` 现在符合取 **scode 自己**制品的条件,`fetch_index_artifact` 原子换整棵树
—— **无感自愈,不需要用户手工 `rm -rf`。**

抽查此前全部歧义的包:`cmake` 1 条;`perl` 1 条(另一条是描述含 "Perl" 的 `pcre2`);
`util-linux` 2 条 —— `scode:util-linux` 与 `xim:util-linux` 是**真的两个不同的包**
(scode 索引里确实有 util-linux),用命名空间区分,这是正确状态。

### 7.3 三处被实测推翻的判断

**① 「统一同步函数」不是纯重构 —— 它改了本地子索引的语义。**
`sync_one_repo` 一开始对本地源一律走 `ensure_local_repo_link_`(symlink),
而原来顶层是 symlink、**子索引是 git clone(快照)**。symlink 让索引跟着源实时变,
于是 C2 按需刷新(#366)永远没东西可刷、也就不再打印那句提示。
`install_refresh_on_missing_test.sh` 抓到了(旧二进制过、新的挂)。
已恢复原语义,并用 `linkLocalSource` 参数**显式标注这处既有的不对称**,
而不是顺手把它改掉 —— 那是另一个决定。

**② `default_global_index_repos_` 不能读单例。**
它在 Config **自身初始化过程中**被调用,`instance_()` 造成
`__gnu_cxx::recursive_init_error`,19 个测试二进制同时挂,而报错不指向任何有用的东西。
改成把 `declaredUrl` 传参进去。

**③ 「默认索引先同步」是必要的,不是洁癖。**
`artifact_is_declared_for` 要读默认索引的 `xim-indexrepos.lua`。首次运行时若先判定
其他条目,文件还不在盘上 → 拿不到声明 → 静默退回 git 一整轮。
`syncRepos` 现在把名为 `xim` 的条目排到最前(其余保持配置顺序)。
这不是恢复"位置有语义",而是"声明者先于被声明者"。

### 7.4 验证矩阵

| 层 | 内容 | 结果 |
|---|---|---|
| 单测 | 49/49 | 通过 |
| 单测(新增) | `IndexPeerSync.*` —— pointer key / 声明来源匹配 / 制品资格 / 默认索引目录常量性 | 通过 |
| e2e(新增) | `index_repo_order_test.sh` 两条差分 | 新构建过;**发布版 2026.8.27.4 两条都失败** |
| e2e(回归) | index_repo_order / install_refresh_on_missing / sub_index_search / sub_index_install / index_cache / install_subindex_first_run / local_query_no_index_sync / legacy_config / project_e2e / mirror_fallback | 10/10 通过 |
| 真机 | 用户故障场景在忠实复现 home 上消失 + 自愈 | 见 §7.2 |

### 7.5 仍然没有被挡住的

1. **真实制品下载路径的端到端覆盖**。e2e fixture 全是本地路径,
   `is_local_repo_source` 在看 pointer 之前就短路。"配置的 pointer → 按 name 取 →
   写进哪个目录"这条链只有纯函数单测 + §7.2 的真机验证,没有自动化 e2e。
   要补需要 HTTP fixture 服务器 + 假 pointer,独立立项。
2. **`declared_sub_index_url` 依赖默认索引已在盘上**。§7.3③ 的排序把首次运行的窗口
   关上了,但如果默认索引本身同步失败,后续条目会退回 git —— 安全方向,无测试。
3. **§6 的三个目录唯一性缺陷**仍然存在,与本 bug 无因果,单独立项。
