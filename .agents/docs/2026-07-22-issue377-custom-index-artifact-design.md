# 用户自定义 index 的 artifact 支持(git 回退)— 设计方案

**日期**: 2026-07-22
**类型**: 设计 (design)
**关联**: openxlings/xlings#377(自定义 index 支持 artifact)、openxlings/xlings#378(compact::git CA 环境)、
`2026-06-25-index-ecosystem-unification-plan.md`(该轮明确把"用户自定义仓"留作后续 —— 本设计即补上这一环)、
`2026-06-22-index-as-resource-impl-plan.md`(Y-asset 机制)

---

## 0. TL;DR

**核心理念:把 artifact 来源从"官方索引的硬编码特权"降级为"任何 IndexRepo 都可声明的属性"。**

现有 artifact 管线(pointer → manifest → sha256 → 原子替换 → `.xlings-index-version`)本身是
来源无关的;真正硬编码官方的只有两处 —— URL 拼装(全局单仓 `xim-index`)和准入判定
(`isDefaultOfficial`)。因此本设计不新建机制,只做一件事:给 `IndexRepo` 增加一个可选的
`artifact` 来源声明,把这两处从"常量"参数化为"每仓属性"。

```jsonc
"index_repos": [{
  "name": "mcpplibs",
  "url":  "https://github.com/mcpp-community/mcpp-index.git",   // git 模式 / 回退
  "artifact": "https://github.com/xlings-res/mcpp-index",       // 新增,可选;string 或 {"GLOBAL":..,"CN":..}
  "source": "auto"                                              // 新增,可选;auto(默认)| artifact | git
}]
```

