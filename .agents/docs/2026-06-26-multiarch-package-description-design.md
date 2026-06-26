# 多平台 × 多架构 包描述方案 — 分析对比报告 + 设计提案

> 日期: 2026-06-26
> 范围: `xlings` (`src/core/xim`) · `xim-pkgindex` (xpkg `.lua` 格式) · `libxpkg` (共享加载器/运行时)
> 目标: 让"架构"成为 xpm 资源解析的一等维度，给出多个候选方案与推荐方案

---

## 0. TL;DR

- **现状**: xpm 资源解析的键只有 **平台**(`linux`/`windows`/`macosx`)。**架构不是解析维度**——`package.archs` 是纯元数据(从 V0 起从未被消费/校验)，唯一对架构敏感的路径是魔法字符串 `XLINGS_RES`，它把 `detect_arch_()` 拼进 URL，但**不带 per-arch sha256**、强制单一命名+托管约定。显式 `url=` 条目天然只能服务单架构。
- **后果**: 多个包的 `archs` 声明与实际 URL **不一致甚至错误**(如 `github-gh` 声明 `aarch64` 却把所有 URL 写死 `linux_amd64`，在 ARM 上必装错;`node` 声明 `x86_64` 但 macOS URL 是 `darwin-arm64`)。这是一个**真实的、会装错二进制**的缺陷，而非纯美学问题。
- **结论**: 在保持 V1 "静态 package 表" 约束与向后兼容的前提下，把架构提升为显式数据键。推荐方案 = **Cargo-binstall 的模板转义阀** + **Nix/Homebrew 的 fail-closed per-arch 哈希表** + **架构别名归一化** + **消费 `package.archs` 做准入校验**。

---

## 1. 现状分析(代码级证据)

### 1.1 解析键 = 平台,无架构层

xpm 的数据模型是两层映射:`xpm[platform][version] -> { url, sha256, ref }`。

- **平台探测**是编译期 `#ifdef`,只会返回 `linux`/`macosx`/`windows`/`unknown`
  (`installer.cppm:1886-1896`)。→ 这意味着 spec 文档里的 `ubuntu`/`archlinux`/`manjaro` 等发行版键**在安装路径上是死键**,永远不会被选中。
- **平台继承 `ubuntu = { ref = "linux" }` 实际不工作**:上游加载器只认 `inherits = "linux"` 字段(`xpkg-loader.cppm:250-254`),而 `xpm.inherits` 在 xlings 里**从未被消费**(resolver/installer 都不跟随继承链)。文档写的 `ref="linux"` 形态甚至会被误解析成一个名为 `"ref"`、`url="linux"` 的假版本条目。
- 唯一真正生效的 `ref` 是**版本级别**别名 `["latest"] = { ref = "1.0.0" }`
  (`installer.cppm:1022-1029`, `catalog.cppm:114-130`)。
- 注意:存在**两套并行加载器**解析同一份 xpm —— 上游 `libxpkg/src/xpkg-loader.cppm` 与 xlings 内自带的 Lua 沙箱解析器 `installer.cppm::load_platform_entries_`(831-920),下载路径实际用后者。任何 schema 改动需同步两处。

### 1.2 架构处理 —— 核心缺口

- `detect_arch_()`(`installer.cppm:123-140`)能给出 `x86_64`/`aarch64`/`arm64`(Apple)/`x86`,但**全代码库仅被一处调用** —— `XLINGS_RES` 的 URL 拼装(`installer.cppm:160`)。
- `package.archs` 解析一次进 `Package::archs`(`xpkg.cppm:88`),**之后再无任何读取**。没有任何 "host 架构不在 archs 列表则拒绝安装" 的校验 —— 装错只会以 URL 404 形式延迟暴露。
- **同平台+同版本能否对 x86_64/aarch64 给不同 URL?**
  - 显式 `url="..."`:**不能**。一个 version 键 → 一个资源,作者只能把架构手写进那唯一的 URL 串。
  - `"XLINGS_RES"`:**隐式能** —— 生成 URL 内嵌 `detect_arch_()`,这是**唯一**的多架构通路。
- Lua 沙箱里 `os.arch` 被**硬编码常量**(`installer.cppm:786` 默认返回 `'arm64'`;`xpkg-loader.cppm:50` 返回 `'unknown'`),所以连 hook 里都无法在解析期可靠地按架构分支。

