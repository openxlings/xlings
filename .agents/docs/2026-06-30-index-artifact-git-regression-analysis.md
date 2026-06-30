# 索引从 artifact 退化回 git 且无法自愈 — 根因分析与修复方案

**日期**: 2026-06-30
**状态**: Bug confirmed（本机 reflog + 残留临时目录实测复盘，见 §2）；**fix 已实现于 0.4.62**（P0-1/P0-2/P1，见 §4 与各代码注释）
**关联代码**: `src/core/xim/repo.cppm`(主迁移闸 `main_should_attempt_artifact`、sync_repo 非破坏守卫、`reconcile_index_temps` 调用)、`src/core/xim/indexfetch.cppm`(`reconcile_index_temps`、原子交换)、`src/platform/{unix,windows}.cppm`(`atomic_swap_paths` / renameat2 RENAME_EXCHANGE)
**影响版本**: 0.4.52 ~ 0.4.61（index-as-resource 引入后全程存在），**0.4.62 修复**
**发现场景**: 本机此前主索引 `xim-pkgindex` 已是 artifact-managed（有 `.xlings-index-version`、无 `.git`）；某次 `xlings update` 后退回 git（有 `.git`、`pkgs/`、无 marker），且后续每次 `xlings update` 都打印 `updating index repo:`（git pull），再也回不到 artifact。

---

## 1. 现象

`xlings update xlings -y --verbose` 输出：

```
[debug] updating index repo: xim-pkgindex
[debug] updating index repo: scode
[debug] updating index repo: xim-pkgindex-awesome
...
```

7 个索引仓全部走 git pull。检查磁盘：

```
~/.xlings/data/xim-pkgindex/.git           ← 存在
~/.xlings/data/xim-pkgindex/pkgs           ← 存在
~/.xlings/data/xim-pkgindex/.xlings-index-version  ← 不存在（关键）
```

远端 artifact **正常已发布**（本机 6/26 还成功拉过 `xim-pkgindex.artifact.*`），所以**不是 publish 缺失**，是本地状态退化且无法自愈。

---

## 2. 证据 / 复盘时间线

`~/.xlings/data/xim-pkgindex` 的 git reflog：

```
d9c219e HEAD@{2026-06-27 20:33:35}: clone: from https://gitee.com/sunrisepeak/xim-pkgindex.git   ← 退回 git 的时刻
83e3ff1 HEAD@{2026-06-29 11:58:53}: pull --ff-only
e33ddd6 HEAD@{2026-06-29 17:26:26}: pull --ff-only
5e4f263 HEAD@{2026-06-29 19:10:58}: pull --ff-only
106bfc9 HEAD@{2026-06-29 22:53:27}: pull --ff-only
```

父目录残留的 artifact 临时目录（`fetch_index_artifact` 的 `tmpRoot`，本应被 RAII `Cleanup` 删除）：

```
xim-pkgindex.artifact.1445862 (06-22, 内含 xim-index-latest.json 半成品)
... 共 8 个 06-22 的
xim-pkgindex.artifact.2296504 (06-26 22:14, 空)
xim-pkgindex.artifact.2304876 (06-26 22:19, 空)
```

**解读**：
- 6/22、6/26 `fetch_index_artifact` 被反复调用且**进程异常退出**（被 kill / 崩溃），所以 `Cleanup` 析构没执行，`tmpRoot` 泄漏；空目录说明失败发生在很早期（下载工件之前）。
- 6/27 20:33 发生 `git clone` —— 这要求当时 `mainArtifactManaged == false`（即 marker 已不存在），否则 `repo.cppm:487` 的守卫会 `continue` 跳过 git。
- 一旦变 git，之后只有 `pull --ff-only`，**永不回 artifact**。

即：**marker 在某次异常的 artifact 流程中丢失 → 下一次 update 走 git 回退并 `remove_all` 重建 → 永久卡死在 git。**

---

## 3. 根因（三个互相叠加的缺陷）

### 缺陷 A —— 主索引没有「迁移闸」，丢了 marker 就回不去（核心）

`repo.cppm:462-465` 主索引 auto 模式判定：

```cpp
else attemptArtifact = mainIsOfficialRemote && (mainArtifactManaged || !mainHasIndex);
```

- `mainArtifactManaged` = 有 `.xlings-index-version`
- `mainHasIndex` = 有 `pkgs/`

**一个存量 git 主索引（有 pkgs、无 marker）→ `true && (false || false)` = false → 永不切 artifact。**