未声明 `artifact` 的仓行为逐字节不变(纯 git);官方索引路径零改动。
配套项(#378):`compact::git` 调 git 前补一次 CA bundle 探测,让 git 回退在 Debian/Ubuntu 上真正可用。

---

## 1. 深度分析:问题与现状

### 1.1 #377 — 自定义索引被锁死在 git

两处硬编码共同造成(根因是第二处,布尔判定只是表象):

1. **准入判定**(`repo.cppm` `sub_should_attempt_artifact`):三条分支全部要求
   `isDefaultOfficial`,自定义索引连显式 `XLINGS_INDEX_SOURCE=artifact` 都无法覆盖,
   用户没有任何配置手段。
2. **URL 拼装**(`indexfetch.cppm` `index_asset_urls` / `index_pointer_urls`):所有 artifact
   URL 都从 `Config::resource_server()` + 固定仓名 `xim-index` 拼出,仅靠文件名区分索引。
   自定义索引的 artifact 在别人的仓库下,现有代码无从表达。

另有两处 #377 未点名但必须覆盖(mcpp 实测定位):

- **global 主列表的非官方仓**:mcpp 以 `XLINGS_HOME=~/.mcpp/registry` 运行 xlings,registry 的
  `.xlings.json` 是 xlings 的 *global* 配置;官方 `xim` 默认仓被前插到列表头
  (`config.cppm` 的 defaults-prepend),`mcpplibs` 作为主列表非首位仓走
  `syncRepos(global)` → `sync_repo`(纯 git)。这才是 issue 报错日志的实际路径。
- **project 级 index_repos / 子索引循环**:mcpp 的 per-project `[indices]` 会 seed 出项目
  `.mcpp/.xlings.json`,走 `sync_all_repos` 的 project 分支 —— 同样只有 `sync_repo`,
  连 artifact 尝试的入口都没有。

后果:自定义索引永远享受不到 0.4.49 的 adaptive mirror reorder + stall watchdog,且被迫依赖
一个可用的 git —— 同一次 update 里"主索引 artifact 成功、自定义子索引 git 失败"并排出现。

### 1.2 #378 — git 回退本身在 Debian/Ubuntu 上必失败

linux `xim:git` 静态构建的 OpenSSL 以 `OPENSSLDIR=/etc/ssl` 编译,默认 CA 文件为
`/etc/ssl/cert.pem`(BSD/Alpine 布局),Debian/Ubuntu/RHEL 上不存在 → 所有 HTTPS git 传输失败。
xim-pkgindex#406 通过 xvm `envs` 注入 `GIT_SSL_CAINFO` 修好了 **git 作为用户 CLI** 的场景,
但 `compact::git` **直接调二进制、绕过 xvm shim**,注入的 env 到不了它(`git.cppm` 全文无任何
`GIT_SSL_*` 处理)。实测四种覆盖方式中唯一有效的是 `GIT_SSL_CAINFO`(唯一能改写 curl
`CURLOPT_CAINFO` 的入口)。

两个 issue 互补:#377 让索引同步**不必**依赖 git;#378 让它确实依赖 git 时**不要挂掉**。
即使 #377 落地,bootstrap、无 artifact 声明的自定义索引、显式 `source: git` 仍走 git。

### 1.3 生产端已就绪 — xlings-res/mcpp-index v2e23e20 逐项核实

对 release `v2e23e20` 与仓内 `mcpp-index-pointers.json` 的实测结论:**发布布局与 xlings 现有
消费端预期 100% 兼容,消费端是唯一缺口**。

| 项 | mcpp-index 现状 | xlings 消费端预期 | 兼容 |
|---|---|---|---|
| pointer 文件 | 仓根 `mcpp-index-pointers.json`,raw 可取 | `<repo>-pointers.json` raw 拉取(`index_pointer_urls`) | ✅ |
| pointer 格式 | `{"format_version":1,"indexes":{"mcpp":{...}}}` | `load_index_pointers` 同构 | ✅ |
| manifest 字段 | format_version/index_version/index_name/generated_at/source_commit/artifact{name,sha256,size}/signature | `parse_index_manifest` 全部字段一致(signature 预留位也在) | ✅ |
| release tag | `v<短sha>`(如 `v2e23e20`) | `index_asset_urls` 版本化 tag 优先(`v<index_version>`) | ✅ |
| asset 命名 | `mcpp-index-2e23e20.tar.gz` = manifest `artifact.name` | 按 manifest 的 `artifact_name` 下载 | ✅ |
| sha256 | pointer 值与实测下载一致(`05593d4e…`) | manifest 钉住 sha256 校验 | ✅ |
| tarball 布局 | 根部含 `pkgs/`(另有 index.toml/README) | 解压后 `stage/pkgs` 存在性检查 | ✅ |
| 镜像 | 仓描述 "GLOBAL mirror",由 mcpp-community CI 发布;CN 侧可对称部署 gitcode.com/xlings-res/mcpp-index | 每仓 artifact 来源支持 region 对象 | ✅ |

无 `latest` rolling tag —— 不影响:manifest 带 `index_version`,版本化 tag 先试,`latest`
回退 404 后 fallthrough(`download_candidates_` 对 404 继续尝试,正是为此设计)。

---

## 2. 设计

### 2.1 配置(用户面)

`IndexRepo` 条目(global 与 project `.xlings.json` 的 `index_repos`)新增两个可选字段:

- **`artifact`** — artifact 来源 base。`string`,或 region 对象 `{"GLOBAL": "...", "CN": "..."}`
  (与 `xim.index-base` 的既有形态一致,按当前 mirror 解析、GLOBAL 兜底)。
  base 的最后一个路径段即 artifact 仓名(如 `.../xlings-res/mcpp-index` → `mcpp-index`)。
- **`source`** — 该仓的传输模式,覆盖全局 `XLINGS_INDEX_SOURCE`:
  - `auto`(默认):声明了 `artifact` 则 artifact 优先、失败回落 git;未声明则纯 git。
  - `artifact`:只走 artifact,失败即报错(不回退 git;与主索引
    `XLINGS_INDEX_SOURCE=artifact` 语义一致)。
  - `git`:强制 git,忽略 `artifact` 声明。

不设 `tag` 字段(#377 提议中有):版本化 tag 从 pointer 的 `index_version` 推导
(`v<index_version>`),pointer 本身就是版本发现机制,再配 tag 是冗余(YAGNI)。

### 2.2 发布方契约(producer contract)

第三方索引发布方(mcpp 已全部满足,零改动)需提供:

1. base 仓根部一个 raw 可取的 **`<artifact仓名>-pointers.json`**,格式
   `{"format_version":1,"indexes":{"<key>":{<manifest>}}}`;
2. 每个版本一个 release,tag **`v<index_version>`**,附 manifest 所写的 tarball asset;
3. tarball 根部含 **`pkgs/`**;
4. manifest 的 `artifact.sha256` 与 asset 一致。

manifest 查找 key:先按 `repo.name` 精确匹配;未命中且 `indexes` 仅一个条目时取该条目
(mcpp 场景:key `mcpp` vs 配置名 `mcpplibs`,由此规则覆盖);多条目且无匹配 → 明确报错。

### 2.3 base 的两种布局

由 base 的 host 判定(与现有 `rawFor` / `resolve_base_` 的语义合并,不新造概念):

- **forge 布局**(github.com / gitcode.com):pointer 走 raw 路径
  (`raw.githubusercontent.com/.../main/<f>`、`raw.gitcode.com/.../raw/main/<f>`,复用
  `index_pointer_urls` 的映射),artifact 走 `<base>/releases/download/<tag>/<asset>`
  (复用 `index_asset_urls` 的拼装)。
- **flat 布局**(其余 http(s) base、`file://`、本地路径):pointer 与 artifact 均为
  `<base>/<文件名>` 直取(复用 `obtain_file` 的 local/remote 处理)。自建静态服务器与
  e2e fixture 由此天然支持。

### 2.4 客户端改动(复用现有骨架,共 4 处)

1. **`config.cppm`** — `IndexRepo` 增加 `std::string artifactBase;`(已按 mirror 解析)与
   `std::string source;`;`load_index_repos_from_json_` 解析两个新字段(缺省为空)。
2. **`indexfetch.cppm`** —
   - `index_asset_urls` / `index_pointer_urls` 增加可选的 `(server, repo)` 覆盖参数;
     缺省仍是 `Config::resource_server()` + `index_repo_name_()`,官方路径零行为变化。
   - `load_index_pointers` 从单一 `once_flag` 缓存改为**按 base 键控的缓存**
     (`map<base, map<key, IndexManifest>>`;官方来源即 base=""),每来源每进程仍只拉一次。
   - `fetch_index_artifact` 增加可选 `const IndexRepo* customSource`:非空时用其 base 解析
     pointer/asset URL 与 manifest key。下载→sha256→解压→`pkgs/` 检查→
     `.xlings-index-version`→原子交换,全部原样复用。
3. **`repo.cppm`** —
   - `sub_should_attempt_artifact` 准入从 `isDefaultOfficial` 放宽为
     `isDefaultOfficial || hasArtifactSource`;声明了 artifact 的自定义仓在 auto 下
     **总是先试 artifact**(与主索引 auto 收敛语义一致,不做 C1 式迁移闸 —— 原子交换对
     既有 git checkout 的迁移本就是安全的);每仓 `source` 覆盖全局值。
   - 抽出单仓同步助手(artifact 尝试 + git 回退),接入三处:全局子索引循环(已有该
     模式)、`syncRepos` 主列表(global + project 的非官方仓)、project 子索引循环。
     mcpp 的 project 场景由此覆盖。
4. **持久化** — `xim-indexrepos.json` 值保持 string(纯 git 仓),声明了 artifact 的仓写
   对象 `{"url":..,"artifact":..,"source":..}`;`load_sub_repos_json` 两种都接受。

**天然继承的既有保护**(无需新代码):`sync_repo` 的非破坏 guard(拒绝对 pkgs-无-.git 目录
做破坏性 clone)同样保护自定义 artifact 管理目录;`reconcile_index_temps` 覆盖其临时目录;
`get_repo_head_hash` 经 `.xlings-index-version` 为 artifact 目录提供缓存 key,catalog 无感知。

### 2.5 消费端接入(mcpp 侧,随后跟进)

xlings 侧落地后,mcpp 需要三个小改动才能让 `mcpplibs` 真正吃到 artifact(基于
mcpp-community/mcpp 现状核实):

1. **默认配置模板 + seed 透传**:`config.cppm` 的 `write_default_config_toml` 给
   `[index.repos."mcpplibs"]` 加 `artifact = "https://github.com/xlings-res/mcpp-index"`;
   `seed_xlings_json` 的 `(name, url)` pair 透传 `artifact`/`source` 两个新字段。
2. **存量迁移**:registry `.xlings.json` 只在缺失时 seed;需要一个
   `migrate_xlings_json_index_names` 同款的一次性迁移,给既有 `mcpplibs` 条目补
   `artifact` 字段(mcpp 已有该迁移先例与调用点)。
3. **(可选)`[indices]` 扩展**:mcpp 项目级 `IndexSpec` 已支持 url/rev/tag/branch/path,
   可后续增加 `artifact`,让用户的项目自定义索引也声明 artifact 来源 —— 非本轮必需。

另:pointer key(`mcpp`)与配置名(`mcpplibs`)不一致由 §2.2 的"唯一条目兜底"覆盖;
若未来 mcpp-index 发布多个 key,应把 key 与消费端配置名对齐。

### 2.6 git 回退加固(#378)

`compact::git` 在拼命令前调用一次 `ensure_ca_env_()`(同构先例:`prepend_current_bin_dir_`
已在调用前 `set_env_variable("PATH", ...)`):

```cpp
inline void ensure_ca_env_() {
    if (!env_or_empty_("GIT_SSL_CAINFO").empty()) return;     // 用户/CI 显式设置优先
    if (std::filesystem::exists("/etc/ssl/cert.pem")) return; // Alpine/BSD 布局本来就对
    for (auto* f : {"/etc/ssl/certs/ca-certificates.crt",     // Debian/Ubuntu/Arch/openSUSE
                    "/etc/pki/tls/certs/ca-bundle.crt",       // RHEL/CentOS/Fedora
                    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                    "/etc/ssl/ca-bundle.pem"})                // openSUSE 兼容
        if (std::filesystem::exists(f)) {
            platform::set_env_variable("GIT_SSL_CAINFO", f);
            return;
        }
}
```

Linux-only(`#if` 守护);Windows/macOS 的 git 不经此 CA 路径。覆盖系统 git、
`XLINGS_COMPACT_GIT_BIN` 覆盖、bootstrap 等一切绕过 xvm 的调用点。

---

## 3. 信任模型与兼容性

- **信任模型与官方索引完全一致**:HTTPS raw 取 pointer,manifest 钉 sha256,校验后原子替换;
  manifest 的 `signature` 预留位保持,端到端签名(minisign)仍是后续阶段、且自定义仓与官方仓
  将同构受益。用户添加 `artifact` base 与添加 git `url` 的信任决策等价 —— 都是"我信任这个来源"。
- **向后兼容**:无 `artifact` 字段 → 全部行为逐字节不变;官方索引(main + 默认子索引)路径
  不受任何影响;`XLINGS_INDEX_REPO/TAG/BASE_URL` 环境覆盖保持只作用于官方来源。
- **降级兼容**:旧版 xlings 读到 `xim-indexrepos.json` 的对象值会跳过该仓(`is_string()`
  过滤),不会崩溃;`.xlings.json` 是配置事实源,升级回来即恢复 —— 可接受,文档注明。

## 4. 测试

- **单测**:`sub_should_attempt_artifact` 新准入矩阵(official/custom × auto/artifact/git ×
  managed/fresh/git-checkout);`load_index_repos_from_json_` 新字段(string/region 对象/缺省);
  `xim-indexrepos.json` string/object 双格式往返;manifest key 匹配规则(精确/唯一条目/多条目报错)。
- **e2e**:`file://` flat-布局 fixture(pointer + tarball)验证自定义仓 artifact 全流程、
  失败注入(坏 sha256 / 缺 pointer / 缺 asset)验证 git 回退与 `source:"artifact"` 硬失败、
  既有 git checkout → artifact 迁移(`.git` 消失、`.xlings-index-version` 出现)。
- **实机**:以 `xlings-res/mcpp-index` 为真实对象跑通 mcpp 场景(project 级 index_repos)。

## 5. 非目标

- 端到端签名/内容寻址(预留位已在,后续阶段)。
- pointer 的 ETag/304 增量(结构已预留)。
- github-proxy 扩展到自定义 artifact 来源(与官方索引同理由排除:代理常 TCP 可达但不服务 asset)。
- `xlings repo add` CLI 对 artifact 字段的交互式支持(配置文件先行,CLI 可后补)。