### 1.3 `XLINGS_RES` 的真实形态(既是优点也是天花板)

```
{server}/{pkg}/releases/download/{version}/{pkg}-{version}-{platform}-{arch}.{ext}
                                                          ↑os      ↑detect_arch_()  ↑zip(win)|tar.gz
```
(`installer.cppm:148-162`)

它**已经是一个隐式模板**(template),证明 "模板法" 与本项目天然契合。但三个硬伤:
1. **不带 sha256** —— per-arch 完整性校验缺失(供应链风险)。
2. **强制托管约定** —— 必须放在 res-server 且文件名严格遵循该模式,无法直接指向上游 GitHub release 的任意命名。
3. **无法 per-arch 覆盖** —— 某个架构若命名不规则就只能整体放弃 XLINGS_RES。

### 1.4 已有的"区域(mirror)"维度 —— 可复用的先例

`url = { GLOBAL = "...", CN = "..." }` 已是一个**声明式的、键控的多源**结构,优先级 `GLOBAL > CN`,其余转为 fallback(`installer.cppm:891-907, 1038-1065`)。这给"再加一个架构维度"提供了**现成的、已被验证的工程范式**:键控 map + 默认键 + fallback。

### 1.5 Spec 演进佐证

- **V0**: `archs` 元数据 + `xpm[platform][version] = {url, sha256}`(位置式)。
- **V1**(`spec="1"`): 强制 **package 表静态可求值**(只许字面量 + `string.format`)、libxpkg 标准库、mirror 表、`XLINGS_RES`、`os.host()/os.arch()` prelude。

→ 架构**从未**是解析维度;V2 引入它是自然延续。**关键约束**:package 表必须保持静态 —— 所以架构维度必须是**声明式数据键**,不能是运行期函数逻辑(这恰好排除了 xmake 的 `if is_arch()` 路线)。

---

## 2. 横向调研对比(Nix / Cargo / xmake / Homebrew / Bazel)

两种"平台标识"哲学:Nix/xmake 用 **2 段 `arch-os` 元组**;Cargo 用 **4 段 LLVM 三元组** `arch-vendor-os-abi`(把 libc/ABI `gnu|musl|msvc` 编进键里)。这个选择会级联影响资源键控、fallback、准入声明的形态。

| 系统 | 元组格式 | 资源(URL+hash)键控方式 | 声明式 vs 命令式 | "支持平台"声明 |
|---|---|---|---|---|
| **Nix/Nixpkgs** | 2 段 `arch-os`(`aarch64-darwin`),求值期 elaborate 成谓词 attrset | 静态 attrset 按 `system` 元组键 → `fetchurl{url,hash}`;`.${system}` 查找,`or throw` | **声明式数据 map**(语言允许命令式,但社区强约定) | `meta.platforms` 独立列表 |
| **Cargo/binstall** | 4 段三元组 `arch-vendor-os-abi` | 单个 `pkg-url` **模板** + `{target}/{arch}/{version}` 占位符,加 per-triple/`cfg()` 覆盖表 | **声明式,模板化**(URL 由三元组插值算出) | `[metadata.dist].targets` / `overrides.<triple>` 键集合 |
| **xmake/xrepo** | `plat` + `arch` **两条独立轴**,无组合串 | `if is_arch()` 分支,各自 `set_urls`+`add_versions(ver→sha256)` | **命令式**(Lua 控制流,运行脚本才知道) | 由分支隐式涌现;可选 `set_plat` |
| **Homebrew** | 融合 `arm64_<codename>` / `x86_64_linux`(OS版本×arch) | `bottle do` 块内扁平 `sha256 tag: "..."` map | **声明式** | bottle 标签集合 |
| **Bazel/Bzlmod** | `constraint_value` 集合(os+cpu) | `select({config_setting:...})` + `http_archive{sha256}` | **声明式约束匹配** | `platform()` target + toolchain 解析 |

### 关键设计教训(可直接迁移到 xlings)

