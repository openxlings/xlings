# `xlings install xmake` 报 `ncurses` 歧义:分析报告

日期:2026-09-25 · 客户端:`xlings 2026.9.20.1` · 平台:linux x86_64

## 0. 结论

- **直接原因**:`~/.xlings/.xlings.json` 的 `index_repos` 里显式写了一条
  `scode`。`xim` 主索引本来就把 `scode` 声明为子索引,所以它现在注册了两次:
  一次作为子索引(次级),一次作为显式条目(和 `xim` 同一级)。xmake 的 recipe
  写的是不带命名空间的依赖 `"ncurses"`,这时两边各有一个候选,解析器拒绝替用户选。
- **算不算设计缺陷:算,但不在"拒绝猜"这一步。** 有多个候选时拒绝猜是
  2026.7.30.2 定下的约定,这一步本身是对的。缺陷在**提问的范围**:recipe 里的
  依赖名是按**终端用户配置的仓库集合**去解析的,不是按**声明它的 recipe 所在的索引**
  解析的。于是同一个 `xim:xmake` 的含义取决于用户加了哪些仓库,recipe 作者看不到、
  也测不到。
- **现在就能用的解法(已在隔离 home 里执行验证)**:删掉显式的 `scode` 条目。
  `scode` 仍然作为子索引可用,`scode:ncurses` 照样能装。
  ```
  xlings interface remove_repo --args '{"name":"scode"}'
  xlings install xmake
  ```
- **报错给出的补救命令不起作用**:按提示先装 `xim:ncurses@6.5`,再装 xmake,
  还是报同样的错(见 §2 腿 C)。

## 1. 现象

```
$ xlings install xmake
[error] package 'ncurses' is ambiguous, candidates:
        1. scode:ncurses@6.4 from global repo 'scode'
        2. xim:ncurses@6.5 from global repo 'xim'
        use one of: xlings install scode:ncurses@6.4 / xlings install xim:ncurses@6.5
[error] (同一段完整重复一遍)
```

当前目录 `~/test/mcpp/hello` 没有 `.xlings.json`,所以是全局作用域,和项目配置无关。

## 2. 复现(隔离 home,真实 home 未写入)

方法:把真实 home 的三个索引目录(`data/xim-pkgindex`、`data/scode`、
`data/xim-index-repos`)`cp -a` 到 scratch 目录,各 home 只有 `index_repos` 不同,
直接执行 entry binary(`~/.xlings/bin/xlings`,2026.9.20.1),用
`env -i HOME=$HOME PATH=/usr/bin:/bin XLINGS_HOME=<隔离>`。所有腿都用 `plan_install`,
这是 dry-run,不下载也不安装。跑完用 `find -newer` 确认真实 `~/.xlings` 没有任何文件被改。

| 腿 | `index_repos` | 目标 | 结果 |
|---|---|---|---|
| A | `scode` + `xim`(照搬用户配置) | `xmake` | **歧义,同一条错误出现 2 次**,rc=1 |
| B | 仅 `xim`(`scode` 只作为子索引) | `xmake` | `xim:glibc@2.44.3` + `xim:ncurses@6.5` + `xim:xmake@3.1.1`,rc=0 |
| C | 同 A | `xim:ncurses@6.5` + `xmake`(按报错提示做) | **仍然歧义**,rc=1 |
| D | 同 B | `scode:ncurses` | `scode:ncurses@6.4`,rc=0(子索引仍可达) |
| E | 同 A | `xmake`,`noDeps: true` | 仍然歧义,rc=1(`noDeps` 不生效,见 §5.4) |
| F | 同 A,但把 xmake.lua 的依赖改成 `xim:ncurses` | `xmake` | 解析成功,rc=0 |
| G | 从 A 出发,执行 `remove_repo scode` | `xmake` / `scode:ncurses` | 两个都 rc=0 |
| — | A 对比 B | `zlib`(用户直接输入的裸名) | A 歧义(只出现一次、带 hint);B 选 `xim:zlib` |

A/B 的差别只有配置里那一条 `scode`,所以这就是触发条件。F 说明索引侧改一行就能绕开。
G 是实际执行了推荐的补救命令,不只是从代码推断。

## 3. 根因

### 3.1 触发条件:同一个索引注册了两次,两次的优先级不同

- `data/xim-pkgindex/xim-indexrepos.lua` 把 `scode` 声明为子索引,URL 是
  `https://github.com/openxlings/xim-pkgindex-scode.git`,同步到
  `data/xim-index-repos/xim-pkgindex-scode`,加载时 `subIndex = true`
  (`src/core/xim/catalog.cpp:377`)。
