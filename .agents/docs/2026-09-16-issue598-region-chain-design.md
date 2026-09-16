# #598 — 区域声明是「偏好顺序」，不是「单选」

日期: 2026-09-16 ／ 目标版本: 2026.9.16.1

## 一句话

配置里每一处 `{"GLOBAL": ..., "CN": ...}` 今天都被读成「按 mirror 选一个」，
读完另一个就不存在了。官方索引不是这样的：`index_asset_urls` /
`index_pointer_urls` 对官方索引是**区域有序 + GLOBAL 兜底**。本轮把这条口径
统一：**区域对象是一条有序的候选链，mirror 决定顺序，不决定集合。**

## 现象（issue #598 实测，2026-09-16）

mcpp 2026.9.16.1 给 mcpplibs 索引写入：

```json
{ "name": "mcpplibs", "url": "https://github.com/mcpplibs/mcpp-index.git",
  "artifact": { "GLOBAL": "https://github.com/xlings-res/mcpp-index",
                "CN":     "https://gitcode.com/xlings-res/mcpp-index" } }
```

`mirror=CN` 时 `raw.gitcode.com` 偶发 403（华为 WAF 限流），客户端**不试
GitHub**，直接回落 git clone GitHub —— 正好是设 CN 镜像要避开的那条连接。

```
[warn] [index] pointer fetch failed: HTTP 403
[warn] [index] 'mcpplibs' artifact fetch failed (no pointer available at the
       declared source (offline, or the base is wrong)); falling back to its git/local source
```

## 根因（读于 4ea4eac）

同一个「按 mirror 选一个」的写法在配置层出现了三次，各自实现：

| 位置 | 读的键 | 现状 |
|------|--------|------|
| `parse_index_repos_json` (config.cpp:41) | `index_repos[].artifact` | 选一个 → `IndexRepo::artifactBase` |
| `Config::resolve_index_base_` (config.cpp:127) | `xim.index-base` | 选一个 → `indexBase_` |
| `Config::resolve_default_index_repo_` (config.cpp:141) | `xim.mirrors.index-repo` | 选一个 → git remote |

前两个的下游都是**无状态 HTTP 取文件**，完全可以按顺序多试几个；第三个的下游
是 `git clone`（一个本地克隆只有一个 origin），链式没有意义。

下游又把「一个 base」焊死了两处：

- `artifact_source_for()` → `ArtifactSource{ base, ... }`，
  `index_pointer_urls(custom)` 注释写着 “one authoritative location, no
  official mirrors”，返回**恰好一个** URL；
- `base_override_for_()` 对 flat base 设 `BaseOverride::base`，
  `obtain_file` 一旦看到它就丢掉传进来的候选 URL 列表。

另有一个同族的静默损失：`load_sub_repos_json` 只认 `artifact` 的字符串形态，
`save_sub_repos_json` 写回时也只写字符串 —— 一个区域对象经过一次
load→save 往返就**永久塌成单区域**。

## 设计

### D1 一个回答者：`parse_region_chain`

```cpp
export struct ArtifactBase { std::string region; std::string url; };
export std::vector<ArtifactBase> parse_region_chain(const nlohmann::json& value,
                                                    const std::string& mirror);
```

- 字符串 → 一条 `{region:"", url}`；
- 对象 → 顺序为 **当前 mirror 区域 → GLOBAL → 其余按声明顺序**；
- 去空、去尾斜杠、去重（同一 url 出现在两个区域只保留第一次）。

`resolve_index_base_` 与 `parse_index_repos_json` 都改用它。
`resolve_default_index_repo_` 也用它，但只取 `front()`（git remote 不链式，
理由见上；这一条写进注释，免得下一个人以为是漏掉了）。

### D2 `IndexRepo` 只保留一个字段

```cpp
std::vector<ArtifactBase> artifactBases;   // 偏好顺序，首个即原 artifactBase
std::string artifact_base() const;          // = artifactBases.front().url
```

删掉 `artifactBase` 这个拼写。两个拼写共存正是本仓库反复付学费的形态
（见 memory: one-question-many-answerers）。

### D3 `ArtifactSource` 带着整条链

`artifact_source_for(repo)` 仍返回**首选** base 派生出的 source（身份：
`key` / `repoName` / `base` 不变），新增 `altBases`（其余 base，有序）。
新增 `artifact_source_from_base(base, key)` 供 URL 构造与测试使用。

`index_pointer_urls(custom)` / `index_asset_urls(custom)`：对链上**每个远端
base**（forge 或 flat）按序产出 URL。forge 用各自的 `server`/`repoName`
推导，pointer 文件名沿用首选 base 的名字（互为镜像的仓库同名；不同名的情况
首选仍然工作，与今天一致）。本地 base 不出现在 URL 里 —— 它们走 D4。

### D4 `BaseOverride` 变成有序条目

```cpp
struct BaseOverride {
    struct Entry { std::string base; std::optional<fs::path> local; };
    std::vector<Entry> entries;   // 有序
    bool allowRemoteUrls = false; // entries 用尽后是否再试调用方给的 URL 列表
};
```

`obtain_file` **严格按 entries 顺序**逐个尝试（本地 → 拷贝，远端 → 下载），
全败后按 `allowRemoteUrls` 决定是否再走候选 URL 列表。

- `resolve_base_()`（`xim.index-base` / `XLINGS_INDEX_BASE_URL`）：
  entries = 整条链，`allowRemoteUrls=false`（override 就是 override，
  不偷偷回到官方服务器）。这条因此**也**获得了区域回落。
- `base_override_for_(custom)`：entries = 链上的**本地** base（有序），
  `allowRemoteUrls=true`；远端 base 由 D3 的 URL 列表覆盖。
  代价：链里同时混本地和远端时，本地总是先试 —— 一行注释说明，
  区域对象里写本地路径本来就不是一个真实配置。

### D5 往返不丢

`load_sub_repos_json` 认对象形态的 `artifact`；`save_sub_repos_json` 在链
长度 > 1 时写回区域对象（`ArtifactBase::region` 正是为此保留的）。

### D6 说出来

- pointer 取失败时，warn 带上**试过几个 base**；
- `xlings index status --json` / `interface list_repos` 暴露 `artifact_bases`
  （区域 + url），用户能看出「接下来会按什么顺序试」。

## 不做

- git remote 链式回落（`xim.mirrors.index-repo`、`xim-indexrepos.lua` 的
  GLOBAL/CN）。一个本地克隆只有一个 origin，链式要改写 origin，是另一件事。
- 跨 base 的自适应重排。override / 区域顺序是**用户声明的偏好**，
  不该被延迟探测推翻；官方索引那条路径的重排行为不变。

## 验收

1. 单测：链解析顺序、URL 构造（pointer/asset 各区域有序）、往返不丢。
2. e2e（hermetic，custom_index_artifact_test.sh 新增 F/G）：
   - F：区域对象，首选 base 坏、次选 base 好 → **从 artifact 装上**，不回落 git；
   - G：`xim.index-base` 区域对象，首选坏 → 官方索引仍从次选装上。
3. 真机：`mirror=CN`，`xlings update` 在 raw.gitcode.com 403 时命中 GitHub raw，
   不出现 git clone。
