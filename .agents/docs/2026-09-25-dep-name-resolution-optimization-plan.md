# 依赖名解析、索引注册与 SubOS 用户数据保护:总体优化方案

**日期**:2026-09-25
**类型**:架构设计(改变依赖解析语义和 subos 删除语义;涉及 xlings + xim-pkgindex,不涉及 libxpkg)
**状态**:D1–D9(含 D2')全部已定并实现,见 §15 实施记录
**起因**:
- 第一部分:`xlings install xmake` 报 `package 'ncurses' is ambiguous`,分析见
  [2026-09-25-bare-dep-ambiguity-analysis.md](2026-09-25-bare-dep-ambiguity-analysis.md)
- 第二部分:维护者反馈"自动修复、升级或 `self doctor --fix` 在某种情况下删掉了 subos 的用户
  目录,丢了很多用户数据"。排查与方案见 §8–§14
**前置设计**:[2026-08-05-dependency-resolution-single-source.md](2026-08-05-dependency-resolution-single-source.md)
(把"依赖是哪个**版本**"收成一个回答者)。第一部分把同一原则延伸到"依赖是哪个**包**"。

**两部分共用的一条原则**:一个问题只有一个回答者;"没能读到"不等于"是空的"。

---

# 第一部分:依赖名解析与索引注册

---

## 0. 一句话

recipe 里写的依赖名,现在是在**用户配置的仓库集合**里解析,而且在 xlings 里**被回答了五次**。
本方案让它在**声明它的那个索引**里解析,**只回答一次**,之后所有地方都只读这一次的结果。

---

## 1. 三个问题,各有几个回答者

这次故障涉及三个问题,每个都不止一个地方在回答。

### Q-A "recipe R 里的依赖名 N 指的是哪个包?" —— 五个回答者

| # | 位置 | 怎么回答 | 问题 |
|---|---|---|---|
| A1 | `resolver.cpp:65`(`expand`) | `catalog.resolve_target(dep)`,不知道是谁声明的这个依赖 | 结果随用户配置变化(本次故障) |
| A2 | `resolver.cpp:219`(`topoVisit`) | 为了算边,**把依赖再解析一遍** | 必须和 A1 答得一样,但只是碰巧一样 |
| A3 | `installer.cpp:2783`(`deps_exports` / `resolved_deps`) | 在整个 plan 里按 `name`/`canonicalName`/`rawName` 做字符串匹配,**取第一个** | plan 里有两个同名节点时按顺序取,可能取错 |
| A4 | `installer.cpp:2478`(`locate_dep_install_dir_`,构建依赖) | 同样按名字匹配,取第一个 | 同 A3 |
| A5 | `commands.cpp:988`(`direct_dependents_of`,remove 前的检查) | 只比较去掉命名空间后的名字 | 会多报:删 `scode:ncurses` 时会把 `xim:xmake` 列为依赖方 |

libxpkg 的 `pkginfo.resolved_dep` **不算一个回答者**:它只在"这个包自己的依赖记录"
里查,而且多于一个就拒绝,是正确的。前提是 A3 写进去的记录本身是对的。

**A3 是本次调查中新发现的潜在缺陷**,不需要改任何配置就可能出现:默认配置下执行
`xlings install scode:ncurses xmake`,plan 里同时有 `scode:ncurses` 和 `xim:ncurses`,
而 `xim:xmake` 的依赖 `"ncurses"` 在 A3 里会命中 plan 顺序上排在前面的那个。plan 顺序
来自对 `unordered_map` 的遍历。如果命中的是 scode 的**源码目录**,xmake 的 RPATH 就会
指向一个没有库的目录。**这是从代码推断出来的,还没实测;实施的第一步就是复现它**(AC3)。

### Q-B "仓库 X 是哪一级?" —— 由它是怎么注册的决定

- 写在 `index_repos` 里的是一级仓库(`repo_specs_`,`catalog.cpp:344`);`xim` 声明的
  子索引是次级(`catalog.cpp:377`)。
- 同一个 URL 可以同时以两种方式注册,各同步一份,解析时再按 `repoName` 合并,
  用户看不出来。
- 写入入口有两个:`config --index-repo` 和 NDJSON `add_repo`,都不检查是否重复。
  `self doctor` 也不报告。

### Q-C "失败了什么、用户该做什么?" —— 答错了

- 同一条错误打印两次:libxpkg 把 `deps` 同时放进 runtime 和 build 两张表,
  resolver 失败时又不记录这个 key。
- 没有依赖链:用户敲的是 `xmake`,报错只提 `ncurses`。
- 给出的补救命令(`install <候选>`)执行成功,**但解决不了问题**(分析报告腿 C 已验证)。

---

## 2. 设计原则

1. **依赖名按声明方解析。** 不带前缀的依赖名,首先指**声明它的 recipe 所在的索引**里
   的那个包。在那个索引里找不到,才退回现在的全局规则。recipe 的含义应该由它所在的
   索引决定,不应该由安装它的用户加了哪些仓库决定。
2. **一次解析,之后都只读结果。** 解析只发生在 resolver 里,结果以**边**的形式记在
   plan 节点上。A2–A5 只读这些边,不再自己推导。(08-05 文档的原则 1,范围从版本扩大到包。)
3. **一个仓库只有一个身份。** 同一个命名空间在配置里只能对应一个仓库,不能因为注册
   方式不同就拿到不同的优先级。
4. **失败只报一次,补救命令必须能让用户达成目标。** 光能执行不够,执行完要能把用户
   原本想装的东西装上。
5. **老客户端靠索引侧修复。** xlings 侧的修复只对新客户端生效,所以两边都要做,
   而且索引侧先发布。

---

## 3. 方案:六个工作项

### W0 索引侧即时修复(xim-pkgindex,对所有客户端生效)

- `pkgs/x/xmake.lua:98`:`deps = { "ncurses", … }` → `deps = { "xim:ncurses", … }`
- 删掉 `.github/scripts/check-dep-namespace.lua:74` 的过期豁免(#582,
  "remove once published",ncurses 2026-08-09 就已发布)
- 改正 `xmake.lua:87-96` 那段注释中"bare form resolves correctly in every state"的说法

已验证:用户配置不变,只改这一行就能解析成功(分析报告腿 F)。
显式前缀是老语法,所有已部署的客户端都认。

### W1 依赖按声明方解析 + 结果记成边(xlings,核心)

**接口**:catalog 增加一个依赖解析入口,把"谁声明的"一起传进去:

```cpp
// 按声明方解析 recipe 依赖。显式前缀的行为不变。
std::expected<PackageMatch, std::string>
PackageCatalog::resolve_dependency(const std::string& spec,
                                   const DeclaringRepo& declarer,   // repoName + scope
                                   const std::string& platform) const;
```

规则:

1. 带显式前缀(`xim:glibc@>=2.38`)→ 和今天的 `resolve_target` 完全一样。
2. 不带前缀 → 先在 `declarer` 这个仓库里找。在同一个仓库里,同一个名字只有一个完整
   身份(#381 已保证),找到就用。
3. 在自己的仓库里找不到 → 退回 `resolve_target` 的全局规则。全局规则仍然可能报歧义,
   那是真的歧义,交给 W3 报告。
4. **用哪个仓库只看名字,版本在选定的仓库里解决。** 声明方仓库里有这个名字、但没有要求的
   版本时,报错并指出是哪个仓库的哪个包缺这个版本,**不能因为版本对不上就换到别的仓库**。
   `pin_target_to_subos` 也一样:pin 过的版本在声明方仓库里不存在,就在**同一个仓库里**
   退回不 pin 的解析(和今天 `resolver.cpp:66-68` 的退回方式一致),而不是换仓库。
   否则会出现"subos 里激活的是 `scode:ncurses@6.4`,于是 `xim:xmake` 的依赖被 pin 到 6.4,
   xim 里没有 6.4,结果解析到了 scode"这种情况,又把问题带回来了。

**声明方用"仓库"(repoName + scope),不用命名空间字符串。** 词法作用域的边界就是
"recipe 所在的那个索引"。按现有数据,两种取法结果相同;用仓库不需要再额外判断一个
recipe 自己声明的 `namespace` 字段。

**边**:`PlanNode` 增加

```cpp
struct DepEdge {
    std::string spec;      // recipe 里的原文,如 "ncurses" / "xim:glibc@>=2.38"
    DepKind     kind;      // Runtime / Build
    std::string nodeKey;   // resolver 选中的节点的 key
};
std::vector<DepEdge> depEdges;
```

- `expand` 解析依赖时把选中节点的 key 写进边。
- `topoVisit` 直接读边,**删掉** `resolver.cpp:219` 的重新解析(A2)。
- A3 / A4 改成按 `nodeKey` 在 plan 里取节点,**删掉**两处按名字匹配的循环。
- A5 改成读已安装包的 `.xlings-resolution.json`(`installer.cpp:3350` 已经在写,里面有
  `name` = canonicalName)。没有记录的老安装,退回今天按名字比较的逻辑,会多报,但不会漏报。

结果是少了三处重复推导,只新增一个入口。

**行为变化范围(实测,2026-09-25 本机索引):**

| 仓库 | 不带前缀的依赖 | 结果会变的 |
|---|---|---|
| xim(275 个包) | `xmake→ncurses`、`project-graph→webkit2gtk`、`wsl-ubuntu→wsl@winget` | 只有 `xmake→ncurses`:从报歧义变成 `xim:ncurses` |
| scode / d2x / awesome / dsh | **一个依赖都没有声明** | 无 |
| `local:`(CI 新包、用户 `--add-xpkg`) | 按 PR 而定 | 不带前缀的依赖会**先查 `local`**,见下 |

`local:` 这一变化**正是想要的**:memory 里记录的那个"静默"失败——新包依赖一个已发布、
但本 PR 也改了的包,结果装的是已发布版本,被测的 recipe 根本没执行——在词法解析下,
不带前缀的依赖会拿到 PR 里改过的那份。注意:xim-pkgindex CI 固定用
`XLINGS_VERSION=v2026.8.8.2`,在它升级之前,索引 CI 看不到这个变化。

### W2 一个仓库只有一个身份(config / repo / doctor)

规则:在 {显式 `index_repos` 条目、已声明的子索引、`local`} 这三类里,**一个命名空间
只能对应一个仓库**。

> **2026-09-26 复查时发现,已定的 D2 需要修订。** `xlings index use <名字> <版本>` 只认
> `index_repos` 里的条目(`index_cmd.cpp:30`,`collect_index_sources` 只遍历
> `Config::global_index_repos()`)。所以要给 scode 这样的子索引**固定索引版本**,唯一的
> 办法就是把它显式加进 `index_repos`。按 D2 "写入时拒绝",这个用法会被一起挡掉。
> 下面是修订后的方案(D2',2026-09-26 维护者确认)。

修订方案(D2'):**名字和 URL 都和已声明的子索引一致的显式条目,不是第二个仓库,
而是对同一个仓库的配置**(用来固定版本、指定 artifact 等)。

| 情况 | 写入时(`--index-repo` / `add_repo`) | 已存在于配置中 |
|---|---|---|
| 显式条目 = 已声明的子索引(名字和 URL 都一样) | **接受**,并提示"这是对 xim 声明的子索引 `scode` 的配置,它仍然是子索引" | 按同一个仓库处理:**层级保持子索引,只同步一份**(不再额外同步到 `data/<name>`)。doctor 报告多出来的 `data/<name>` 是可回收的索引缓存,`--fix` 删除它(派生数据) |
| 名字一样、URL 不一样 | **拒绝**:两个不同的仓库不能都叫 `scode` | doctor 报告命名空间冲突;**不自动修**,交给用户选 |

和原 D2 的区别:原 D2 会删掉用户的显式条目,如果条目上写着固定版本,这个配置就丢了。
D2' 不删配置,只是不再让"注册方式"决定层级。这样两个症状都消失了:层级不会被抬高,
也不会同步两份。

- **"同一个仓库"必须只对应一个目录。** 现在 `Config::repo_dir_for`(显式条目 → `data/<name>`)、
  子索引同步(→ `data/xim-index-repos/<dir>`)、`collect_index_sources`(读 `installed_index_version`)
  三处各算各的目录。D2' 下这三处要给出**同一个**目录,显式条目上的 `version` / artifact
  设置作用在这一份同步上。否则 `index use scode <版本>` 固定的是一份,解析读的是另一份,
  又回到"两个回答者"。
- 加载配置时**不**改写用户配置文件。"显式条目按同一个仓库处理"是读取时的解释,不是写回。
- 写入时的判断、doctor 的报告、`--fix` 的执行,**用同一个判断函数**,避免"doctor 报了、
  修完还报"那类漂移。
- 对用户的可见变化:显式加了 scode 的用户,直接敲裸名(比如 `zlib`)会和没加 scode 的用户
  一样优先选 xim,不再报歧义。这恢复的是默认行为,不是新行为;用户确实想要 scode 的,
  写 `scode:zlib` 即可。

### W3 失败报告(resolver / commands)

- **只报一次**:失败按 (声明方, spec) 记录,同一个失败只推一次。另一种做法是解析失败时
  也写入 `color`,让第二张表直接跳过。
- **带依赖链**:`expand` 手里本来就有 `path`,报成
  `xim:xmake@3.1.1 → ncurses: ambiguous …`。NDJSON 的 ErrorEvent 增加一个 `chain`
  字段(新增字段,不破坏协议 v1.0)。
- **按层给补救**:
  - 用户直接输入的目标有歧义 → 和今天一样,让用户写全名。
  - 依赖层有歧义(W1 之后,只有"声明方仓库里没有这个名字,而两个同级仓库都有"才会出现)
    → 说明这是 recipe 的问题,指出是哪个 recipe、哪个索引,建议给依赖加上命名空间前缀;
    如果某个候选来自用户自己加的同级仓库,再给出 `xlings config --rm-index-repo <name>`
    作为另一种办法(D4)。**不再建议 `install <候选>`**,因为那样解决不了问题。

### W4 `noDeps`(capabilities)

NDJSON 的 `install_packages` / `plan_install` 在 schema 里写着 "Skip dependency installation",
但 `cmd_install` 从来不读这个参数。

已定(D3):`noDeps: true` 时**明确拒绝**,并从 schema 里删掉这一项。不真的实现它:
在形态 X 下跳过 glibc/ncurses,装出来的就是一个运行不了的包。

### W5 索引 CI 护栏(xim-pkgindex `check-dep-namespace.lua`)

- **豁免自动过期**:被豁免的名字一旦出现在 base 分支的 `pkgs/` 里,检查就失败。
  "发布后移除"这个条件机器可以判断,不需要靠人记。
- **冲突集合加上已声明的子索引**:用 `xim-indexrepos.lua` 里列出的子索引包名一起判断
  冲突。成本是 CI 里多拉 3 个小仓库。
- **"必须写前缀"的策略保持不变。** W1 只对新客户端生效,老客户端会长期存在,所以对 xim
  索引来说这条策略实际上是永久的。

---

## 4. 发布顺序与兼容

```
W0 (索引, 立即)  →  xlings 一个版本: W1 + W2 + W3 + W4 + 第二部分 S1–S9  →  W5 (索引 CI)
```

- **W0 先发**:它是老客户端唯一能拿到的修复,和其余几项互不依赖。
- **xlings 侧全部放进一个版本**(D5 已定)。代价是出了回归不容易定位到是哪一项,
  所以同一个 PR 里**按工作项分成独立的提交**,每个提交单独能编译、能跑测试,
  这样必要时可以 bisect。
- **不需要发布 libxpkg。** 两张表都有 `ncurses` 的问题在 xlings 这边去重就行;
  `resolved_dep` 的语义保持不变。这样就不用走"四个仓库依次发布"的流程。
- 老客户端的行为不变。新客户端遇到老索引(W0 之前的 xmake.lua)也能正常解析。

---

## 5. 验收标准

每一条都必须能执行、能复现,而且**在修复前是失败的**。

| # | 内容 | 修复前 |
|---|---|---|
| AC1 | **W1 单独验证**:fixture 里两个**真正同级**的仓库(不是已声明的子索引,这样 W2 帮不上忙)都有 `ncurses`,加上**没改过的** xmake.lua(裸名)→ `plan_install xmake` 解析出 `xim:ncurses@6.5` | 报歧义 |
| AC1b | **W2 单独验证**:用户的真实配置(显式 scode + xim),`plan_install xmake` 成功;`scode` 的层级是子索引;`xlings update` 之后不再同步 `data/scode` | 报歧义(腿 A);同步两份 |
| AC2 | 同一个 AC1 fixture 下 `plan_install zlib`(两个同级仓库都有)**仍然报歧义** | 报歧义(证明 W1 没有放宽全局规则) |
| AC2b | W1 规则 4:声明方仓库里有 `ncurses` 但没有要求的版本 → 报错并点名这个仓库,**不会**换到 scode | 按代码推断会换仓库 |
| AC3 | 默认配置,`install scode:ncurses xmake`:xmake 的 `.xlings-resolution.json` 里 `ncurses` 指向 `xim-x-ncurses/6.5` | **待实测**(代码推断会按 plan 顺序取) |
| AC4 | 依赖层失败只报一次,并且带 `xim:xmake@… → ncurses` 这条链 | 报两次,没有链 |
| AC5 | fixture 里构造一个依赖层歧义,执行报错给出的每一条补救命令,之后原目标能装上 | 补救无效(腿 C) |
| AC6 | `config --index-repo scode:<声明的 URL>` 被接受并提示"仍是子索引";之后 `xlings index use scode <版本>` 能固定版本;`config --index-repo scode:<别的 URL>` 被拒绝;doctor 报告多出来的 `data/scode`,`--fix` 删除它后 `install xmake` 和 `scode:ncurses` 都 rc=0 | 接受且不提示;层级被抬高;doctor 没有这一项 |
| AC7 | `plan_install` 带 `noDeps: true` → 明确报错 | 静默忽略(腿 E) |
| AC8 | 索引 CI:EXEMPT 里写一个 base 上已存在的名字 → 检查失败 | 通过 |
| AC9 | **plan 差分**:默认配置下,对 xim 的每个包分别用新旧两个二进制跑 `plan_install`,除 `xmake` 外结果逐字节相同 | —— |

AC9 是用来**实测**影响范围的,不靠推理:上面 §3 W1 那张表是扫描 recipe 得到的,
AC9 用真实的 plan 结果确认它。

测试放在哪里:
- W1:catalog 单元测试,fixture 是两个同级仓库都有 `libX`,R1 里的 `app` 用裸名依赖 `libX`。
  需要同时断言三件事:`app` 选到 `R1:libX`;用户直接输入 `libX` 仍然报歧义;R1 里没有
  `libX` 时退回全局规则。
- AC1–AC7:一个 e2e(`tests/e2e/dep_declarer_scope_test.sh`),把分析报告里的腿 A–G
  固化下来,并注册进 `run_all.sh`。

---

## 6. 不做什么

- **不改用户直接输入裸名时的层级模型**(一级 / 子索引 / `local` 降级)。那是另一个问题。
- **不把索引策略翻转成"推荐写裸名"**。原因是老客户端,见 W5。
- **不在加载配置时自动改写用户配置**。
- **不改 libxpkg**。

---

## 7. 已定的决定(2026-09-26 维护者评审)

| # | 问题 | 决定 |
|---|---|---|
| D1 | 依赖按声明方解析的强度 | **先查声明方所在的仓库**,找不到再走全局规则 |
| D2 | 已存在的重复注册 | ~~写入时拒绝;doctor 报告;`--fix` 删掉显式条目~~ → **修订为 D2'**(2026-09-26 确认) |
| D2' | 显式条目和已声明子索引名字、URL 都一致时 | **视为同一个仓库的配置**:层级保持子索引,只同步一份;URL 不一致才拒绝。原因:`index use` 固定子索引版本,只能通过显式条目来做(W2) |
| D3 | `noDeps` | **明确拒绝**,并从 schema 删掉 |
| D4 | 给人用的删除命令 | **新增 `xlings config --rm-index-repo <name>`** |
| D5 | 发布怎么拆 | **xlings 侧全部放一个版本**(连同第二部分),PR 内按工作项分提交 |

---

# 第二部分:SubOS 用户数据保护

## 8. 规则(2026-09-26 维护者确认)

> subos 里的**安装包数据**坏了,可以卸载、删除,因为它是固定的、能恢复的;
> subos 的 **home 目录以及用户产生的数据**,不能默认删除。
>
> 用户数据本来就在 `subos/<name>/` 下,不需要另外存一份。删除要有确认提示,
> 除非用户显式给了自动确认(`-y`)。不另加 `--purge`,只要保证删除确实是用户主动发起的。

写成可以直接实现的判断标准:

| 类别 | 内容 | 谁可以删 |
|---|---|---|
| **派生数据** | `data/xpkgs/` 下的 payload;subos 里指向 xpkgs 的 sysroot 链接;`bin/` 下的 shim;`generations/`;登记记录;索引缓存 | 任何流程,包括自动修复。重装一次或 `use` 一次就能恢复 |
| **用户数据** | `home/`、`home.img`(镜像模式);subos 里**任何不能证明属于 xlings 的普通文件**,包括在沙箱里写进 `usr/`、`etc/` 的文件(D9) | **只有用户主动发起的删除** |
| **易失数据** | `tmp/` | 随 subos 一起删 |

**"用户主动发起"的定义**,两个条件同时满足才算:

1. 执行的命令本身就是删除这个 subos 的:`subos remove`、`self uninstall`、`self install` 的覆盖分支;
2. 用户在终端上确认了,或者显式带了 `-y`(NDJSON 里是 `yes: true`)。

以下几种**都不算**,无论如何不能删用户数据:`doctor --fix` 及其子进程、升级(`self update`,
以及 `self install` 的普通路径)、创建失败时的回滚、GC、任何打印出来的补救建议。

---

## 9. 排查结果

### 9.1 结论

**没能复现"自动修复 / 升级 / `doctor --fix` 自动删除 subos home"。** 在隔离 home 里,
用 2026.9.20.1 对三种损坏的 subos 跑 `self doctor --fix`(配置 JSON 损坏、manifest 块缺失、
sysroot 链接悬空),三个 subos 的 home 数据都完好。代码里 `doctor --fix` 以子进程方式
执行的命令只有 `install`、`remove --force`、`use`(都作用在包上),以及对其他 subos
递归执行的 `self doctor --fix --subos <name>`。没有一条会删除 subos 目录。

但**能删掉 subos home 的路径有好几条**,其中几条正好会出现在"修复"或"agent 自动操作"
的过程里。另外,xlings **不记录任何删除操作**,所以过去那次丢失事后无法归因,
这本身就是要补的缺口(S8)。

你的 shell 历史里有这些删除相关的命令:9 月 6 日 `self clean`,9 月 7 日 `self doctor --fix`,
9 月 24 日 `self update`,以及多次 `subos remove`。但历史只有命令没有输出,
无法把丢失对应到其中某一条。

### 9.2 能删除 subos home 或用户数据的路径

"实测"都是在隔离 home 里用 2026.9.20.1 做的,真实 `~/.xlings` 没有被写入。

| # | 路径 | 证据 | 删的是什么 | 严重程度 |
|---|---|---|---|---|
| L1 | `xlings subos remove <name>`(CLI) | **实测** M1 | 整个 subos 目录**连同 `home/`**。**没有任何确认**(stdin 不是终端时也一样),exit 0,只打印 "subos removed",不说删了多少用户数据 | 高 |
| L2 | NDJSON `remove_subos {"name":…}` | **实测** M3 | 同 L1。agent 走的是这条路 | 高 |
| L3 | NDJSON `create_subos` / `remove_subos` **传空名字** | issue #611 | **整个 `subos/` 根目录,所有 subos 的 home**,两个调用都报成功 | 致命;2026.9.20.1 已修,**2026.9.16.1 及以前的客户端仍受影响** |
| L4 | 先"收养"再删除:`subos/<name>/` 在磁盘上存在但没有登记,`subos new <name>` 会**悄悄收养它**并报 "subos created";之后 `subos remove` 删掉的是 xlings 从来没创建过的数据 | **实测** M2 | 收养前就存在的 home | 高。**你的真实 home 现在就有 4 个这样的目录**(见 §13) |
| L5 | doctor 对"subos 配置读不了"给出的补救建议是 `` `xlings subos remove <name>` `` | **实测**(doctor 输出) | 照着做就会删掉 home。agent 会执行工具打印出来的补救命令 | 高 |
| L6 | `subos create` 校验失败时回滚 `remove_all(dir)`(`subos.cpp:764`),不区分目录是这次新建的还是之前就有的 | 代码 | 之前就存在的整个目录 | 潜在(`ensure_subos_info_` 会先修好 manifest,很难触发) |
| L7 | `self install`(`quick_install` 升级走的就是它,从临时解压目录执行时**不询问**)第 1 步:删掉 XLINGS_HOME 顶层**除 `data/`、`subos/`、`.xlings.json` 以外的所有条目**(`install.cpp:765-777`) | 代码 | 用户放在 `~/.xlings/` 下的任何东西(你的 home 里现在有 `verify-eco` 2.2G、`verify-953-src`),以及 `config/` | 中 |
| L8 | `self install` 重装同一版本时问 "overwrite data and subos?",回答 y 就删掉**所有** subos(`install.cpp:692`) | 代码 | 所有 subos 的 home。提示语没说会删掉用户数据,也没给大小 | 中(需要显式回答 y) |
| L9 | `self clean` 的 GC:只要有一个 subos 的配置读不了,它引用的 payload 就被判为可删(`profile.cpp`,`collect_subos_references_`) | **实测** G2(dry-run) | payload。属于派生数据,能重装,但这是"没读到当成空"的错误,全局配置读不了时会判定**所有** payload 可删(代码推断) | 中(能恢复) |
| L10 | `self clean` 按名字递归删除 `<XLINGS_HOME>/.xlings` | 代码 | 如果 XLINGS_HOME 被设成了 `$HOME`,删掉的就是整个 `~/.xlings` | 低 |

另外两条**不是**数据丢失,只作记录:
- 自定义目录的 subos(NDJSON `create_subos` 的 `dir`),`remove` 时算出的路径是
  `subos/<name>`,不是登记的那个目录。所以那个目录这次**碰巧没被删**,但留下了骨架文件
  (实测)。这又是"subos 在哪里"有两个回答者。
- 文档和 skill(`docs/quick-start/subos-and-agent.md`、`.agents/skills/xlings-usage`)都在教
  agent "用完就 `subos remove`",而 agent 正是在 subos 的 home 里干活的(你 home 里的
  `agent-other-p` 有 2.0G,`agent-influence` 有 193M)。在删除语义改掉之前,这条建议就等于
  "用完就删掉工作成果"。

### 9.3 根因

1. **删除不分数据类别。** 所有删除 subos 的代码都是对整个目录 `remove_all`,
   没有"派生数据"和"用户数据"之分(L1、L2、L3、L6、L8)。
2. **"这个目录是谁的"没有回答者。** `create` 只查配置里有没有这个名字,不查磁盘;
   `remove` 只看配置有没有登记,不看目录里有什么(L4、L6)。
3. **补救命令没有经过"会不会删用户数据"这一关**(L5)。
4. **"没读到"被当作"是空的"**(L9)。和 AGENTS.md 里 shim 表那一节写的是同一类问题。
5. **删除不留记录**,出了事没法追查。

---

## 10. 方案:S1–S9

### S1 删除 subos 必须由用户确认

- `subos remove <name>` 删除前先确认。确认内容包括:路径、home 的大小和文件数、home 之外
  不属于 xlings 的文件数(D9),以及"删除后无法恢复"。默认答案是 no:
  ```
  $ xlings subos remove dev-hello
    will delete ~/.xlings/subos/dev-hello
      home/              2.5 GB, 30256 files      user data
      other user files   12 files in usr/, etc/   user data
      managed by xlings  sysroot links, shims     rebuildable
    delete dev-hello and its user data? [y/N]
  ```
- 没人能回答(stdin 不是终端,或者 NDJSON 客户端不回复),又没带 `-y` → **拒绝**,
  什么都不改,exit 2,提示 "re-run with -y"。直接复用 `commands.cpp:85` 的
  `confirmed_or_refused_`(`remove` 包时用的就是它),把它提到共享模块里。
  `self install` 里另有一套 `confirm_`,也一并改成用它,不再出现第三套确认逻辑。
- `-y` 是现有的全局参数(`cli.cpp:1643`,"Skip confirmation prompts"),代表用户显式的
  自动确认。`subos remove` 现在不读这个参数,需要接上,做法和 `self uninstall` 一样。
- NDJSON `remove_subos` 增加 `yes` 参数(默认 `false`)。不带时发出 PromptEvent,客户端可以
  用已有的 `prompt-reply` 回答;没人回答就拒绝。**这对 agent 是一个行为变化**:以前一调用
  就删,以后必须显式传 `yes: true` 或者回答确认。要写进接口文档和发布说明。
- `self uninstall` 和 `self install` 的 "overwrite data and subos?" 用**同一段**确认内容
  (列出每个 subos 的 home 大小),`-y` 的规则也一样。

### S2 一个删除入口,"用户确认过"写进类型

- 新增 `subos::delete_subos(dir, const UserConfirmed&)`。`UserConfirmed` 只能由确认流程
  构造:终端上确认成功,或者读到了 `-y` / `yes: true`。自动流程拿不到这个对象,所以
  **从类型上就无法整体删除一个 subos**,不靠调用方自觉。
- **检查目标**:必须是 `<home>/subos/` 的直接子目录(或者登记过的自定义目录),不能是
  `subos/` 根目录,不能是 `current`,不能是符号链接。这一层从结构上挡住 #611 那一类问题,
  而不是只挡"名字为空"。
- **检查挂载**:读 `/proc/self/mountinfo`,目录下有**任何**活的挂载点就拒绝。现在只检查了
  镜像模式的 `.mountpoint`,要扩大到 bind mount。
- **不跟随链接**:遇到 junction 或符号链接只删链接本身,沿用 `xvm/commands.cpp:323` 的做法。
- **写删除记录**(S8)。
- 调用点:`subos.cpp:1496`(remove)、`self install` 的覆盖分支、`self uninstall`
  (后两者删除 subos 时逐个 subos 调用它,这样目标检查、挂载检查和删除记录都能覆盖到)。
  `subos.cpp:764` 的回滚**不用**这个入口,见 S3。
- CI 加一条 lint:除了这个函数,`src/` 里不允许对 subos 下的路径直接调用 `remove_all`。
- 自动流程能删的只有派生数据:指向 xpkgs 的链接、shim、声明过的资产。现有代码对这些
  已经是逐个 `fs::remove`,**不删普通文件**;把这一点写成不变量测试固定下来。

### S3 `create` 不再悄悄收养,回滚只撤销自己做过的

- `subos/<name>/` 已经存在但没有登记 → 按 S1 的方式确认:"目录已存在,里面有 N MB 数据,
  要收养它吗?",默认 no;没人回答又没带 `-y` 就拒绝。确认之后输出写 "adopted",
  不写 "created"。NDJSON `create_subos` 同样增加 `yes` 参数。
- 回滚只删这次运行**新建**的条目(运行时记下清单)。目录原本就存在的,绝不整体删除。
- doctor 报告未登记的 subos 目录和其中的数据量,补救建议是 `xlings subos new <name>`
  (它会提示收养)。`--fix` 不碰这类目录。

### S4 补救建议和 `--fix` 永远不删用户数据(D8 已定)

- "subos 配置读不了"的处理:`--fix` 把坏文件改名为 `.xlings.json.corrupt-<时间戳>`
  保留下来,重建一份最小配置(manifest 按 Describe 写,workspace 为空),home 不动,
  然后报告"这些包需要重新 `use`"。**补救文字里去掉 `subos remove`。**
- 不变量测试:doctor 打印的任何补救命令、`--fix` 执行的任何子命令,都不能是删除 subos 的命令。

### S5 GC:没读到就是"保留",不是"空"

`profile::gc`(`self clean` 调用它)和 `collect_subos_references_` 改用带 `unreadable`
输出的快照加载:
- 只要有一个 subos 的配置读不了,或者全局 versions DB 解析失败,就**拒绝 GC**,并列出
  是哪几个。按 AGENTS.md 的要求,拒绝是二元的,不设阈值。
- 从记录路径里提取 payload 目录时,按路径分量解析,不再用 `find("xpkgs/")`
  (Windows 的反斜杠路径会匹配不上)。

### S6 `self install`:只删自己发布的东西

- 第 1 步从"除了 data、subos、`.xlings.json` 全删"改成"**只删这个发布包里有的顶层条目**"。
  XLINGS_HOME 下用户自己放的东西不再被删。
- "overwrite data and subos?" 分支里的 subos 删除,用 S1 的确认内容,走 S2 的入口。

### S7 `self clean` 的旧缓存路径

`<XLINGS_HOME>/.xlings` 只有在它确实是旧版缓存布局,并且 XLINGS_HOME 不等于 `$HOME`
时才删,否则跳过并说明原因。

### S8 删除记录

每一次破坏性操作都往 `<XLINGS_HOME>/logs/destructive.ndjson` 追加一行:时间、xlings 版本、
argv、来源(CLI 还是 interface)、父进程名、**是怎么确认的**(终端 / `-y` / `yes:true`)、
路径、删除的字节数和文件数。覆盖范围:`subos remove`、`self uninstall`、`self install` 覆盖、
GC、`doctor --fix` 的删除和注销、`remove --force`。

之所以要做这一项,是因为这次排查查不出过去那次丢失是怎么发生的。有了记录,下次就能
直接查到是谁、用什么方式确认的。`self doctor` 可以顺带显示最近 7 天的破坏性操作次数。

### S9 文档、skill 和接口说明

- `docs/quick-start/subos-and-agent.md`、`xlings-usage` skill:讲清楚 `subos remove` 会删掉
  home 里的用户数据,需要确认;agent 代表用户删除时传 `yes: true`。
- `docs/spec/interface-ndjson-v1.md`:补充 `remove_subos.yes` 和 PromptEvent 的行为。
- AGENTS.md:加一段"subos 用户数据"规则,把 §8 那张表放进去。和版本号规则一样,
  这条规则要放在做决定的地方,不能只放在 skill 里。

---

## 11. 第二部分的验收标准

每一条在修复前都是失败的(或者按代码推断会失败,已注明)。

| # | 内容 | 修复前 |
|---|---|---|
| AC10 | `subos remove X`:没有终端也没带 `-y` → 拒绝,X 原样不动;NDJSON 不带 `yes` 且不回复 → 拒绝 | 直接删除(M1、M3) |
| AC11 | 确认内容包含 home 大小、文件数和 home 之外的用户文件数;带 `-y` / `yes: true` 后删除,并留下删除记录。`self uninstall` 和 `self install` 覆盖分支显示同一段内容 | 没有确认;覆盖分支的提示不说会删用户数据 |
| AC12 | 删除入口指向 `subos/` 根目录、`current`、`subos` 之外的路径、符号链接时,都被拒绝 | 只检查了空名字(#611) |
| AC13 | subos 目录下有活的 bind mount 时,`remove` 拒绝执行 | 只检查 `.mountpoint` |
| AC14 | 目录已存在但没登记时,`subos new X` 在没有确认时拒绝;带 `-y` 就收养,home 不变,输出写 "adopted" | 悄悄收养,还报 "created"(M2) |
| AC15 | doctor 遇到损坏的 subos 配置:`--fix` 之后坏文件被保留、配置被重建、home 不变;输出里不再出现 `subos remove` | 补救建议是 `subos remove` |
| AC16 | 有一个 subos 配置读不了时,`self clean` 拒绝 GC,并列出这个 subos | 判定 payload 可删(G2) |
| AC17 | 往 `XLINGS_HOME` 顶层放一个用户目录,`self install` 之后它还在 | 按代码推断会被删除 |
| AC18 | 上面每一个破坏性操作都在 `destructive.ndjson` 里有一行,确认方式和字节数都与实际一致 | 没有记录 |
| AC19 | CI lint:删除入口之外的代码对 subos 路径调用 `remove_all` 时检查失败;不变量测试:`doctor --fix` 在 sysroot 里只删链接,不删普通文件 | 没有这两项检查 |
| AC20 | `XLINGS_HOME=$HOME` 时,`self clean` 不删除 `$HOME/.xlings`,并说明原因 | 按代码推断会删除 |

全部写成 e2e(`tests/e2e/subos_user_data_test.sh`),在隔离 home 里跑,
并注册进 `run_all.sh`。

---

## 12. 第二部分不做什么

- **不做回收站或保留区**。维护者已确认:用户数据就在 `subos/<name>/` 下,删除靠确认把关。
- 不追溯恢复已经丢失的数据,也无从恢复。
- 不改镜像模式的存储格式。

---

## 13. 你现在的 home:建议马上做的事

排查时顺带看到的(只读,没有做任何修改):

| 目录 | 状态 | home 数据 |
|---|---|---|
| `subos/dev` | 目录存在,**配置里没有登记** | 21M,3725 个文件 |
| `subos/dev-2` | 同上 | 278M,5850 个文件 |
| `subos/dev-3` | 同上 | 21M,3725 个文件 |
| `subos/host` | 同上,没有 `home/` | — |

在修复发布之前:
- 当前版本的 `subos remove` **没有任何确认**,敲之前请再核对一遍名字。
- **不要对 dev、dev-2、dev-3、host 执行 `xlings subos new` 之后再 `subos remove`**,
  这正是 L4 的触发方式。需要的话,先把这几个 home 备份到 `~/.xlings` 之外。
- `~/.xlings/verify-eco`(2.2G)和 `verify-953-src` 放在 XLINGS_HOME 顶层,下一次
  `self install` 会删掉它们(L7)。如果还要用,请移走。

---

## 14. 第二部分的决定(2026-09-26 维护者评审)

| # | 问题 | 决定 |
|---|---|---|
| D6 | 删除时用户数据放在哪里 | **不另存**。用户数据本来就在 `subos/<name>/` 下;删除靠确认把关 |
| D7 | 要不要 `--purge` | **不加**。只要保证删除是用户主动发起的:终端确认,或显式 `-y` / `yes: true` |
| D8 | 配置损坏时 `--fix` 怎么处理 | **保留坏文件并重建最小配置**,home 不动 |
| D9 | 沙箱里写进 `usr/`、`etc/` 的普通文件算不算用户数据 | **算**。自动流程只删能证明属于 xlings 的东西;确认提示里要把这些文件计入 |

---

## 15. 实施记录(2026-09-26)

实现与方案的差异,以及原因:

| 项 | 方案 | 实际 | 原因 |
|---|---|---|---|
| W3 依赖链 | NDJSON ErrorEvent 新增 `chain` 字段 | 写进 `message`,沿用 `resolver.cpp` 已有的 `a -> b` 格式 | 协议结构不变,升级无感;agent 读 message 即可 |
| W5 第二项 | 冲突集合加入子索引包名 | **不做** | xim 自己提供的名字已被现有规则要求写前缀;xim 没有的名字在 W1 之后先在声明方仓库解析。真正的漏洞是过期豁免,已由"豁免自动过期"覆盖 |
| D6 回收站 | 保留区 | 不做(D6 定为不另存) | 用户数据留在 `subos/<name>/`,删除靠确认把关 |
| S1 agent 提示 | — | 不带 `yes` 时返回 exit 2 + hint "this needs the user's confirmation. Tell the user what would be deleted; …" | 维护者要求"返回一个提示词给 agent 让其知情" |
| S3 收养 | `--adopt` | 复用确认:终端确认或 `-y` / `yes:true` | 少一个参数,和删除同一套确认语义 |
| S2 共享模块 | subos.cpp 内部 | 新模块 `xlings.core.subos.userdata` | xself(`self install` / `uninstall`)也要用,而 subos 模块依赖 xself,不能反向 import |
| `self uninstall --keep-data` | 未在方案中 | 同时保留 `subos/` | 实现时发现:摘要写 "data: KEEP",却删掉所有 subos home |
| `self clean` 返回值 | 未在方案中 | GC 拒绝时 `self clean` 失败(exit 1) | e2e 发现:GC 拒绝后仍打印 "clean ok" 并 exit 0 |
| AC13(挂载) | e2e | e2e + 单元测试(mountinfo 解析) | CI/本机常不允许非特权 mount namespace;e2e 在那里明确 SKIP,解析逻辑由单元测试覆盖 |

跨仓库:xim-pkgindex PR #874(W0 + W5 豁免过期)。

## 附:顺带发现、不在本方案范围内的问题

- `xim:project-graph` 依赖的 `webkit2gtk` 在任何已配置的索引里都不存在,这个依赖在
  所有客户端上都无法解析。
- scode 索引的 `pkgs/n/ncurses.lua` 把模板代码整段重复了两遍,而且 `type` 先写 `"lib"`
  又改成 `"package"`。
- 把 home 复制一份做实验时,`.xlings-index-cache.json` 记录的是原 home 的 recipe 绝对路径,
  在副本里改 recipe 之前必须先删掉这个缓存(分析报告 §8)。