1. **结构化存 arch/os,而非裸串**:把规范串解析成 `{arch, os, libc}` 并暴露**谓词**(Nix `isAarch64`、Cargo `cfg()`),作者匹配字段而非脆弱子串。**保留一份归一化内部表示并接受别名**(`arm64↔aarch64`、`darwin↔macosx`)—— 这正是 xlings 当前 `detect_arch_()` 已在 OS 间分裂(Linux=`aarch64`/Apple=`arm64`)所急需的。
2. **优先声明式键控 map,而非命令式分支**:Nix `sources.${system}`、Homebrew bottle 块对工具可枚举(可预取全部 hash、可自动更新);xmake 的 `if is_arch()` 不执行 Lua 就不可见。**这条直接否决 xmake 路线**(也与 V1 静态表约束冲突)。
3. **但要给"模板转义阀"**:binstall 的 `pkg-url + {target}` 让一行覆盖 N 个目标,远少于 N 条 attrset。xlings 的 `XLINGS_RES` 已经是这个思路 —— 应**显式化、可定制化**它。
4. **校验和必须 per-(tuple×version) 强制**:Nix/Homebrew/Bazel 都把 hash 绑死到每个 artifact;binstall 弱依赖签名是反面教材。**schema 应让"加一个架构产物却不带 hash"成为不可能**——补上 `XLINGS_RES` 缺失的 per-arch sha256。
5. **"支持平台列表"与"字节在哪"分离,但由后者派生前者**:Nix `meta.platforms` 与 `sources` 会漂移,解法是 `platforms = attrNames sources`。→ xlings 应**由 xpm 资源键自动派生 `archs`**(或据此校验),消除双真源。
6. **显式 fail-open vs fail-closed**:Nix 对未列元组 `throw`(fail-closed,可预测);binstall 乐观模板化再回退编译(fail-open)。→ 预编译二进制**默认 fail-closed**,绝不隐式把 x86 二进制发给 ARM 主机;回退链(mirror→源码构建)须**显式声明**。
7. **per-artifact 多源 fallback 一等公民**:xmake `add_urls(primary, mirror)`、Bazel `urls=[...]` 让一个 hash 背多个源。xlings 的 mirror 表已具雏形,应推广为 `{ urls:[...], sha256 }`。
8. **预留 build/host/target 三分**:Nix 的三分是唯一能严谨表达交叉编译的。即便先只做 host-only,也应**预留词汇**("运行于"vs"构建于"),避免日后从单一 `platform` 字段痛苦改造。

---

## 3. 候选方案(共 4 个)

所有方案都满足硬约束:① package 表静态可求值(无运行期函数);② 向后兼容(老的 `xpm[platform][version]={url,sha256}` 不动仍可用);③ per-arch sha256 可表达;④ 架构别名归一化。

### 方案 A —— 平台下增设架构子层(Nix/Homebrew 纯嵌套)

`xpm[platform][arch][version] -> resource`

```lua
xpm = {
  linux = {
    x86_64 = {
      ["latest"] = { ref = "2.86.0" },
      ["2.86.0"] = { url = ".../gh_2.86.0_linux_amd64.tar.gz", sha256 = "..." },
    },
    aarch64 = {
      ["latest"] = { ref = "2.86.0" },
      ["2.86.0"] = { url = ".../gh_2.86.0_linux_arm64.tar.gz", sha256 = "..." },
    },
  },
}
```
- ✅ 最直白、最声明式、per-arch hash 天然;工具可枚举。
- ❌ **破坏式**:与现有 `xpm[platform][version]` 两层结构歧义(解析器要区分"第二层键是 arch 还是 version")。版本多时**冗余爆炸**(每架构重抄一遍版本表)。

### 方案 B —— version 条目内嵌架构映射

`xpm[platform][version] -> { [arch] = resource, ... }`

```lua
linux = {
  ["latest"] = { ref = "2.86.0" },
  ["2.86.0"] = {
    x86_64  = { url = ".../linux_amd64.tar.gz", sha256 = "..." },
    aarch64 = { url = ".../linux_arm64.tar.gz", sha256 = "..." },
  },
}
```
- ✅ 版本仍是主键(契合"多版本共存"心智);per-arch hash 清晰;`ref` 行为不变。
- ✅ 解析器只需判断 version 条目的 value 是 `{url=...}`(老式单架构)还是 `{x86_64=...}`(新式多架构)——**可平滑共存**。
- ❌ 仍需逐架构列 URL,规则化命名时有重复(用方案 C 的模板缓解)。