- 用户配置里又有 `{"name":"scode","url":<同一个 URL>}`,走的是
  `Config::global_index_repos()`,在 `repo_specs_()`(`catalog.cpp:344`)里**不带**
  `subIndex`,也就是和 `xim` 同一级。它还单独同步到 `data/scode`,同一份内容下载两次。
- 裸名解析的优先级规则(`collect_matches_`,`catalog.cpp:512`)是:"只要有一级仓库
  命中,就不看子索引"。显式条目把 `scode` 抬成了一级,这条规则就不再把它压下去。
  之后的 `namespace_rank_`(`catalog.cpp:197`)只会降级 `local`,`scode` 和 `xim`
  打平,于是返回歧义(`catalog.cpp:818`)。
- `same_match_identity_`(`catalog.cpp:167`)按 `repoName` 去重,两份 `scode` 被合成
  一个候选。所以用户**看不到** scode 被注册了两次,报错里只写 "global repo 'scode'"。

**`scode` 的优先级由它是怎么被注册的决定,而不是由它是哪个仓库决定。**
写入这条显式条目的入口有两个:`xlings config --index-repo`(`src/cli.cpp:940`)和
NDJSON `add_repo`(`src/capabilities.cpp:435`)。两个都不检查这个名字或 URL 是不是
已经被声明成了子索引,写入时也不给任何提示。所以这不能算用户用错:配置接受了,
而且没有任何提示。这条条目是什么时候、由哪条命令写进去的,从现有状态查不出来。

### 3.2 机制(真正的设计问题):依赖名按用户的仓库集合解析

`resolver.cpp:65` 解析顶层目标,`resolver.cpp:179/182` 递归展开依赖时,调用的都是
`catalog.resolve_target(dep, platform)`。这个调用**不带任何"是谁声明了这个依赖"
的信息**。所以 `xim:xmake` 里的 `"ncurses"` 和用户在命令行上直接敲 `ncurses`
走的是同一个问题:"在用户配置的所有仓库里,ncurses 指哪个"。

后果有三个:

1. **recipe 的含义不归 recipe 作者控制。** xmake.lua 第 87–96 行的注释明确依赖一个
   前提:"bare names prefer primary repos … over the scode sub-index — so the bare
   form resolves correctly in every state this recipe meets"。这个前提在"scode 只是
   子索引"时成立,用户把 scode 显式加进来就不成立了。作者在 CI 里测不到这种状态。
2. **从语义上看,这两个候选根本不对等。** `scode:ncurses` 下载的是
   `ncurses-6.4.tar.gz` 源码包,解压后原样放进去,注册的是 `scode-ncurses`。它不提供
   `libncurses.so.6` / `libtinfo.so.6`,而 xmake 3.1.x 的 `DT_NEEDED` 需要的正是这两个。
   也就是说,歧义列表里有一个选项选了一定会错。
3. **优先级规则是写给命令行输入的,却被用到了 recipe 依赖上。** 对用户直接敲的
   `xlings install zlib`,在 scode 和 xim 同一级时报歧义是合理的(表中最后一行),
   因为这是用户自己的选择。对 `xim:xmake` 的依赖来说,用户加了哪些仓库和
   "xmake 需要哪个 ncurses" 无关。

### 3.3 为什么现在才暴露

- ncurses 是在 xim-pkgindex #582(`0853fefd`,2026-08-09)里新增的。同一个 PR 给 xmake
  加了裸名依赖,并在 `.github/scripts/check-dep-namespace.lua:72-75` 登记了豁免:
  `["pkgs/x/xmake.lua"] = { ["ncurses"] = "#582, new in this PR; remove once published" }`。
  这个检查文件自己的头注释说得很清楚:豁免只在"包还没发布"时正确,发布之后
  歧义规则就开始生效,而且 "An entry left behind is the bug this check exists to catch,
  wearing a permit"。**ncurses 早已发布,这条豁免已经过期 47 天,还留在 origin/main 上。**
- 这个检查只判断"裸名在**本索引**里是否存在",不看 `xim-indexrepos.lua` 声明的子索引,
  也不看第三方仓库。所以即使没有豁免,它也发现不了和 scode 的冲突。

## 4. 这是不是设计缺陷:逐条判定

