# 版本键的 namespace 判定会随配置翻转 —— 根因与修复方案

> 状态:**已实现**(2026.9.2.1,分支 `fix/version-key-namespace-flip`)。基线 HEAD `a16f82f`,
> 发布版 `2026.8.30.2`;实施后 rebase 到 `f5a0775`(#576, 2026.8.30.2)。实施中被实测推翻/修正的判断见 §10,逐条标注而不是悄悄改掉。
> 所有数字都在真实 home(`/home/speak/.xlings`)或它的 slice 上实测,命令写在每节的
> 「实测」里,可复现。真实 home 全程只读:两次 `slice-real-home.sh verify-untouched`
> 均为 OK。

> **For agentic workers:** REQUIRED SUB-SKILL: 用 `superpowers:subagent-driven-development`
> (推荐)或 `superpowers:executing-plans` 逐任务实施。步骤是 `- [ ]` checkbox。
> 每个 D 都有自己的差分验收,**先让差分在旧二进制上失败**,再让它在新二进制上通过。

**Goal:** 让一个 `(target, version)` 的**版本键形状**由它自己的身份决定 —— 来自哪个
索引、由哪个 provider 注册 —— 而不是由 `index_repos` 数组的排列决定;并且一旦写下,
后续再注册也不另铸一个形状。

**Architecture:** 删掉 `version_namespace_()` 里最后一处「数组第 0 个条目 = 官方主索引」
的位置特权(#575 清掉了同步侧的三处,漏了这一处);读侧把单边规范化改成双边;写侧让
「已经登记过的键」优先于「现在铸出来的键」;并给已经产生的重复键一条可预览的修复路径。

**Tech Stack:** C++23 modules;gtest(`mcpp test`);bash e2e。

## Global Constraints

- 构建/测试只用 `mcpp build` / `mcpp test`,**不要**裸 xmake,**不要** `mcpp clean`。
- 工具链 `gcc@16.1.0`;链接报 glibc/musl 错时先 `xlings use gcc@16.1.0`。
- **模块实现单元里禁止 range adaptor 管道**(`views::split`、`transform | ranges::to`);
  `std::ranges::any_of` / `find_if` 安全(已在用)。
- 新增 e2e 必须注册进 `tests/e2e/run_all.sh`。
- commit 一律 `git commit -F -` + 带引号 heredoc(`-m` 里的反引号会被执行)。
- 断言只断言不变量,不断言索引版本号 / pointer revision / 快照条数。
- 任何在真实 home 上的验证都走 slice(`.agents/tools/slice-real-home.sh`),
  跑完必须 `verify-untouched`。

---

## 0. 核心:判定会变,键不会变

xlings 把「这个版本属于哪个索引」编进**版本键**里:

```
data/xpkgs/xim-x-claude/2.1.222     ← 目录名认 namespaceName,恒定
versions.claude.versions["2.1.222"]      ← 官方索引 → 裸键
versions.foo.versions["scode:1.0"]       ← 其它索引 → ns: 前缀
```

「官方索引不加前缀」是一条**向后兼容规则** —— 老 home 里全是裸键,给它们加前缀等于
一夜作废。规则本身没错。错的是**谁来回答「你是不是官方索引」**:

`src/core/xim/installer.cpp:869`

```cpp
std::string version_namespace_(std::string_view namespaceName) {
    const auto& globalRepos = Config::global_index_repos();
    const bool isPrimary = !globalRepos.empty()
        && namespaceName == globalRepos.front().name;   // ← 认下标,不认名字
    if (isPrimary || namespaceName.empty()) return {};
    return std::string(namespaceName);
}
```

于是同一个 `xim` 包,在**同一台机器上**,键的形状取决于 `index_repos` 数组里谁排第一:

| `index_repos` | `version_namespace_("xim")` | claude 注册进 DB 的键 |
|---|---|---|
| `[xim, scode, dsh]` | `""` | `2.1.222` |
| `[scode, dsh, xim]` | `"xim"` | `xim:2.1.222` |

**判定是可变的,写进去的键是永久的。** 这两句话放在一起就是整个 bug。

`#575`(2026.8.30.1)已经把这个特权从同步侧拆掉了 —— `main_repo_dir()` 变成常量,
「默认索引就是名叫 `xim` 的那条」,`Config::DEFAULT_INDEX_REPO_NAME` 这个常量也已经
存在(`config.cppm:107`)。**`version_namespace_` 是同一个特权的最后一处残留,
`#575` 漏了它。**

### 0.1 铁证(一条命令,不用读代码)

```
$ xlings remove patchelf -y --force
[error] uninstall failed: xvm removal selection failed for xim:patchelf@0.18.0:
        exact removal version is not registered (target='patchelf', version='xim:0.18.0')

$ jq '.versions.patchelf.versions | keys' ~/.xlings/.xlings.json
[ "0.18.0" ]
```

产品自己把两边都打印出来了:**查的是 `xim:0.18.0`,存的是 `0.18.0`。**

### 0.2 触发器怎么被按下的

`#575` 的 commit message 已经查清了这条路径,而且**每一步都是文档里的命令**:

```cpp
// config.cpp:576      读配置时,文件里没有 xim 就补在开头
globalIndexRepos_.insert(globalIndexRepos_.begin(), std::move(def));
// index_cmd.cpp:238   index use 物化默认条目 —— #575 已改成 insert(begin()),与 Config 一致
// cli.cpp:924         config --index-repo 仍然是 push_back(追加到末尾)
```

本机 `~/.xlings/.xlings.json` 的现状:

```json
"index_repos": [ {"name":"scode",...}, {"name":"dsh",...}, {"name":"xim",...} ]
```

`xim` 在 2 号位。`#575` 修好了「谁的目录装谁的索引」,但**没有回收 `xim` 的 0 号位**,
也没有必要回收 —— 平级化的意思就是位置不该有意义。可 `version_namespace_` 还在读位置。

**实测(键形状的翻转时间点):**

```
$ ls -ld --time-style=long-iso ~/.xlings/data/xpkgs/xim-x-{code,mesa,vim,nvidia-gl-host-link}/*
2026-09-01 16:44  xim-x-code/1.132.0            → 键 xim:1.132.0
2026-08-30 19:24  xim-x-mesa/25.0.7.2           → 键 xim:25.0.7.2
2026-08-30 01:51  xim-x-vim/8.1.1045            → 键 xim:8.1.1045
2026-08-30 15:39  xim-x-nvidia-gl-host-link/0.1.3 → 键 xim:0.1.3
```

**所有带 `xim:` 前缀的键,注册时间都在 2026-08-30 之后;之前装的全是裸键。**
claude 2.1.222 装于 08-26(`.xpkg-install.json` 记 `xlings_version: 2026.8.26.1`),
所以它是裸键 —— 而今天的 `remove` 拿 `xim:2.1.222` 去查。

---

## 1. 单变量差分:确凿

在真实 home 的 slice 上,**只改 `index_repos` 的顺序**,别的都不动:

```
index_repos = [scode, dsh, xim]              index_repos = [xim, scode, dsh]
──────────────────────────────────           ────────────────────────────────
$ xlings remove claude -y                    $ xlings remove claude -y
[xlings] xim:claude: registers no                ✓ xim:claude@2.1.222 detached
         xvm version; running its                  payload kept — still used
         uninstall hook and removing               by gfxbuild
         the payload                               remove it there too to
[error] uninstall failed: xvm removal              delete it for good
        batch failed for xim:claude@
        2.1.222: versionless removal
        has no unique validated
        selection (target='claude',
        version='')
EXIT=1                                       EXIT=0
```

右栏就是用户期望的行为,而且信息完整:**payload 保留,因为 gfxbuild 还在用。**

**实测复现:**

```bash
SP=<scratchpad>
bash .agents/tools/slice-real-home.sh --dst $SP/homeA \
     --bin ~/.xlings/bin/xlings --real xim-x-claude
# 注:该脚本用 python3,而本机 python3 被 shim 挡住;
#     用 /usr/bin/python3 单独跑其中的 repoint 段即可。
env XLINGS_HOME=$SP/homeA $SP/homeA/bin/xlings remove claude -y
jq '.index_repos = ([.index_repos[]|select(.name=="xim")]
                  + [.index_repos[]|select(.name!="xim")])' ...
env XLINGS_HOME=$SP/homeA $SP/homeA/bin/xlings remove claude -y
```

---

## 2. 五张面孔

同一个根因,在五个地方以五种不同的、互不相似的样子出现。**没有一张脸说出了真正的原因**,
其中两张还在甩锅。

### 2.1 `remove` 拒绝 —— 两种消息,取决于老记录有没有主

```
# 老记录无主(无 bindingGroup):claude、fd
[xlings] xim:claude: registers no xvm version; running its uninstall hook …
[error] xvm removal batch failed …: versionless removal has no unique validated
        selection (target='claude', version='')

# 老记录有主:patchelf、elfutils
[error] xvm removal selection failed for xim:patchelf@0.18.0:
        exact removal version is not registered (target='patchelf', version='xim:0.18.0')
```

第一条**是假话**。claude 注册过 xvm 版本(`versions.claude.versions["2.1.222"]`,
带 `envs.DISABLE_AUTOUPDATER`,只有 `register_batch` 会写 `envs`)。走到这句话是因为
`executing_provider_owns_no_version()`(`installer.cpp:843`)只看 `bindingGroup`:

```cpp
for (const auto& [_, data] : it->second.versions) {
    if (!data.bindingGroup || data.bindingGroup->provider.empty()) continue;
    if (data.bindingGroup->provider == executingProvider) return false;
}
return true;   // ← 老的无主记录 ⇒ 「不是我们的」⇒ 「没注册过 xvm 版本」
```

「没有这条记录」和「有一条老格式的记录」在这里长得一模一样。参见
`reference_absent_record_needs_observation`:**缺省值不是观察。**

而这句假话还有后果:它把删除引到 `hasSelection == false` 的 versionless 分支,
recipe 的 `xvm.remove("claude")` 不带版本,claude 有 6 个版本 → `AmbiguousVersion`。
最终那条 `versionless removal has no unique validated selection` 是**第三层症状**。

### 2.2 多 subos 只 detach 的路径被整个跳过 ← 用户报告的正是这一条

`installer.cpp:3207`

```cpp
auto stillReferenced = !detachVersion.empty()
    && detail_::is_version_referenced_anywhere_(scope, detachTarget,
                                                detachVersion, currentWorkspacePath);
```

`is_version_referenced_anywhere_`(`installer.cpp:1240`):

```cpp
auto matches = [&](std::string_view stored) {
    return stored == version
        || xvm::strip_namespace(std::string(stored)) == version;  // 只剥 stored 一边
};
```

`stored = "2.1.222"`,`version = "xim:2.1.222"` → 两个分支都不成立 → `stillReferenced = false`
→ **detach-only 分支不执行**,直接去删 payload,然后在 xvm 批量删除里炸掉。

**实测(claude@2.1.222 被 3 个 subos 引用):**

```
$ for f in ~/.xlings/subos/*/.xlings.json; do jq … ; done
current:  2.1.222   installed=2.1.222
default:  2.1.222   installed=2.1.222
gfxbuild: 2.1.222   installed=2.1.222
```

**detach 逻辑本身是好的** —— §1 右栏证明了它能正确工作并正确报出「gfxbuild 还在用」。
它只是没被走到。

### 2.3 `use` 静默改挂到无主的重复记录

```
$ jq '.versions.perl.versions' ~/.xlings/.xlings.json
  "5.44.0"      : { path: …/xim-x-perl/5.44.0/bin }                    ← 无主
  "xim:5.44.0"  : { path: …/xim-x-perl/5.44.0/bin, bindingGroup: xim:perl }  ← 有主

# workspace 之前:active = "xim:5.44.0"
$ xlings use perl@5.44.0
[xlings] perl -> 5.44.0
# workspace 之后:
{ "active": "5.44.0", "installed": ["5.44.0", "xim:5.44.0"] }
```

用户要的是同一个版本,拿到的是**另一条记录** —— 而且是那条**没有 bindingGroup** 的。
group 一致性、header/sysroot 的挂载全都挂在 group 上。这不是查不到,是查到了错的那条,
而且没有任何提示。参见 `reference_one_question_many_answerers`。

### 2.4 `self doctor` 报**假的** `incomplete install`

```
✗ incomplete install   xim:libxcb@1.17.0 did not finish installing; its payload
                       is on disk and nothing registered it
  → run                xlings install xim:libxcb@1.17.0
```

libxcb **注册得好好的**:`versions.libxcb.versions["1.17.0"]` 带
`bindingGroup.provider = "xim:libxcb"`,外加 33 条 `libxcb.files.N` 成员。

> **这一条要放低断言强度。** `stamped_incomplete()`(`payload.cpp:112`)读的是
> `.xpkg-install.json` 里的 `"incomplete": true`,与 namespace 无关 —— 也就是说
> doctor 这句话在**它自己的口径下是真的**:上一次安装确实没跑完。真正的问题是
> **上一次安装为什么没跑完**,答案在 2.5。所以 2.4 不是独立缺陷,是 2.5 的可见后果;
> 但它给出的 remedy(`xlings install xim:libxcb@1.17.0`)在 2.5 修好之前**永远失败**,
> 这一点是缺陷。

### 2.5 `install` 被楔死,而且消息甩锅给 recipe ← 最严重

```
$ xlings install xim:libxcb@1.17.0 -y
[warn] [libxcb@1.17.0] previous install left an incomplete state; running its install hook again
[error] registration group 'libxcb' conflicts with persisted root 'libxcb@1.17.0'
          code:     xvm-group-conflict
          provider: xim:libxcb@1.17.0
          at:       libxcb-composite.so@xim:1.17.0
          field:    /nodes/1/binding/rootTarget
          hint:     the recipe puts one release under two different roots;
                    give the members a single common binding target
          nothing was changed
[error] [libxcb] failed: config hook failed
  ✗ 5 package(s) failed
```

**recipe 是无辜的。** 机制在 `registration.cpp:419-489`,关键是**一个结构体内部的不对称**:

```cpp
// installer.cpp:200-203  —— provider 版本是裸的
.provider        = "xim:libxcb",
.providerVersion = node.version,          // "1.17.0"  ← 裸
// installer.cpp:274      —— 成员版本键被加了前缀
registrationNode.version = make_ns_version("xim", "1.17.0");  // "xim:1.17.0"
```

于是:

| | 值 | 结果 |
|---|---|---|
| `persistedGroups` 收集条件 | `ref.provider == batch.provider && ref.providerVersion == batch.providerVersion` | 裸的 `1.17.0` **命中** |
| persisted root | `{libxcb, 1.17.0}` | |
| batch root | `{libxcb, xim:1.17.0}` | **不等** |
| persisted 成员是否在 batch 里 | `(libxcb-composite.so, 1.17.0)` vs `nodeIndexes` 里的 `(…, xim:1.17.0)` | **缺失** |
| → | `GroupConflict` | 「一个 release 挂在两个 root 下」 |

**provider 身份匹配上了(所以守卫开火),成员身份没匹配上(所以看起来像两个 root)。**
两者的差别只有一个 `xim:` 前缀。

`nothing was changed` —— 装不上;§2.1 —— 删不掉。**这些包被彻底楔死了。**

**实测(真实 home 上被楔死的包,10 个):**

```
$ grep -l '"incomplete": true' ~/.xlings/data/xpkgs/*/*/.xpkg-install.json
xim-x-expat/2.6.2          xim-x-libXau/1.0.11      xim-x-xorgproto/2024.1
xim-x-gcc-runtime/15.1.0   xim-x-libxcb/1.17.0      xim-x-zlib/1.3.1
xim-x-glibc/2.44           xim-x-libXdmcp/1.1.5
xim-x-libffi/3.4.4         xim-x-mcpp/2026.8.26.2
```

全部由 `xlings_version: 2026.8.30.1` 写下,`reason: "config hook failed"` —— 翻转之后。
这是 glibc / X11 / mesa 依赖链的底座。

---

## 3. 为什么会放大:读侧只规范化了一边

`resolve_exact_version_key`(`db.cpp:67`):

```cpp
if (version.find(':') != std::string::npos) {
    if (it->second.versions.contains(version)) return version;
    return std::unexpected(… "exact removal version is not registered" …);  // ← 零宽容
}
std::vector<std::string> matches;
for (const auto& [storedVersion, _] : it->second.versions)
    if (strip_namespace(storedVersion) == version) matches.push_back(storedVersion);  // ← 有宽容
```

**裸查询宽容,带 namespace 的查询零宽容。** 而顺序翻转恰好只制造后一种方向
(查询有前缀 / 存储没有)。`is_version_referenced_anywhere_::matches` 同病。

参见 `reference_canonicalize_both_sides`:单边解析让 143 个 payload 看起来是坏的。
这次是同一个形状,换了个字段。

---

## 4. 现有 home 的损伤量(实测)

```
$ jq … ~/.xlings/.xlings.json
xim 包注册的、只有裸键的 target        767   ← 今天全都 remove 不掉
带 xim: 键的 target                    468   ← 翻转之后装的
同一版本两种键都有(重复对)            240
  其中 path 完全相同                   240 / 240   ← 同一份 payload,重复登记
  其中 裸=无主 且 xim:=有主            240 / 240   ← 形态完全一致
  其中 裸记录没有 kind 字段            240 / 240   ← 都是 #384 之前的遗留记录
被楔死(装不上也删不掉)的包            10
```

**两个人群,一条根因:**

| 翻转前那条记录 | 再注册时发生什么 | 症状 |
|---|---|---|
| **无主**(#384 之前的遗留) | 新键与老键并存,不冲突 | 240 对重复 → §2.3 `use` 挂错 |
| **有主**(#384 之后、翻转之前) | `GroupConflict` | 10 个包楔死 → §2.5 |
| 两种都算 | `remove` 查不到裸键 | §2.1 / §2.2 / §2.4 |

### 4.1 为什么「把 `xim` 挪回 0 号位」不是干净的绕过

它只是把失败搬个家:

```
$ # index_repos = [xim, scode, dsh]
$ XLINGS_ACTIVE_SUBOS=eco-vk-test xlings remove elfutils -y --force
[error] xvm removal selection failed for xim:elfutils@0.191:
        bare removal version '0.191' matches 2 stored versions
```

这台机器上已经有**两代键共存**,任何单一的 namespace 判定都救不了全部。所以 D1 必须做,
但**只有 D2 + D6 才能让现存的两代键都可达**。

---

## 5. 修复方案

六个改动。D1 是触发器,D2 是放大器,D3 断绝再生,D4/D5 是两条假消息,D6 是存量修复。
**每一个都可以单独 review 和单独回滚。**

### D1 — 触发器:认名字,不认下标

`src/core/xim/installer.cpp:869`

```cpp
std::string version_namespace_(std::string_view namespaceName) {
    // 默认索引就是名叫 `xim` 的那条 —— 与 #575 之后的 main_repo_dir() 同一口径。
    // 读 index_repos 的排列会让同一个包的版本键形状随一次配置编辑翻转,而键是永久的。
    if (namespaceName.empty()
        || namespaceName == Config::DEFAULT_INDEX_REPO_NAME) {
        return {};
    }
    return std::string(namespaceName);
}
```

`Config::global_index_repos()` 在这里不再被读 → **数组排列不再决定任何身份。**
这是 `#575` 「条目平级」那句话的最后一块。

**不变量:** 同一个 `namespaceName`,在任意 `index_repos` 排列下,`version_namespace_`
给出同一个答案。

**注意** 这是行为变更:翻转期间(08-30 起)装的 468 个 `xim:` 键,之后铸出来的会是裸键。
它们靠 D2 保持可达,靠 D3 不再产生新的重复,靠 D6 清理已有的重复。

### D2 — 读侧双边规范化

`db.cpp: resolve_exact_version_key`:带 namespace 的查询精确未命中时,**退回到与裸查询
同一段逻辑**(两边都 `strip_namespace` 后比较),而不是立即失败。

`installer.cpp: is_version_referenced_anywhere_::matches`:两边都 strip。

```cpp
auto matches = [&](std::string_view stored) {
    return stored == version
        || xvm::strip_namespace(std::string(stored))
               == xvm::strip_namespace(version);
};
```

歧义仍然是错误(strip 后多于一个命中 → `AmbiguousVersion`),**不猜**;D3/D6 负责让这种
歧义在构造上不再产生。

**不变量:** 读侧对键形状的宽容度与方向无关 —— 带前缀查裸键,和裸键查带前缀,行为对称。

### D3 — 键形状一次决定,不再另铸

新 helper,放在 `xvm/db`:

```cpp
// 这个 (target, 裸版本) 在库里已经有键、且本 provider 有资格认领它 —— 就用那个键,
// 不要按今天的判定另铸一个形状。判定可以变,键不该跟着变。
std::optional<std::string> claimable_existing_key(
        const VersionDB& db,
        const std::string& target,
        const std::string& bareVersion,
        const std::string& provider,      // "xim:libxcb"
        const std::string& payloadPath);  // 本包 install_dir
```

认领条件(二选一,都不成立就铸新键):

1. 该记录 `bindingGroup->provider == provider`;或
2. 该记录**无主**,且它的 `path` 落在 `payloadPath` 之内 ——
   与 `registration.cpp:732` 的 `payload_path_covers_` 同一判据。

> 条件 2 里的「无主」不能省。两个不同索引合法地各自提供 `foo@1.0` 时,复用会把两份不同的
> payload 并成一个键。有主记录只认同 provider,无主记录靠 payload 归属来证明。

调用点(三个写者,同一个 helper):

- `installer.cpp: normalize_xpkg_registration_plan` —— 240 对重复全部来自这条路;
- `xim/libxpkg/types/script.cpp:51`;
- `xim/libxpkg/types/subos.cpp:75`。

**不变量:** 同一个 `(target, 裸版本, provider)` 在 DB 里只有一个键。

**副作用(正面):** §2.5 的 `GroupConflict` 从此不会被触发 —— batch root 会复用
`{libxcb, 1.17.0}`,与 persisted root 相等,10 个楔死的包能直接装回去。

### D4 — 「没注册 xvm 版本」需要一次观察

`installer.cpp:843 executing_provider_owns_no_version`:在返回 `true` 之前,先看一眼
payload —— 目标下若存在一条 `path` 落在本包 install_dir 之内的记录,那它**就是我们的**,
只是键没解析出来。这时不要打 `registers no xvm version`,让原始的解析错误照原样报出去。

```
# 现在(假话,且引向 versionless 分支)
[xlings] xim:claude: registers no xvm version; running its uninstall hook …
[error] … versionless removal has no unique validated selection

# 之后(真话)
[error] xvm removal selection failed for xim:claude@2.1.222:
        exact removal version is not registered (target='claude', version='xim:2.1.222')
```

D1+D2 之后这条路本来就不该走到;D4 是保证**万一走到,说的是真话**。
参见 `reference_absent_record_needs_observation`、`project_silent_success_pattern`。

### D5 — `GroupConflict` 的消息不许甩锅 recipe

`registration.cpp:467`:当 persisted root 与 batch root **只差 namespace**
(`target` 相同且 `strip_namespace(version)` 相同)时,这不是 recipe 的错。

- 保留 `xvm-group-conflict` 这个 code(它对真正的 recipe 错误仍然正确);
- 该情形下换一条诊断:「同一版本在库里存在两种键形状 —— 运行 `xlings self doctor --fix`」;
- 原 hint(「recipe 把一个 release 挂在两个 root 下」)只留给 root 的**裸版本**确实不同的情况。

参见 `reference_gate_the_message_on_behaviour`:消息要挂在行为上,不是挂在猜测上。

### D6 — `self doctor`:报告并合并重复键

**一个谓词,reporter 与 repairer 共用**(`reference_reporter_repairer_predicate_drift`):

> target `T` 上存在键 `K1`、`K2`,满足
> `strip_namespace(K1) == strip_namespace(K2)`、`path(K1) == path(K2)`,
> 且恰有一条带 `bindingGroup`。

- **报告**:`duplicate version key` —— `T@<bare>` 在库里有两条记录(`K1`、`K2`),
  指向同一份 payload。
- **`--fix`**:
  1. winner = 带 `bindingGroup` 的那条(它携带 group 接线);
  2. 每个 subos 的 `workspace[T].active` / `installed[]` 里的 loser 键改写成 winner 键,去重;
  3. 删除 loser 的 `versions` 条目;
  4. 清理 `VInfo::bindings` 里指向 loser 的边。

**实测基线(本机):** 240 对,`bareOwned=false / nsOwned=true` 240/240,`path` 全等
240/240 —— 形态完全一致,规则不需要分支。

> 为什么不放在 `Config::load` 里自动迁移:那会让每条命令都重写 versions DB,不可预览、
> 出错时无从解释。参见 `reference_atomic_write_vs_flock`、`project_silent_success_pattern`。
> doctor 有 preview、有 `--fix`、有报告,是这件事该待的地方。

---

## 6. 不做什么

- **不改用户的 `index_repos` 排列,不做配置迁移。** D1 之后位置不再有意义,这就够了;
  去「纠正」用户的数组反而是又一个位置特权。
- **不把 `xim:` 前缀推广到默认索引。** 那会让所有老 home 的裸键一夜作废,
  是把 767 个 target 的问题扩大到全部。
- **不在读侧猜歧义。** strip 后多于一个命中就报 `AmbiguousVersion`;
  让 D3/D6 消灭歧义的来源,而不是让读侧替用户选。
- **不动 detach 的逻辑。** §1 已经证明它是对的,它只是没被走到。

---

## 7. 任务清单

### D1 触发器
- [x] `installer.cpp: version_namespace_` 改为比较 `Config::DEFAULT_INDEX_REPO_NAME`
- [x] unit:同一 `namespaceName` 在 3 种 `index_repos` 排列下答案相同
- [x] 确认 `Config::global_index_repos()` 在 installer.cpp 中不再被 `version_namespace_` 读

### D2 读侧双边规范化
- [x] `db.cpp: resolve_exact_version_key` 带 ns 的查询未命中时退回 strip-both 比较
- [x] `installer.cpp: is_version_referenced_anywhere_::matches` 两边都 strip
- [x] unit:`(存裸,查带前缀)` 与 `(存带前缀,查裸)` 两个方向对称;歧义仍报 `AmbiguousVersion`

### D3 键形状一次决定
- [x] `xvm/db` 新增 `claimable_existing_key`(含无主 + payload 归属的判据)
- [x] `normalize_xpkg_registration_plan` 接入(需要把 db 传进来)
- [x] `types/script.cpp`、`types/subos.cpp` 接入同一 helper
- [x] unit:同 provider 复用;**不同 provider 同裸版本不复用**(防止把两份 payload 并成一个键)

### D4 兜底判断需要观察
- [x] `executing_provider_owns_no_version` 加入 payload 归属观察
- [x] unit:老的无主记录 + payload 在本包目录内 ⇒ 返回 false(不是「没注册」)

### D5 消息
- [x] `registration.cpp` GroupConflict:root 只差 namespace 时换诊断,不甩锅 recipe
- [x] unit:root 裸版本不同 ⇒ 仍是原 hint

### D6 doctor 修复存量
- [x] `duplicate version key` 谓词(一个,reporter/repairer 共用)
- [x] reporter + `--fix`(workspace 改写、loser 删除、bindings 边清理)
- [x] unit:修复后 `--fix` 再跑一次为零(收敛)
- [x] e2e:在含重复键的 fixture 上 `doctor --fix` 后 `use` 停在有主记录

### 集成
- [x] e2e `tests/e2e/version_key_namespace_flip_test.sh`,注册进 `run_all.sh`
- [x] 在 slice 上跑完 §8 全部六条差分,并 `verify-untouched`

---

## 8. 验收(差分:先在 `2026.8.30.2` 上失败,再在新构建上通过)

| # | 命令 | 现在 | 期望 |
|---|---|---|---|
| 1 | `remove claude -y` | `versionless removal has no unique validated selection` | `detached`,`payload kept — still used by gfxbuild` |
| 2 | `remove patchelf -y --force` | `exact removal version is not registered (version='xim:0.18.0')` | 成功 |
| 3 | `install xim:libxcb@1.17.0 -y` | `xvm-group-conflict` + `nothing was changed` | 成功,`incomplete` 戳消失 |
| 4 | `use perl@5.44.0` | workspace 改挂到无主 `5.44.0` | 停在有主记录 |
| 5 | `self doctor` | 10 条 `incomplete install`;0 条重复键 | 3 之后 incomplete 归零;新增 240 条 `duplicate version key`,`--fix` 后归零且收敛 |
| 6 | 1–5 在 `[scode,dsh,xim]` / `[xim,scode,dsh]` / `[dsh,xim,scode]` 三种排列下 | 结果不同 | **结果相同** |

第 6 条是这次真正要买的东西:**排列不再能改变任何行为。**

---

## 9. 复现环境

```
slice:      <scratchpad>/homeA        (hardlink farm,--real xim-x-claude, xim-x-libxcb)
原配置备份:  <scratchpad>/homeA/.xlings.json.orig
真实 home:  两次 verify-untouched 均 OK,全程只读
```

已知坑(下一个人会踩):

- `slice-real-home.sh` 用 `python3`,而本机 `python3` 被 shim 挡住
  (`[error] python3 is not installed in this subos`)。用 `/usr/bin/python3`
  单独跑脚本里 189–259 行那段 repoint,再补 `cp bin/xlings` 和
  `ln -sfn default subos/current` 两步。
- 任何会**重装**的实验必须先把对应 store 目录换成真拷贝(`rm -rf` slice 里的名字再
  `cp -a`),否则 install hook 会就地改写 hardlink 共享的 inode ——
  那是**会写穿到真实 home** 的唯一方式。
- 跑完一定 `verify-untouched`,不要靠推理。

---

## 10. 实施记录:自我 review 改掉的两处,和实测新发现的一处

**先说结论:§5 的六个改动全部落地,但其中两个的形状在自我 review 时改了,实施中又多出第七处缺陷。**
每一条都有对应的测试;§8 的六条验收在 slice 上全绿,同一脚本在 `2026.8.30.2` 上报 11 条缺陷。

### 10.1 D2 改了:读侧不是"两边都 strip",是"带前缀的查询只多认裸拼法"

§5 原文写的是"退回到 strip 两边比较"。写代码时发现这**太宽**:`xim:1.0` 会命中 `scode:1.0`,
而那是另一个索引的 payload。最终规则(`xvm::version_key_matches`,一个答案者,三处调用):

| 查询 | 命中 |
|---|---|
| `v`(裸) | `v`、任何 `ns:v`(历史行为,保留) |
| `ns:v` | `ns:v`、**`v`**(默认索引的记录本来就是裸的) |
| `ns:v` | ~~`other:v`~~ 不命中 |

`is_version_referenced_anywhere_::matches`、`profile.cpp find_subos_pinning_version::matches`、
`resolve_exact_version_key` 都走它。单元测试 `ANamespacedQueryDoesNotReachAnotherNamespace`
锁住那条不命中。

### 10.2 D3 改了:不是"逐 target 复用键",是"每个 batch 用 provider 已有记录的命名空间"

§5 原文的 `claimable_existing_key(target, bareVersion, …)` 是逐 target 决定拼法。写到
`normalize_xpkg_registration_plan` 才看清:一个 batch 里的 root 和 members 如果各自决定,
新版本 recipe 新增的成员会拿到与 root 不同的拼法,group 就散了;而且 headers 的
`effects[].version` 也从同一个 namespace 铸出来,两边必须同源。

最终:`xvm::registered_namespace_for(db, provider, providerVersion, primaryTarget, payloadPath)`
——**这个 provider 的这个 release 在磁盘上已经用哪个命名空间,整个 batch 就用哪个**;
有主记录优先,其次是 payload 落在本包目录内的无主记录;都没有才按今天的判定。
`installer.cpp: effective_version_namespace_` 包一层,四个写者/读者共用
(`process_xvm_operations_`、script/subos 的 `default_config`、`remove` 的 `detachVersion`)。

这一条同时让 D5 的 `GroupConflict` 在构造上不再触发——batch root 会复用
`{libxcb, 1.17.0}`,与 persisted root 相等。§8 第 3 条在 slice 上直接装回去了。

### 10.3 "孪生键"成为一个概念,而不是 D6 的私有谓词

§6 原文写"不在读侧猜歧义"。但 D1 落地后,本机 240 对重复键的裸查询会变成
`AmbiguousVersion`——**在用户跑 doctor --fix 之前,升级本身会把这 240 个 target 弄坏**。
这不是"无感升级"。

所以把"同一裸版本 + 同一 `path`"定义成**孪生**(`twin_version_keys`),并让四个地方共用它:

| 谁 | 做什么 |
|---|---|
| `resolve_exact_version_key` / `match_version` | 一对孪生收敛成一个答案:带 `bindingGroup` 的那条(`representative_version_key`) |
| `apply_removal_batch` | 删一个就删整对,workspace 里两个键都清掉——否则幸存的那条让 `remove` 之后包"还在" |
| doctor reporter | `plan_twin_merges` |
| doctor `--fix` | 同一个 `plan_twin_merges` |

这不是猜:孪生是**同一份 payload 被登记了两遍**,`path` 相等是证据。测试
`TheSamePayloadIsTheEvidenceForATwin` 锁住"path 不同就不是孪生,仍报歧义"。

### 10.4 实测新发现:doctor 用扫描时的旧快照做"其它 subos"的修复计划(第七处)

e2e S5 第一版跑出来:`--fix` 之后 `other` subos 的 `active` **消失**了。doctor 的完整输出:

```
· merged                flip-tool@1.1.0 folded into xim:flip-tool@1.1.0
· merged                other: 2 reference(s) moved to the surviving record
· other subos repaired  other: deactivated 'flip-tool' — it named a version that is not registered
```

`repair_other_subos_()` 用 `plan_subos_metadata_repair(Config::versions(), st.otherSubos)`
——**当前的 DB** 对 **扫描时的快照**。合并已经把 `other` 改到了幸存记录上,但快照里它还指着
被删的那条,于是计划说"deactivate",然后把这个旧计划**应用到重新读出来的新文件上**。
一个 reporter/repairer 漂移的变体:计划与被修复对象不是同一份数据。

修法:apply 之前对重新读出的文件**重新计划**。同一个谓词、同一份数据。

### 10.5 单元测试文件不能独立成 TU

新建的 `test_version_key_identity.cpp` 在 GCC 16 上 ICE:
`failed to load pendings for 'std::map'`,并把锅甩给 `xlings.core.xvm.commands`
("Bad file data")。就是 `test_xvm_bindings.cpp` 文件头写的那个编译器 bug——在
namespace 作用域的 helper 里首次实例化 `std::map<std::string, VInfo>`。测试并入
`test_xvm_bindings.cpp` 末尾,helper 改名避免冲突。参见 `reference_gcc16_modules_map_ice`。

### 10.6 实测数字(slice,新旧二进制同一脚本)

| | 新构建 2026.9.2.1 | 发布版 2026.8.30.2 |
|---|---|---|
| e2e `version_key_namespace_flip_test.sh` | PASS(S1–S6) | 11 defects |
| S3 `remove` 另一个 subos 在用的版本 | detached,payload kept | `exact removal version is not registered (version='xim:1.0.0')` |
| S4 翻转后重装 | 成功,一种拼法 | `xvm-group-conflict` |
| S5 `use` | 落在有主记录 | 落在无主重复记录 |
| S5 doctor / --fix | 报告 / 合并 / 收敛 | 不报告 / 不合并 |

### 10.7 顺带量到、不在本次范围的:`remove fd`

fd 在这台 home 上只有一条无主裸记录 `10.4.2`,不在任何 subos 的 workspace 里。
`remove fd` 把它解析成索引的 latest `10.5.0`(不是装着的那个),于是走到
`registers no xvm version` → `removal operation has no exact version (version='')`。
**与排列无关**——发布版在 `[xim,…]` 下同样失败;是"只有一个存储版本的 versionless 删除在
`hasSelection == false` 时从不解析"这条独立缺陷。另开 issue,不塞进本 PR。

### 10.8 slice 上的完整验收(§8 六条 + 楔死的 10 个包)

全部通过;数字见 `2026-09-02-release-2026.9.2.1-notes.md`。10 个 `incomplete` 包逐个
`xlings install` 装回去,剩余 0。`verify-untouched` OK。