### 方案 C —— 模板 + 占位符 + per-arch sha256(Cargo-binstall,`XLINGS_RES` 的显式泛化)

```lua
linux = {
  ["latest"] = { ref = "2.86.0" },
  ["2.86.0"] = {
    -- 一行模板覆盖所有架构;${...} 由框架在解析期替换
    url = "https://github.com/cli/cli/releases/download/v${version}/gh_${version}_linux_${arch_alias}.tar.gz",
    -- 强制:每架构一条 sha256(fail-closed:未列架构 = 不支持)
    sha256 = {
      x86_64  = "aaaa...",
      aarch64 = "bbbb...",
    },
    -- 可选:架构 token 别名(上游用 amd64/arm64,xlings 内部用 x86_64/aarch64)
    arch_alias = { x86_64 = "amd64", aarch64 = "arm64" },
  },
}
```
占位符词表(建议):`${version}` `${os}`(linux/macosx/windows)`${arch}`(归一化 x86_64/aarch64)`${arch_alias}`(经 `arch_alias` 映射)`${ext}`(自动 zip/tar.gz)。

- ✅ **最少重复**:一行 URL 模板 + 一张 hash 表覆盖 N 架构。
- ✅ 把 `XLINGS_RES` 升级为同一机制的特例:`"XLINGS_RES"` ≡ `url="${server}/${name}/releases/download/${version}/${name}-${version}-${os}-${arch}.${ext}"`,只是**补上了 sha256 表**。
- ✅ 天然 fail-closed:`sha256` 表里没有的架构即不支持。
- ❌ 模板可读性稍弱;不规则命名仍需配 `arch_alias` 或退回方案 B 的逐架构 `url`。

### 方案 D —— 最小侵入:默认条目 + 可选架构覆盖表(混合,渐进)

保持老结构当作**默认/主架构**,仅对需要分化的版本加一张可选 `archs` 覆盖表:

```lua
linux = {
  ["latest"] = { ref = "2.86.0" },
  ["2.86.0"] = {
    url = ".../gh_2.86.0_linux_amd64.tar.gz",  -- 默认(= x86_64)
    sha256 = "aaaa...",
    archs = {                                   -- 可选:仅覆盖差异架构
      aarch64 = { url = ".../gh_2.86.0_linux_arm64.tar.gz", sha256 = "bbbb..." },
    },
  },
}
```
- ✅ **零破坏**:不写 `archs` 的包行为与今天完全一致;迁移成本最低。
- ✅ 解析:host arch 命中 `archs[arch]` 则用之,否则用默认。
- ❌ "默认条目到底是哪个架构"语义含糊(隐式 = 主架构),与教训 #6(禁止隐式 arch fallback)轻微抵触——需规定:**默认条目必须配合顶层 `archs` 列表声明其适用架构**,否则 fail-closed。

---

## 4. 推荐方案

**采用 B + C 融合,以 D 的兼容策略落地,分三步走。**

理由:B 提供清晰的 per-version/per-arch 结构与平滑共存(解析器靠 value 形状判别新老),C 提供消除重复的模板转义阀并把 `XLINGS_RES` 收编为特例,D 提供零破坏的渐进迁移路径。三者共享同一套底层语义:**version 条目的 value 可以是 ①老式单架构 `{url,sha256}`、②架构 map `{[arch]={url,sha256}}`(方案 B)、③模板 `{url=tmpl, sha256={[arch]=...}}`(方案 C)**。解析器按形状分派,互不冲突。

### 4.1 统一的 version-entry 解析规则(伪代码)

```
resolve(entry, host_arch):
  if entry.ref:                      return follow_ref(entry.ref)        # 版本别名,不变
  if entry == "XLINGS_RES":          entry = expand_xlings_res_template() # 收编为模板 (4.3)
  if entry.url is string and not entry.sha256[*]:   # ①老式单架构
      require host_arch in package.archs  # fail-closed 校验 (4.4)
      return { url: entry.url, sha256: entry.sha256 }
  if entry.url is template string:                   # ③模板 (方案 C)
      a = normalize(host_arch)
      if a not in entry.sha256:  fail("unsupported arch")   # fail-closed
      return { url: interpolate(entry.url, {version, os, arch:a, arch_alias, ext}),
               sha256: entry.sha256[a] }
  if entry[host_arch] or entry[alias(host_arch)]:    # ②架构 map (方案 B)
      return entry[matched_arch]   # {url, sha256}
  fail("arch not provided for this version")          # fail-closed,不隐式回退
```