| 问题 | 判定 | 理由 |
|---|---|---|
| 多个候选时拒绝猜 | **不是缺陷** | 2026.7.30.2 的 deterministic-or-refuse 约定,保留 |
| recipe 依赖按用户仓库集合解析,不按声明方的索引解析 | **设计缺陷(根因)** | recipe 的含义随用户配置变化,作者无法测试 |
| 同一个索引可以用不同优先级注册两次,而且不提示 | **设计缺陷(触发条件)** | 优先级由注册方式决定,不由仓库身份决定;还会重复同步 |
| xmake 裸名依赖 + 过期豁免 | **索引侧遗留 bug** | #582 自己写明"发布后移除",一直没移除 |
| 错误打印两次、没有上下文、补救命令无效 | **UX 缺陷** | 见 §5 |

## 5. 顺带发现的问题(都已核实)

1. **错误打印两次。** libxpkg 把扁平的 `deps` 同时展开进 `runtime_deps` 和
   `build_deps`(`libxpkg/src/xpkg-loader.cppm:303-304`)。resolver 对两张表各
   `expand` 一次(`resolver.cpp:179/182`),解析失败时不会记下这个 key
   (`resolver.cpp:70` 在着色之前就返回了),所以同一条错误被 `push` 两次,
   `commands.cpp:648` 又逐条发出。对照:用户直接输入的裸名(`zlib`)走另一条路径,
   只报一次,还带 `hint`。
2. **报错没有依赖链。** 用户敲的是 `xmake`,报错只提 `ncurses`,没有写
   `xim:xmake -> ncurses`。
3. **补救命令不能让用户达成目标。** `xlings install xim:ncurses@6.5` 能执行成功,
   但 `resolve_target` 不看安装状态,之后 `install xmake` 照样失败(腿 C)。另一个选项
   `scode:ncurses@6.4` 是源码包,满足不了 xmake。真正有效的补救办法是删掉重复注册
   (腿 G),而报错里一个字也没提。
4. **`noDeps` 不起作用。** NDJSON `install_packages` / `plan_install` 的 schema 写着
   `"noDeps": "Skip dependency installation"`(`src/capabilities.cpp:81`),但
   `cmd_install`(`src/core/xim/commands.cpp:291`)从来不读这个参数(腿 E)。调用方以为
   跳过了依赖,其实没有。这和本次故障无关,属于"没生效却看起来成功"那一类问题。
5. **`self doctor` 不报告"同一个索引注册了两次"。** doctor 代码里没有任何地方读
   子索引声明。
6. 次要:scode 仓库的 `pkgs/n/ncurses.lua` 把模板代码(`package.type = …` 到
   `uninstall`)整段重复了两遍,而且先写 `type = "lib"` 又改成 `"package"`。
   和这次故障无关,属于 scode 索引的内容质量问题。

## 6. 解决方法

### 6.1 用户侧,立即可用(已验证)

```
xlings interface remove_repo --args '{"name":"scode"}'
xlings install xmake
```

scode 仍然作为 `xim` 声明的子索引存在,`scode:<pkg>` 显式命名照常能用(腿 D、G)。
删掉后 `data/scode` 这份重复的同步目录就没有读者了,可以手动删除,不删也不影响功能。
目前没有面向人的 CLI 删除命令,只能用 NDJSON 的 `remove_repo`,或手动编辑
`.xlings.json`。

如果**有意**让 scode 和 xim 同级(比如想让 `xlings install zlib` 在两边之间歧义),
那在 §6.3 修好之前,用户侧没有办法让 xmake 的依赖绕开这个问题。只能等索引侧修复。

### 6.2 索引侧,最小修复(xim-pkgindex,一行,已验证)

- `pkgs/x/xmake.lua` 第 98 行:`deps = { "ncurses", … }` → `deps = { "xim:ncurses", … }`。
- 删掉 `.github/scripts/check-dep-namespace.lua:74` 那条过期豁免。
- 顺带把 xmake.lua 第 87–96 行"bare form resolves correctly in every state"的注释
  改掉。这个说法是错的,§3.2 已经证伪。

腿 F 证明:用户配置不变,只改这一行,就能解析成功。这是 check 文件自己要求做的收尾,
影响面只有一个 recipe。

建议再做一项检查改进,防止同类问题再出现:**豁免自动过期**。被豁免的名字一旦在
base 分支的 `pkgs/` 里存在,就判为失败。"发布后移除"这个条件是能机器判断的,
不需要靠人记得。另外,冲突检测的名字集合应该加上 `xim-indexrepos.lua` 里声明的
子索引。