对比子索引 `sub_should_attempt_artifact()`（`repo.cppm:201-209`），C1 迁移闸（#342 commit `c248f09`）给子索引加了 `|| mainArtifactManaged`：

```cpp
return isDefaultOfficial && (subManaged || !subHasPkgs || mainArtifactManaged);
```

**子索引能在「主已 artifact」时迁移，但主索引自己没有任何迁移触发器。** C1 闸只做了一半。后果：
- 主一旦掉回 git → 主永不回 artifact；
- 子索引迁移又依赖 `mainArtifactManaged` → 主卡 git → 子也永远卡 git。
- 这正是 `.agents/docs/2026-06-25-index-ecosystem-unification-plan.md:84` 预期「主 artifact-managed」被打破后，整套连锁退化、且**单向不可逆**的原因。

### 缺陷 B —— git 回退是破坏性的，会抹掉 marker

`sync_repo()` 的 clone 分支（`repo.cppm:235,261-279`）在目标无 `.git` 时：

```cpp
auto clone = compact::git::clone_shallow(urls[i], tmpDir, false);
if (clone.rc == 0) {
    fs::remove_all(localDir, ec);   // :265 —— 连同 .xlings-index-version 一起删掉
    fs::rename(tmpDir, localDir, ec);
    ...
```

任何让 `mainArtifactManaged` 瞬时为 false（marker 缺失或目录被移走）、同时 artifact fetch 又失败的事件，都会触发这条 `remove_all` + clone，把 artifact 索引**就地变成 git 索引**。叠加缺陷 A → 永久。

可能的瞬时触发源：
1. artifact 原子交换被中断（见缺陷 C）——目录被 rename 走，marker 暂时不在原位；
2. `xlings update xlings` 自更新期间，旧版本二进制先跑了一遍 git 路径；
3. artifact fetch 在 marker 不在位的窗口内失败回退。

### 缺陷 C —— 原子交换有「目标缺失」窗口 + 临时目录泄漏

`fetch_index_artifact()`（`indexfetch.cppm:371-386`）：

```cpp
auto backup = destIndexDir + ".old." + pid;
if (fs::exists(destIndexDir)) {
    fs::rename(destIndexDir, backup, ec);   // :375  把（带 marker 的）旧目录移走
}
fs::rename(stage, destIndexDir, ec);        // :378  新目录就位
...
fs::remove_all(backup, ec);                 // :386  仅成功路径删 backup
```

- `:375` 与 `:378` 之间若进程被 kill → `destIndexDir` **整个不存在**了（被移到 `.old.<pid>`），而 RAII `Cleanup`（`indexfetch.cppm:345`）**只删 `tmpRoot`，不管 `backup`** → 唯一带 marker 的副本变成孤儿 `.old.<pid>`，下次 update 看到「主目录不存在」→ `!mainHasIndex` → 再 attempt artifact，若又失败 → git clone（缺陷 B）。
- 本机 10+ 个泄漏的 `.artifact.<pid>` 证明异常退出确实在发生，且**没有任何启动期清理**来回收 `.artifact.*` / `.old.*` / `.tmp.*`。

---

## 4. 修复方案（按优先级）

### P0-1：主索引迁移闸对称化（修缺陷 A）

`repo.cppm:465` 改为：official remote 在 auto 下**始终**尝试 artifact，git 仅作回退：

```cpp
// auto: 官方远端索引始终收敛到 artifact；git 是回退而非终点。
// 本地(file://)/fork/自定义 URL 已被 mainIsOfficialRemote 排除，e2e fixture 不受影响。
else attemptArtifact = mainIsOfficialRemote;
```

`mainArtifactManaged` / `!mainHasIndex` 两个子条件对 official remote 变冗余：存量 git 主索引下次 update 即尝试 artifact，成功则 `fetch_index_artifact` 自带的原子交换把 git 目录换成 artifact 目录（带 marker、无 `.git`），**自愈**。失败则回退（配合 P0-2 非破坏性回退）。

> 这才是 C1 迁移闸的完整形态——plan 里只给子索引加了 `|| mainArtifactManaged`，遗漏了主索引本身。