### 4.2 架构归一化(必做,教训 #1)

引入单一归一化表 + 别名,**消除 `detect_arch_()` 跨 OS 分裂**带来的作者困惑:

| 归一化(内部规范) | 接受的别名 |
|---|---|
| `x86_64` | `amd64`, `x64`, `x86-64` |
| `aarch64` | `arm64`(Apple/上游常用), `armv8` |

- `xpm` 的架构键与 `package.archs` 一律用**归一化名**(`x86_64`/`aarch64`)。
- 模板里若上游用别名(`amd64`/`arm64`),通过 `arch_alias` 局部映射;`${arch}` 给归一化名、`${arch_alias}` 给映射后的名。

### 4.3 `XLINGS_RES` 升级(向后兼容)

- 现有 `["x"] = "XLINGS_RES"` 继续可用,等价展开为方案 C 模板(`installer.cppm:148-162` 已有逻辑)。
- **新增**:允许带校验的形态
  `["x"] = { res = true, sha256 = { x86_64 = "...", aarch64 = "..." } }`,
  使官方 res-server 包也能 fail-closed + 完整性校验,补上当前 `XLINGS_RES` 无 sha256 的硬伤。

### 4.4 消费 `package.archs`(准入校验,教训 #4/#5)

- 安装前:若 `host_arch ∉ package.archs` → 明确报 `unsupported architecture: aarch64 (supported: x86_64)`,而非延迟到 404。
- 一致性:CI 校验 `package.archs` 必须 = 各 xpm 资源键并集(由资源派生,消除双真源)。这能**立刻抓出现有的 `node`/`github-gh` 等错误声明**。

### 4.5 顺带修复(非架构,但同属 xpm 解析债)

- **平台继承**:让 `ubuntu = { ref = "linux" }` 真正生效(resolver/installer 跟随平台 ref 链),或从 spec 删除这个不工作的承诺;同时让 `detect_platform_()` 能产出发行版键或显式声明发行版键不参与 install 解析。
- **预留 host/target 词汇**(教训 #8):本轮只做 host-only,但在 schema 里给资源键预留可选 `target` 维度位,避免日后交叉编译再破坏式改造。

### 4.6 实现改动点(落地清单)

| 改动 | 位置 |
|---|---|
| version-entry 形状分派(①②③) | `installer.cppm::load_platform_entries_`(831-920) + 上游 `xpkg-loader.cppm`(166-332) **两处同步** |
| 架构归一化表 + 别名解析 | 新增工具函数,替换/包裹 `detect_arch_()`(`installer.cppm:123-140`) |
| 模板插值器(`${version}/${os}/${arch}/${arch_alias}/${ext}`) | 泛化 `build_xlings_res_url_`(`installer.cppm:148-168`) |
| per-arch sha256 存取 | 扩展 `PlatformResource`/`xpm.entries` 资源结构(`type.cppm` 的 `DownloadTask` 已有 `sha256`/`fallbackUrls`,够用) |
| `package.archs` 准入校验 | resolver 解析后、下载前;消费 `Package::archs`(`xpkg.cppm:88`) |
| CI 一致性检查 + 现存包修正 | `xim-pkgindex` 侧脚本;先修 `node`/`github-gh`/`cmake` 等 |
| spec 文档 V2 + 模板更新 | `xim-pkgindex/docs/V2/xpackage-spec.md`(从 V1 派生) |

---

## 5. 决策摘要

- **不要**走 xmake 的 `if is_arch()` 命令式路线 —— 违反 V1 静态表约束且工具不可枚举。
- **要**走 Nix/Homebrew 的声明式键控 + Cargo 的模板转义阀 + fail-closed + 强制 per-arch hash。
- **落地顺序**:① 架构归一化 + `package.archs` 校验(立刻抓错,低风险)→ ② 方案 B 架构 map(新包可用,老包不动)→ ③ 方案 C 模板 + `XLINGS_RES` 带 sha256 升级(减重复、补完整性)。
- **一个判据贯穿全程**:schema 必须让"新增一个架构产物却不带其 sha256"成为不可能。
