# Issue #381 — 同仓库同名包的 namespace 索引身份设计

**日期**: 2026-07-25
**状态**: Released and verified（libxpkg 0.0.46 / xlings 0.4.69）
**Issue**: [openxlings/xlings#381](https://github.com/openxlings/xlings/issues/381)
**影响版本**: xlings 0.4.68 + openxlings/libxpkg 0.0.45
**涉及仓库**:

- `openxlings/libxpkg`
  - `src/xpkg.cppm`
  - `src/xpkg-loader.cppm`
  - `src/xpkg-index.cppm`
- `openxlings/xlings`
  - `src/core/xim/index.cppm`
  - `src/core/xim/catalog.cppm`
  - `src/core/xim/resolver.cppm`
  - `tests/unit/test_main.cpp`
  - `tests/e2e/index_cache_test.sh`

---

## 0. 结论先行

Issue #381 是真实且稳定可复现的身份模型缺陷，不只是查询时少做了一次 namespace
过滤。

当前 `libxpkg::build_index()` 在扫描 descriptor 时，以裸 `package.name` 写入
`PackageIndex::entries`。同一个 index repo 内的 `alpha:demo` 和 `beta:demo` 都写到
`entries["demo"]`，后写入者覆盖先写入者。xlings catalog 随后才加载 descriptor 并检查
namespace；此时另一个 descriptor 的路径已经从索引和缓存中消失，catalog 无法恢复。

推荐进行跨仓库结构性修复：

1. **libxpkg 将 `(effective namespace, package.name)` 作为完整包身份**；
2. **PackageIndex 同时维护完整身份表和短名候选表**；
3. **xlings catalog 允许一个 repo 返回多个短名候选**；
4. 显式 `namespace:name` 精确解析，裸 `name` 多候选时报现有 ambiguity 错误；
5. xlings index cache 升级为 v2，强制废弃缺少 namespace 信息的 v1 cache；
6. 同一完整身份重复时构建失败并报告两个 descriptor 路径，彻底取消扫描顺序决定胜者。

只在 xlings 侧加过滤无法修复本问题，因为输入 xlings catalog 之前信息已经丢失。

---

## 1. 现状验证

### 1.1 隔离复现

使用当前仓库构建的 xlings 0.4.68，在独立 `XLINGS_HOME` 中建立单一 index repo：

```text
pkgs/a/alpha.demo.lua  -> namespace = "alpha", name = "demo"
pkgs/b/beta.demo.lua   -> namespace = "beta",  name = "demo"
```

实测结果：

```text
xlings search demo
  alpha:demo

xlings info alpha:demo
  success

xlings info beta:demo
  [error] package 'beta:demo' not found
```

该隔离环境生成的 `.xlings-index-cache.json` 也只含一个条目：

```json
{
  "version": 1,
  "entries": {
    "demo": {
      "name": "demo",
      "path": ".../pkgs/a/alpha.demo.lua"
    }
  }
}
```

这直接证明丢失发生在索引构建阶段，而不是 CLI 展示阶段。

### 1.2 不受影响的场景

两个同名包位于不同 index repo 时，每个 repo 有独立 `PackageIndex`，不会在构建时共享
key space。catalog 能聚合两个候选并报告 ambiguity；这个行为正确，应保持不变。

### 1.3 当前根因链

```text
descriptor:
  namespace=alpha, name=demo ─┐
                              ├─ build_index -> entries["demo"] -> 只剩一个路径
  namespace=beta,  name=demo ─┘
                                                │
                                                v
                              xlings cache v1 同样只保存一个条目
                                                │
                                                v
                     catalog 先按 "demo" 查找，再加载 descriptor
                                                │
                                                v
                     namespace 仅作为后置过滤，无法找回 beta
```

具体代码：

- `openxlings/libxpkg/src/xpkg-loader.cppm`
  - `build_index(repo_dir, namespace_)`
  - key 使用函数参数 `namespace_ + pkg.name`
  - xlings 调用时不传该参数，因此实际 key 为 `pkg.name`
  - `index.entries[key] = ...` 对重复 key 直接覆盖
- `xlings/src/core/xim/index.cppm`
  - `IndexManager::rebuild()` 调用 `xpkg::build_index(repoDir_)`
  - cache v1 只保存 entry key/name/path，不保存 descriptor namespace
- `xlings/src/core/xim/catalog.cppm`
  - `build_match_()` 先以 `parsed.name` 调 `resolve/find_entry/match_version`
  - 找到单个 entry 后才加载 package 并检查 `pkg.namespace_`
  - `build_match_()` 的返回类型还是单个 `PackageMatch`，无法表达同仓库多候选

---

## 2. 需求与不变量

### 2.1 功能需求

1. 同一 index repo 可以同时保存 `alpha:demo` 与 `beta:demo`。
2. `xlings info/install alpha:demo` 只能解析到 alpha descriptor。
3. `xlings info/install beta:demo` 只能解析到 beta descriptor。
4. 裸 `demo`：
   - 只有一个候选时保持现有便捷解析；
   - 有多个 namespace 候选时明确报 ambiguity，并列出所有候选。
5. `xlings search demo` 必须列出两个包。
6. install、info、search、remove、plan/interface 和递归依赖解析使用同一身份规则。
7. 跨 repo 的既有候选聚合、project scope 优先级和 sub-index 优先级保持不变。

### 2.2 身份不变量

定义：

```text
effectiveNamespace =
    package.namespace 非空 ? package.namespace : repo.defaultNamespace

PackageIdentity = (effectiveNamespace, package.name)

canonicalName =
    effectiveNamespace 非空 ? effectiveNamespace + ":" + package.name
                             : package.name

entryKey =
    version 非空 ? canonicalName + "@" + version
                 : canonicalName
```

必须满足：

- 同一个 `PackageIndex` 内，`entryKey` 唯一。
- `build_index()` 扫描得到的无独立 version descriptor，其 `canonicalName` 唯一；手工构造
  的 versioned entries 可以共享 canonical identity，但 version/entry key 必须不同。
- 不同 namespace 的同一短名是两个合法身份。
- 同一完整身份对应两个 descriptor 是索引错误，不能覆盖、不能选一个继续。
- `canonicalName` 表示不带版本的包身份；`entryKey` 表示该身份下的具体索引条目。当前
  `build_index()` 生成的 descriptor entry 没有独立 version，因此两者相同。保留
  `entryKey` 是为了兼容 libxpkg 现有 versioned-entry API。
- 空 descriptor namespace 与显式写出 repo 默认 namespace 的同名包属于同一身份：

```text
repo default = alpha

{ namespace = "",      name = "demo" } -> alpha:demo
{ namespace = "alpha", name = "demo" } -> alpha:demo  // duplicate，构建失败
```

- 文件系统遍历顺序不得影响合法索引内容或错误结果。

### 2.3 非功能需求

- 构建复杂度保持 O(N)。
- 显式完整身份查询平均 O(1)。
- 裸名查询为 O(k)，`k` 是该短名的候选数，不扫描整个索引。
- ambiguity 与 duplicate 诊断按 canonical name/path 排序，跨平台输出稳定。
- 不增加第二套 Lua descriptor 解析器；身份元数据仍由 libxpkg loader 提取。

---

## 3. 方案比较

### 3.1 方案 A：完整身份表 + 短名候选表（推荐）

`PackageIndex` 的主表按 canonical name 唯一存储，并维护
`short name -> canonical names` 的二级索引。

优点：

- 与 xlings 已公开的 `namespace:name` 地址模型一致；
- 显式查询、裸名候选和 search 都有清晰语义；
- 同仓库与跨仓库最终都进入 catalog 的统一候选/ambiguity 流程；
- 查询复杂度稳定；
- 后续 alias、version、mutex 都可以在完整身份内演进。

代价：

- libxpkg 公共数据模型/API 需要升级；
- xlings `IndexManager` 和 catalog 需要从“单条命中”改为“候选集合”；
- cache 必须升级。

### 3.2 方案 B：`name -> vector<IndexEntry>`

主表直接按短名保存 vector。

优点是直观地保留碰撞项；缺点是所有现有单值操作
`find_entry/resolve/match_version/set_installed/mutex/merge` 都会突然变成多值操作，
完整身份没有成为一等概念，显式 namespace 仍需在 vector 上做后置过滤。

该方案修复了覆盖症状，但继续保留“存储按短名、namespace 是过滤条件”的架构方向，
不推荐。

### 3.3 方案 C：碰撞时 warn/error

在 `entries[key]` 写入前检测已有 key，warning 或直接失败。

优点是改动小、立即消除静默覆盖；缺点是合法的 `alpha:demo` 与 `beta:demo` 仍不能共存，
不满足 issue 的 Expected。

它只适合作为结构修复中的防御性检查，不能作为最终方案。

---

## 4. 推荐设计

### 4.1 libxpkg：让 PackageIdentity 成为一等数据

在 `mcpplibs.xpkg` 数据模型中引入身份元数据：

```cpp
struct PackageIdentity {
    std::string namespaceName;
    std::string name;

    std::string canonical_name() const;
};

struct IndexEntry {
    PackageIdentity identity;
    std::string canonicalName;
    std::string entryKey;
    std::string version;
    std::filesystem::path path;
    PackageType type;
    std::string description;
    bool installed = false;
    std::string ref;
};

struct PackageIndex {
    // entryKey -> unique entry
    std::unordered_map<std::string, IndexEntry> entries;

    // canonicalName -> sorted entryKey candidates
    std::unordered_map<std::string, std::vector<std::string>> identityEntries;

    // short package.name -> sorted, unique canonicalName candidates
    std::unordered_map<std::string, std::vector<std::string>> shortNames;

    std::unordered_map<std::string, std::vector<std::string>> mutex_groups;
};
```

`entries` 的 map key 与 `entry.entryKey` 必须一致。`IndexEntry::identity.name` 始终是
descriptor 的短 `package.name`，不再混用“entry key”“完整身份”和“包名”三个概念。
当前 descriptor build path 中 `entryKey == canonicalName`；现有 libxpkg
versioned-entry 用例则使用 `namespace:name@version`，并由 `identityEntries` 归组。

`canonicalName` 使用 xlings 已有用户地址形式 `namespace:name`，而安装目录继续使用现有
`namespace-x-name`，两者职责不混合：

- `namespace:name`：包身份与查询；
- `namespace-x-name`：文件系统 store name。

### 4.2 libxpkg：build_index 接收 repo 默认 namespace

保留 `build_index` 的第二参数，但明确其语义为 repo 默认 namespace：

```cpp
std::expected<PackageIndex, std::string>
build_index(const std::filesystem::path& repoDir,
            const std::string& defaultNamespace = "");
```

构建前先收集全部 `.lua` descriptor 路径，规范化并排序，再按稳定顺序加载。这样合法索引
结果和 duplicate 诊断都不依赖 `std::filesystem::directory_iterator` 的平台顺序。

对每个 descriptor：

```cpp
auto effectiveNamespace = pkg.namespace_.empty()
    ? defaultNamespace
    : pkg.namespace_;

PackageIdentity identity {
    .namespaceName = effectiveNamespace,
    .name = pkg.name,
};
auto key = identity.canonical_name();
```

写入前执行 duplicate 检查：

```text
duplicate package identity 'alpha:demo' in one index:
  first:  .../pkgs/a/alpha.demo.lua
  second: .../pkgs/b/other.demo.lua
```

duplicate 是 fatal build error。warning 后继续仍会产生不确定的 descriptor 选择，不能接受。

成功写入后，将 entry key 加入 `identityEntries[canonicalName]`，将 canonical name 加入
`shortNames[pkg.name]`。扫描结束后对每个候选 vector 排序并去重，使 search 和
ambiguity 输出稳定。

### 4.3 libxpkg：身份感知的查询 API

新增或替换为以下语义：

```cpp
const IndexEntry*
find_entry(const PackageIndex&, std::string_view entryKey);

std::vector<std::string>
find_candidates(const PackageIndex&,
                std::string_view shortName,
                std::optional<std::string_view> namespaceName = std::nullopt);

std::string
resolve_candidate(const PackageIndex&, std::string_view canonicalName);

std::optional<std::string>
match_version(const PackageIndex&,
              std::string_view canonicalName,
              std::string_view versionHint = {});
```

规则：

- 传 namespace 时，`find_candidates` 只查 canonical key，返回 0 或 1 项。
- 不传 namespace 时，从 `shortNames` 返回全部 canonical keys。
- alias 在候选身份确定后解析，不允许先按裸名选中一个 alias。
- alias 的裸 ref 默认继承 alias 自身 namespace；显式 `other:target` 才能跨 namespace。
- version 匹配只遍历 `identityEntries[canonicalName]`，不能把 `alpha:demo@1` 匹配到
  `beta:demo@1`。
- `set_installed` 接收最终 entry key；`mutex_packages` 等身份级 API 接收 canonical name。

现有 `merge(base, overlay, namespace)` 在 xlings 中没有生产调用点。建议同步收紧语义：

- overlay entry 已带 namespace 时保留；
- namespace 参数只作为 entry namespace 为空时的默认值；
- 完整身份碰撞返回 error，不再覆盖。

### 4.4 xlings IndexManager：封装 libxpkg 身份 API

`IndexManager` 增加 repo 默认 namespace：

```cpp
class IndexManager {
    std::filesystem::path repoDir_;
    std::string defaultNamespace_;
    // ...
};
```

`PackageCatalog::make_state_()` 将 `RepoIndexSpec::defaultNamespace` 传给 manager，
`rebuild()` 改为：

```cpp
xpkg::build_index(repoDir_, defaultNamespace_);
```

对外提供：

```cpp
std::vector<std::string>
find_candidates(std::string_view name,
                std::optional<std::string_view> namespaceName) const;

const xpkg::IndexEntry*
find_entry(std::string_view entryKey) const;

std::expected<xpkg::Package, std::string>
load_package(std::string_view entryKey) const;
```

`all_names/search` 返回 canonical identities；
`installed_names/entry_path/mark_installed/load_package` 使用最终 entry key。

### 4.5 xlings catalog：每个 repo 可以产生多个 PackageMatch

把当前：

```cpp
static PackageMatch build_match_(RepoState&, ParsedTarget_, ...);
```

改成：

```cpp
static std::vector<PackageMatch>
build_matches_(RepoState&, const ParsedTarget_&, ...);
```

解析流程：

```text
target
  │
  ├─ alpha:demo ─> repo.index.find_candidates("demo", "alpha") ─> 0..1
  │
  └─ demo       ─> repo.index.find_candidates("demo", none)    ─> 0..N
                                                                   │
                                                                   v
                  每个 canonical candidate 独立做 alias/version/package 加载
                                                                   │
                                                                   v
                              PackageCatalog 聚合所有 repo 的 PackageMatch
```

`collect_matches_()` 保留现有优先级：

1. project repo；
2. global primary repo；
3. 没有 primary 裸名命中时才使用 sub-index；
4. 显式 namespace 始终检查所有相关 repo；
5. 相同 project/global identity 继续执行 project scope 优先；
6. 最终 0 个候选为 not found，1 个成功，多于 1 个调用现有
   `format_ambiguous_candidates()`。

因此同一 repo 的两个 namespace 包自然得到：

```text
xlings info alpha:demo  -> alpha:demo
xlings info beta:demo   -> beta:demo
xlings info demo        -> ambiguous:
                           1. alpha:demo@1.0.0 from global repo 'demoidx'
                           2. beta:demo@1.0.0  from global repo 'demoidx'
```

### 4.6 search

libxpkg `search()` 遍历完整身份表，并同时匹配：

- canonical name；
- short name；
- description。

它返回 canonical keys。xlings search 对每个 key 构造 match，不再把 search 返回值重新当作
裸名查询，因此不会把两个结果再次折叠。

排序以 canonical name 为主，保证 `alpha:demo`、`beta:demo` 都显示且顺序稳定。

### 4.7 resolver 与依赖

顶层 install/info/remove 和递归依赖都必须调用 `PackageCatalog::resolve_target()`，不能保留
绕过 catalog、直接对 `IndexManager` 做裸名单项查询的生产路径。

依赖规则保持当前语义：

- descriptor 写 `alpha:dep` 时精确解析；
- descriptor 写裸 `dep` 时按 catalog 全局候选规则解析；
- 本次不新增“裸依赖自动继承声明者 namespace”的隐式规则。

如果裸依赖因本次修复暴露出真实的多 namespace 候选，应报 ambiguity，由包作者写完整身份；
不能继续依赖扫描顺序。

### 4.8 cache v2

当前 v1 cache 无法恢复 namespace，必须直接升级：

```json
{
  "version": 2,
  "repo_head_hash": "...",
  "default_namespace": "demoidx",
  "entries": {
    "alpha:demo": {
      "name": "demo",
      "namespace": "alpha",
      "canonical_name": "alpha:demo",
      "entry_key": "alpha:demo",
      "path": ".../alpha.demo.lua",
      "type": 0,
      "description": "alpha's demo package",
      "version": "",
      "ref": ""
    }
  }
}
```

规则：

- loader 只接受 `version == 2`；
- v1 即使 `repo_head_hash` 相同也视为 cache miss，重新扫描 descriptor；
- cache 中的 `default_namespace` 必须与当前 `RepoIndexSpec::defaultNamespace` 一致，否则
  即使 repo HEAD 相同也必须重建。同一物理 repo 可能以不同 repo name/default namespace
  挂载，单靠 HEAD 不能证明缓存身份上下文一致；
- `identityEntries` 与 `shortNames` 不必持久化，加载 v2 entries 时线性重建并排序；
- loader 校验 map key、`entry_key`、`canonical_name`、namespace/name/version 的推导关系；
  任一不一致都把整个 cache 视为 invalid，回退到 descriptor rebuild；
- cache 写入仍是 best effort；
- artifact/git repo head hash 与 cache 文件位置不变；
- cache 是可再生内部数据，不提供 v1 兼容读取开关。

### 4.9 错误处理

| 场景 | 行为 |
|---|---|
| 同 repo、不同 namespace、同 short name | 合法，保存两个候选 |
| 同 repo、相同 effective namespace + name | build_index fatal，列出两个路径 |
| 显式 namespace 无候选 | `package 'ns:name' not found` |
| 裸名多个候选 | ambiguity，列出 canonical name/repo |
| v1 cache | 忽略并重建 |
| v2 cache default namespace 与当前 repo spec 不同 | 忽略并重建 |
| v2 cache 数据自相矛盾 | cache invalid，重建 |
| malformed descriptor | 保持现有 loader/build 策略，本 issue 不扩展其诊断模型 |

---

## 5. 兼容性

### 5.1 用户行为

保持：

- 唯一裸名仍可直接使用；
- 显式 `namespace:name` 语法不变；
- 不同 repo 同名包的现有 ambiguity 行为不变；
- project/global/sub-index 优先级不变；
- 安装目录与 xvm namespace version 格式不变。

有意改变：

- 过去被静默覆盖的同仓库同名包现在全部可见；
- 裸名因此可能从“偶然选中一个”变成 ambiguity；
- 相同完整身份重复从“扫描顺序决定胜者”变成明确构建失败。

这两项变化都是把未定义/错误行为改为确定行为，不需要兼容开关。

### 5.2 libxpkg API

这是数据模型/API 变更，需要发布新的 libxpkg 版本，再由 xlings 升级依赖。

为降低迁移风险，可在一个 libxpkg release 周期内保留旧单项 API 作为 deprecated wrapper，
但 wrapper 遇到多候选必须返回空/错误，不能暗中选第一个。

### 5.3 cache

cache 只属于本地可再生状态。版本升级后的首次命令会重新扫描索引，后续恢复 cache hit；
不需要迁移工具。

---

## 6. 测试设计

### 6.1 libxpkg 单元测试

新增 fixture：

```text
pkgs/a/alpha.demo.lua  namespace=alpha name=demo
pkgs/b/beta.demo.lua   namespace=beta  name=demo
```

覆盖：

1. build_index 保留两个 canonical entries；
2. `find_candidates("demo", none)` 返回两个；
3. `find_candidates("demo", "alpha")` 只返回 alpha；
4. search name/description 均返回两个；
5. 两个相同 canonical identity 构建失败，错误含两个路径；
6. 空 namespace 使用传入的 default namespace；
7. 空 namespace 与显式 default namespace 冲突；
8. alias 裸 ref 不跨 namespace；
9. version matching 不跨 namespace；
10. merge 不覆盖完整身份。

### 6.2 xlings 单元测试

覆盖 `IndexManager` 和 `PackageCatalog`：

1. 同 repo 显式解析 alpha/beta 均成功；
2. 同 repo 裸 `demo` 返回 ambiguity，候选顺序稳定；
3. search 返回两个 canonical matches；
4. cross-repo 既有 ambiguity 不回归；
5. project scope 对同 canonical identity 的优先级不回归；
6. primary/sub-index 优先级不回归；
7. `mark_installed/load_package/entry_path` 使用 canonical key 操作正确；
8. 显式 namespace 的递归依赖解析正确；
9. 裸依赖多候选返回 ambiguity。

### 6.3 cache 测试

扩展 `tests/e2e/index_cache_test.sh` 或新增独立测试：

1. 第一次构建生成 v2 cache；
2. cache 同时保存 alpha/beta；
3. 第二次查询 cache hit，mtime 不变；
4. 手工放入同 HEAD 的 v1 cache，下一次查询仍重建为 v2；
5. 损坏 namespace/canonical_name 一致性时 fail closed 到 cache miss；
6. 同 HEAD、不同 default namespace 时 cache miss 并重建；
7. 从 v2 cache 加载后 explicit/bare/search 行为与冷构建一致。

### 6.4 隔离 E2E

新增 `tests/e2e/index_same_name_namespace_test.sh`，只使用临时 `XLINGS_HOME` 和本地 fixture：

```text
search demo              -> 同时包含 alpha:demo、beta:demo
info alpha:demo          -> alpha description
info beta:demo           -> beta description
info demo                -> non-zero + ambiguity + 两个候选
interface plan_install   -> 显式 namespace 产生正确 canonical package
```

测试不访问真实用户环境，不需要下载 `example.invalid` 资源；使用 `info/search/plan_install`
证明解析链即可。

### 6.5 回归验证

xlings：

```bash
xlings install
xlings use gcc@16.1.0
mcpp build
mcpp test
XLINGS_BIN=$(find target -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/index_same_name_namespace_test.sh
XLINGS_BIN=$(find target -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/index_cache_test.sh
```

libxpkg：

```bash
mcpp build
mcpp test
```

最终以 Linux、macOS、Windows CI 全绿为合入门槛。

---

## 7. 分阶段落地

### Phase 1 — libxpkg 身份模型

1. 增加 `PackageIdentity` 和 IndexEntry namespace/canonical metadata；
2. 改造 build_index，传入 default namespace；
3. duplicate identity fail closed；
4. 增加 short-name candidate index；
5. 改造 search/resolve/version/merge/set_installed；
6. 补齐 libxpkg 单测并发布新版本。

### Phase 2 — xlings 集成

1. 升级 `mcpp.toml` 的 libxpkg 依赖；
2. IndexManager 接收 repo default namespace；
3. cache 升级 v2；
4. catalog 从单 match 改为 match vector；
5. search/resolver/install/remove 统一 canonical identity；
6. 增加 unit + isolated E2E。

### Phase 3 — 真实索引验证

1. 对现有官方索引和多 namespace index 做全量冷构建；
2. 检查是否存在过去被覆盖的重复完整身份；
3. 验证现有短名唯一包行为不变；
4. 验证多 namespace 同短名出现稳定 ambiguity；
5. 三平台 CI 通过后合入。

不建议先发布“warning 后继续覆盖”的过渡版本。Phase 1 自身就应同时做到：

- 不同完整身份共存；
- 相同完整身份 fail closed。

---

## 8. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 现有调用方假设 `IndexEntry.name == map key` | 明确拆分 short name/canonical name，编译期逐点迁移 |
| 旧 cache 继续隐藏包 | cache format 强制升 v2，不读取 v1 |
| 同 repo HEAD 在不同默认 namespace 下误复用 cache | v2 header 记录并校验 `default_namespace` |
| 修复后裸依赖出现新 ambiguity | 输出完整候选，要求 descriptor 使用显式 namespace |
| alias/version 意外跨 namespace | 候选身份先确定，再在同 canonical base 内解析 |
| 同身份重复过去被扫描顺序掩盖 | 构建 fatal 并输出两个 descriptor 路径 |
| search 返回 canonical key 后二次解析折叠 | search 结果按 canonical key 直接构造 match |
| libxpkg 与 xlings 升级不同步 | 先发布并验证 libxpkg，再在 xlings 单独升级依赖 |

---

## 9. 非目标

- 不改变 `namespace:name` 用户语法。
- 不改变 `namespace-x-name` 安装目录格式。
- 不引入裸依赖自动继承当前包 namespace。
- 不改变跨 repo 的 project/global/sub-index 优先级。
- 不重构 descriptor malformed-error 策略。
- 不为 v1 cache 提供迁移或兼容开关。
- 不在 xlings 中复制 Lua descriptor 解析逻辑。

---

## 10. Review 结论

2026-07-25 review 已确认六项设计决策：

1. **跨仓库修复**：接受完整修复跨越 libxpkg + xlings 两个仓库；
2. **Canonical identity**：使用现有用户形式 `namespace:name`；
3. **重复完整身份**：`build_index` 构建失败，并报告冲突的 descriptor 路径；
4. **裸名多候选**：统一返回 ambiguity，不采用 first-match；
5. **Cache 升级**：直接升级 v2，不兼容读取 v1 cache；
6. **裸依赖 namespace**：维持现状，不隐式继承声明者 namespace；多个候选时返回
   ambiguity，由 descriptor 显式填写 `namespace:name`。

以上决策共同建立一个可验证的不变量：

> xlings 对外展示的 `namespace:name`，在索引存储、缓存、查询和依赖解析中始终代表同一个
> 完整包身份；任何歧义都显式报告，任何重复身份都拒绝构建。

---

## 11. 实施结果

设计已按六项 review 决策完成跨仓库落地并发布：

- libxpkg 0.0.46；
- mcpp-index `mcpplibs.xpkg` 0.0.46；
- xlings 0.4.69；
- xlings-res 的 GitHub/GitCode 二进制与 index artifact；
- xim-pkgindex 的 xlings 0.4.69 官方条目。

发布、CI、资源哈希和公共环境端到端验证记录见
[2026-07-25-issue381-namespace-index-identity-validation.md](2026-07-25-issue381-namespace-index-identity-validation.md)。