> **契约**：`auto` 下官方远端索引收敛到 artifact；**要固定 git 检出可显式声明 `XLINGS_INDEX_SOURCE=git`**。CI 不需要额外声明——实测 e2e 在 `auto` 下照常通过:钉定的 git fixture 在 artifact 不可达时被 P0-2 守卫无损保留(`.git` 在则走 git pull,非破坏),不会被替换。曾尝试给所有 workflow env 加 `XLINGS_INDEX_SOURCE=git`,反而干扰了 mcpp 自身的索引解析(`index entry not found in local clone`)导致 e2e 失败,已回退——env 变量会泄漏进 mcpp 构建环境,不应全局设置。

### P0-2：official 索引的 git 回退非破坏性（修缺陷 B）

当 `attemptArtifact` 为真但 artifact fetch 失败、且本地已有可用 `pkgs/` 时，**不要**用 git clone 去 `remove_all` 覆盖它——保留现有副本，下次再试 artifact。

最小改动：扩展 `repo.cppm:487` 的主索引跳过条件——

```cpp
// 既已尝试 artifact 且本地有可用索引，则不让 git 回退把它 remove_all 重建。
bool mainTriedArtifact = !projectScope && repoDir == mainDir && attemptArtifact;
if (!projectScope && repoDir == mainDir && (mainArtifactManaged || (mainTriedArtifact && mainHasIndex)))
    continue;
```

并在 `sync_repo()` clone 分支（`:265`）加防御：若 `localDir` 含 `pkgs/` 但无 `.git`（疑似 artifact 目录），降级为「保留现有、记 WARN」而非 `remove_all`，除非显式 `force` 或 `XLINGS_INDEX_SOURCE=git`。

### P1：原子交换抗中断 + 启动期清理(修缺陷 C)

1. **关窗口（Linux）**：用 `renameat2(RENAME_EXCHANGE)` 让 `stage` 与 `destIndexDir` 一次原子互换，消除「目标缺失」窗口；非 Linux 保留两步 rename 但把 backup 纳入 RAII（失败时 `rename(backup → dest)` 回滚，而非泄漏）。
2. **启动清理**：`sync_all_repos` 入口扫一遍 `data/` 下 `*.artifact.*` / `*.old.*` / `*.tmp.*`，删除超过 N 小时（如 6h）的陈旧目录，回收异常退出的泄漏。
3. **marker 兜底恢复**：若 `destIndexDir` 缺失但存在对应 `.old.<pid>`（含 `pkgs/` 与 marker），优先 rename 回原位再继续。

### P2：可观测性

artifact→git 回退、尤其即将对 official 索引执行破坏性重建时，打 `log::warn` 明确说明「索引将从 artifact 退回 git，原因: …」，便于将来定位（当前 `:265` 的破坏性 clone 在 debug 级别静默）。

---

## 5. 用户立即恢复（无需等修复）

artifact 路径自带原子交换，可直接覆盖现有 git 目录，**无需手动删**：

```fish
XLINGS_INDEX_SOURCE=artifact xlings update xlings -y --verbose
```

- `indexfetch.cppm` 不受 7 天节流影响，必定重新 fetch；
- `XLINGS_INDEX_SOURCE=artifact` 同时强制主索引（`repo.cppm:463`）与默认子索引（`:205`）走 artifact；
- 失败会直接报错（`repo.cppm:471-474`),正好可验证远端 publish 是否健康。

完成后验证：

```fish
test -f ~/.xlings/data/xim-pkgindex/.xlings-index-version; and echo OK   # 应有
test -d ~/.xlings/data/xim-pkgindex/.git; and echo "still git(BAD)"      # 应无
```

顺手清理历史泄漏：

```fish
rm -rf ~/.xlings/data/xim-pkgindex.artifact.* ~/.xlings/data/xim-pkgindex.old.* ~/.xlings/data/*.tmp.*
```

---

## 6. 测试覆盖（修复需补的 e2e）

1. **主索引迁移**:存量 git 官方主索引（有 pkgs、无 marker）→ 一次 `xlings update`(auto)→ 变 artifact-managed(有 marker、无 `.git`),子索引随之迁移。
2. **回退非破坏**:artifact 不可达(mock 404/断网) + 本地已有索引 → update 后**保留**现有副本,不出现 `remove_all` + clone;marker 不丢。
3. **中断抗性**:在 `fetch_index_artifact` 交换窗口注入失败 → 主目录不丢失,下次 update 自愈。
4. **fixture 不回归**:本地(file://)/fork URL 主索引在 `attemptArtifact = mainIsOfficialRemote` 下仍走 git(因 `mainIsOfficialRemote` 已排除)。