### 6.3 xlings 侧,根本修复(建议,未实现)

**F1 按声明方解析依赖(根因)。** resolver 展开一个节点的依赖时,把该节点的
`namespaceName` / `repoName` 一起传给解析函数。对裸名依赖:先在**声明方所在的仓库**
里查,查到就用;查不到再退回现在的全局规则。

- 例子:`xim:xmake` 的 `"ncurses"` → 先查 `xim` → `xim:ncurses`,和用户配了什么无关。
- CI 的 `local:` 覆盖层不受影响,反而更自然:PR 改了 xmake,它就注册为 `local:xmake`,
  裸名 `ncurses` 先查 `local`。PR 同时改了 ncurses 就拿到被测的那份,没改就退回全局
  规则。这正是 check 文件里"新包用裸名"那条约定想要的效果,却不再依赖一次性的豁免。
- 显式前缀(`xim:glibc`)的语义不变。
- 要注意的边界:`topoVisit` 在 `resolver.cpp:219` 附近**重新**解析依赖来算边。它必须
  用和 `expand` 相同的上下文,否则两处算出不同的 key,边会被悄悄丢掉。那附近已经有
  一段注释在说这个问题("Pin exactly as expand() did")。最干净的做法是在 `expand`
  时把解析出来的依赖 key 记在节点上,`topoVisit` 直接读,不再重新解析。
- 测试:一个 fixture home,两个同级仓库都有 `libX`,仓库 R1 的 `app` 裸名依赖 `libX`。
  断言 `install app` 选 `R1:libX`,同时用户直接输入 `install libX` 仍然报歧义。
  **这两条要一起断言。** 只测前一条,会漏掉"把全局裸名规则也改宽了"这种回归。

**F2 注册一次,一个优先级。** `config --index-repo` / `add_repo` 写入时,如果名字或
URL 和某个已声明的子索引相同,要么拒绝,要么明确提示"这会把它提升为一级仓库,
裸名解析会因此改变"。另一种做法:把 URL 相同的显式条目视为同一个仓库,不重复同步。
`self doctor` 增加一项检查,报告"索引注册了两次"。

**F3 把报错改成用户能照着做的样子。**
- `plan.errors` 去重,或者在 `resolver.cpp:70` 失败时也记下这个 key,同一目标只报一次;
- 带上依赖链:`xim:xmake -> ncurses`;
- 如果歧义发生在依赖层,补救命令不应再是 `install <某个候选>`(那条路走不通)。应该
  指出是哪条仓库配置造成了平级,给出删除它的命令。

**F4 `noDeps`:** 要么真的实现,要么从 schema 里删掉。现在这样是答应了却没做。

F1 和 F2 互相不能替代:F1 修好之后,用户直接敲的裸名仍然会受重复注册影响
(比如 `zlib` 的优先级被悄悄改变);F2 修好之后,任何一个第三方同级仓库里出现
同名包,仍然能把官方 recipe 的依赖弄成歧义。

## 7. 影响面

- xim 和 scode 当前重名的包有 8 个:`libffi linux-headers ncurses openssl pango sqlite
  util-linux zlib`。xim 和 dsh 没有重名。
- xim recipe 里真正的裸名依赖只有 `xmake -> ncurses`、`project-graph -> webkit2gtk`、
  `wsl-ubuntu -> wsl@winget` 三个(正则粗扫,结构化结果应以
  `check-dep-namespace.lua --list` 为准)。其中目前只有 `xmake -> ncurses` 和 scode 冲突。
- 所以今天实际受影响的只有"显式注册了 scode,并且安装 xmake(Linux)"这一种情况。
  但只要 §6.3 F1 没做,任何一个同级仓库新增一个同名包,就能从外部把一个官方 recipe
  弄坏,而官方 CI 看不到。

## 8. 复现时遇到的一个坑

复制出来的 home 里,`.xlings-index-cache.json` 记录的是**真实 home 的 recipe
绝对路径**(`/home/speak/.xlings/data/xim-pkgindex/pkgs/x/xmake.lua`)。所以第一次
修改副本里的 xmake.lua 完全没有生效,还是读到了真实 home 的文件。删掉副本里的缓存
文件后才读到了副本。A/B 两条腿读的是同一批 recipe,对比结论不受影响;但以后凡是要
在副本里改 recipe 的实验,**都必须先删掉缓存**,否则改了等于没改。这个缓存也意味着
home 搬家后,在缓存重建之前可能读到旧位置的 recipe。本次没有展开核实这一点。
