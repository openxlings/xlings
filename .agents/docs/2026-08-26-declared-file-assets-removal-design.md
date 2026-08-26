# 声明式文件资产不随卸载回收(#423)——根因与修复方案

> 状态:**已实现**(2026.8.26.1,分支 `fix/423-declared-file-assets-removal`)。
> 落地结果见 §8,其中**五条原方案的判断被实测推翻**,已在 §8 逐条标注
> 而不是悄悄改掉。
> 所有数字都在用户真实 home(`/home/speak/.xlings`)与真实索引
> (`~/.xlings/data/xim-pkgindex`)上实测,命令写在每节的「实测」里,可复现。
> 代码位置对应 HEAD `b6fdd87`(2026.8.22.4 之后)。

---

## 0. 一句话

`xvm.files` 声明的资产,**两条卸载路径都不回收**——不是"少了一个分支",
而是**同一个问题有三个回答者,只有一个答对**:

| 谁在问"这个 release 往 subos 里放了哪些资产" | 怎么问 | 结果 |
|---|---|---|
| `switch_plan.cpp:72-140`(`xlings use`) | 遍历 `selection->members`,逐成员 `file_placement` | **对** |
| `installer.cpp:1316`(detach 路径) | 拿**包名**去问 `file_placement` | 恒空 |
| `installer.cpp:3226-3236`(完整卸载路径) | **根本没问** | 无 |

资产注册在 `<pkg>.files.<n>` 上(名字由 libxpkg 的 Lua 计数器生成),
**永远不等于包名**,所以第二个回答者不是"漏了一种情况",是构造上不可能对。

---

## 1. 真实规模:比 issue 开出时大两个量级

### 1.1 索引侧

```
$ P=~/.xlings/data/xim-pkgindex/pkgs
$ grep -rl "declare_headers\|xvm.files" $P | wc -l        # 39
$ grep -rl "declare_headers_tree" $P | wc -l              # 29
```

不是 issue 正文写的 7 个,也不是 8-26 评论里写的 ~18 个——**39 个 recipe**
使用声明式资产,其中 **29 个用 `declare_headers_tree`**(逐文件声明,嵌套 2-4 层)。

### 1.2 用户 home 的当前状态

```
$ python3 -c "…"   # 见 §5.1
targets 总数 1612,其中 .files. targets 891,version 记录 1020
  glib 279  glibc 130  xorgproto 129  musl 98  alsa-lib 48  libdrm 43 …
```

**已经泄漏在盘上的**(用 `-xtype l`,不是 `-e`):

```
$ cd ~/.xlings/subos
$ for s in */; do n=$(find "${s%/}/usr" "${s%/}/etc" "${s%/}/share" -xtype l 2>/dev/null | wc -l); …
current: 6      default: 6      gfxbuild: 98      xkbverify: 6
```

而 `self doctor` 当前对这四个 subos 全部报告 **0 条** SysrootDangling(原因见 §2.3)。

### 1.3 这些泄漏的来源可以指名

```
$ ls -d ~/.xlings/data/xpkgs/xim-x-libxkbcommon*     # 不存在(payload 已删)
$ python3 -c "… [t for t in V if 'xkb' in t]"        # DB 里没有 libxkbcommon 条目
$ cd ~/.xlings/subos/default && find usr -xtype l
usr/include/xkbcommon/xkbcommon-x11.h -> …/xpkgs/xim-x-libxkbcommon/1.7.0/include/xkbcommon/xkbcommon-x11.h
…(6 条)
$ python3 -c "… 所有 subos 的 workspace 里搜 xkbcommon"   # 无
```

**payload 没了、DB 记录没了、workspace 记录也没了,只有符号链接留着。**
到这一步它们已经无法归因到任何包——`inspect_sysroot_ownership` 只会说
"没有包声明它"(而且它也看不见,见 §2.3)。

`gfxbuild` 的 98 条全部指向已删除的 `local-x-musl`。

---

## 2. 根因

### 2.1 D1 —— 完整卸载路径缺第三个 cleanup

`Installer::uninstall`(`installer.cpp:2942`)在移除批次之后只做两件事:

```cpp
// installer.cpp:3226-3236
cleanup_removed_xvm_library_artifacts(artifactSubosDir / "lib", …);  // kind=="lib"   (:610 continue)
cleanup_removed_xvm_program_artifacts(artifactSubosDir / "bin", …);  // kind=="program" (:661)
```

**数据其实就在手边**:`apply_removal_batch` 在 `purgeSelection = true` 下会遍历
`context.members` 把整个 release 的成员全部删掉(`removal.cpp:278-293`),
而 `<pkg>.files.<n>` 带 `bindingGroup`,是 release 的正经成员——所以
`removalResult->removed` 里**逐条列着**每一个泄漏的资产。
上面两个函数各自遍历这同一个列表,然后 `continue` 跳过它们。

> 这是本仓库反复出现的形状:**该做的事从没发生过,和做成功了,输出一模一样**
> (`project_silent_success_pattern`)。

**同族第四个成员也缺**:`bindingHeaders` / `includedir` 声明的头目录在完整卸载
路径上同样无人回收。但实测真实 DB 里 **0 条**记录带这两个字段(全部迁到
`xvm.files` 了),所以目前没有活的生产者。**记录在案,不安排工作量。**

### 2.2 D2 —— detach 路径三处查询全部问错目标(比 D1 更危险)

`detach_current_subos_`(`installer.cpp:1269`)的 subos 拆除段:

```cpp
// installer.cpp:1307-1319
for (const auto& asset : xvm::group_header_assets(db, target, version))   // ①
    xvm::remove_headers(asset, sysroot_include);
if (const auto placement = xvm::library_placement(db, target, version); …) // ②
    xvm::remove_library(placement.name, sysroot_lib);
if (const auto file = xvm::file_placement(db, target, version); …)         // ③
    xvm::remove_asset(Config::paths().subosDir / file.destination);
```

三行都拿 `target`(=包名,如 `glib`)去查,而资产全部挂在**成员**上:

| | 为什么恒空 | 实测 |
|---|---|---|
| ① | 走 `bindingHeaders`/`includedir` | 真实 DB 中带这两个字段的条目 **0 条** |
| ② | 只有 `target` 自己是 `kind=="lib"` 才命中 | glib 的 15 个 lib 是 `libglib-2.0.so.0` 等**独立 target** |
| ③ | 资产 target 是 `<pkg>.files.<n>`,由 Lua 计数器生成 | `libxpkg/src/lua-stdlib/xim/libxpkg/xvm.lua:77` |

**结论:在任何现代 home 上,`detach_current_subos_` 的 sysroot 拆除段清理量为零。**
它实际只做了 `remove_target_shims_`(那个确实遍历了 `vinfo->bindings`)。

为什么这一半比 D1 更危险:detach 走的是 `stillReferenced` 分支
(`installer.cpp:3065-3076`),**payload 还活着**。所以留下的不是悬空链接,
是**能用的链接**——用户 `xlings remove glib`,glib 的 274 个头文件、15 个库、
5 个 `.pc` 继续躺在 sysroot 里正常工作。没有任何扫描会报告它,
编译器会照用不误。

对比:
- D1 留下**坏的**链接 —— 难看、会累积,但不会被误用。
- D2 留下**好的**链接 —— 一个"已经卸载"的包继续参与编译。

### 2.3 D3 —— doctor 的悬空扫描是深度 1,看不见现在的形状

```cpp
// doctor.cpp:1759-1772
for (const auto& sub : {"usr/include", "usr/lib", "usr/lib64", "usr/bin"}) {
    …
    for (const auto& entry : platform::dir_entries(dir)) {   // directory_iterator,非递归
```

`platform::dir_entries` 就是 `std::filesystem::directory_iterator`
(`modules/platform/src/platform.cppm:239`)——**只看直接子项**。

它旁边的注释写明了当初的假设:

```cpp
// doctor.cpp:1418-1420
// 只有扫描才能看见它们。扫描很便宜(header farm 是每个顶层条目一个链接)。
```

`declare_headers`(目录粒度)时代这句话是对的。`declare_headers_tree`
——为 X11/graphics 栈引入,**29 个 recipe 在用**——把它变成了**每个文件一个链接,
嵌套 2-4 层**。扫描的形状没跟着改。

覆盖缺口:

| 泄漏位置 | 深度 | doctor 看得见? |
|---|---|---|
| `usr/include/xkbcommon/*.h` | 3 | ✗ |
| `usr/include/glib-2.0/gio/*.h` | 4 | ✗ |
| `usr/lib/pkgconfig/*.pc` | 3 | ✗ |
| `etc/**`、`share/**` | 任意 | ✗(根本不在扫描列表里) |

`is_permitted_file_destination`(`bindings.cpp:506-518`)允许的顶层是
`usr` / `etc` / `share`——**后两个 doctor 一个都不扫**。

所以 2026-08-08 triage 评论里写的"interim mitigation: `self doctor --fix`
SysrootDangling sweep"**对当前形状不成立**。实测:110 条泄漏,doctor 报 0。

> 同一个 reporter/repairer 口径漂移的家族(`reference_reporter_repairer_predicate_drift`),
> 这次漂的不是谓词,是**扫描深度**。

### 2.4 D4 —— `use` 换版本时尾部资产被搁浅(已报告,未回收)

`plan_use_switch` 遍历的是**进入方**的成员(`switch_plan.cpp:72`)。
若旧版本声明 279 个资产、新版本声明 250 个,则 `<pkg>.files.251..279`
不在进入方成员里,永远不被访问 —— 它们的链接继续指向旧 payload,
sysroot 变成两个 release 的混合体(正是 `group_header_assets` 注释里说
"binding-group 工作存在就是为了让这个状态不可表示"的那个状态)。

这一条**不是静默的**:`switch_plan.cpp:217-236` 会把它们收进 `plan.stranded`
并打印(`kind_can_strand` 对 `files` 返回 true)。但对 274 个资产的包,
"报告 29 条 stranded 让用户自己处理"不是一个可用的答案。

优先级低于 D1/D2,但**修 D1/D2 时的资产解析器可以直接复用**。

### 2.5 Windows:更严重,且完全不可见

`create_link_`(`commands.cpp:30-48`):

```cpp
#if defined(_WIN32)
    if (fs::is_directory(src)) platform::create_directory_link(...);   // junction
    else { fs::create_hard_link(...); if (ec) fs::copy_file(...); }    // 硬链接 / 副本
#else
    fs::create_symlink(src, dst, ec);
#endif
```

Windows 上文件资产是**硬链接或副本**:

- payload 删了,硬链接**仍然让文件内容活着**——占着盘,而且内容是旧版本的。
- `-xtype l` / `is_symlink` 判据在 Windows 上**恒为假**,悬空扫描永远看不见。

**推论,并且是整个方案的地基:回收必须由元数据驱动,不能由"这是不是一个悬空
符号链接"驱动。** 文件系统形态是平台相关的,声明不是。

### 2.6 D5 —— 声明粒度冲突:现在就在丢头文件(与卸载无关,查这条时发现的)

设计 §4.2 的冲突分支时去核对 `usr/include/scsi` 那一处冲突,结果它不是
"卸载时的边界情况",是一个**此刻正在生效的缺陷**。

两个包各自用 `declare_headers`(**目录粒度**)声明了同一个名字,而两边的内容
**完全不相交**:

```
$ ls ~/.xlings/data/xpkgs/xim-x-glibc/2.44/include/scsi
scsi.h  scsi_ioctl.h  sg.h                                   # 3 个
$ ls ~/.xlings/data/xpkgs/xim-x-linux-headers/5.11.1/include/scsi
cxlflash_ioctl.h  fc  scsi_bsg_fc.h  scsi_bsg_ufs.h  scsi_netlink_fc.h  scsi_netlink.h
```

发行版的 `/usr/include/scsi` 是这 9 项的**并集**。而这里:

```
$ ls -l ~/.xlings/subos/default/usr/include/scsi
… -> …/xpkgs/xim-x-linux-headers/5.11.1/include/scsi          # 整个目录一个链接
$ ls ~/.xlings/subos/default/usr/include/scsi/sg.h
*** MISSING ***      scsi.h / scsi_ioctl.h 同样 MISSING
```

**glibc 装着、声明着,`<scsi/sg.h>` 在这个 subos 里就是不存在。**
`sg.h` 不是冷门头(SCSI generic ioctl:util-linux、hdparm、smartmontools、
libcdio 都用)。

`glibc.lua:371-383` 的注释是知道这件事的——它测过、算过、接受了
"still order-dependent, but now *recorded*"。**但"recorded"没有换来任何东西:
没有 finding 报告它,`doctor` 不看,丢的 3 个头文件没有任何输出。**
记录了却没人读,和没记录是同一个结果。

#### 已有的正确机制没被用上

`declare_headers_tree` 就是为这件事造的(X11 被 8+ 个包分摊)。实测它工作正常:

```
$ ls -ld ~/.xlings/subos/default/usr/include/X11        # 真目录,不是链接
$ ls ~/.xlings/subos/default/usr/include/X11 | wc -l    # 45 项
$ find …/X11 -type l | xargs readlink | …               # 来自 14 个 payload
```

**X11 是 14 个包合并出来的真目录;scsi 是 2 个包里赢家通吃的一个链接。**
同一个索引里,同一类问题,两种结果。

#### 代价实测(决定怎么修)

| 方案 | glibc 节点 | linux-headers 节点 | 全库 files 记录 |
|---|---|---|---|
| 现状(整个 `include` 走目录粒度) | 129 | 11 | 1020 |
| **只把 `scsi` 改成逐文件** | 128+3=131 | 10+9=19 | **1030(+10)** |
| 两边都改成全树 | 484 | 937 | 2301(**翻倍**) |

**建议:定向合并,+10 个节点。** 全树把整个 files DB 翻一倍,买不到任何东西
——glibc 129 个顶层名里只有 1 个冲突,实测过
(`comm -12` 两边的顶层清单,重叠集合 = `{scsi}`)。

`sysroot.lua` 需要一个小口子,因为现在没法表达"这个目录走目录粒度,但其中
某几个名字递归":

```lua
sysroot.declare_headers(install_dir, "include", "usr/include", binding,
                        { merge = { "scsi" } })   -- 这些名字改走 __declare_tree
```

冲突集是**数据**,不能在安装时自动发现(另一个包可能还没装),所以两个 recipe
各自显式写出这一行,是可 review 的。

#### 迁移本身有个坑,必须一起处理

`place_asset`(`commands.cpp:158`)是 `fs::create_directories(destination.parent_path())`,
**没有检查祖先是不是符号链接**。如果一边先改成逐文件、另一边还声明着目录,
那么写 `usr/include/scsi/sg.h` 时,`create_directories` 看到
`usr/include/scsi` 是一个指向对方 payload 的链接,认为"已存在",
于是**把文件写进对方的 payload 里**。

现存的 home 也有这个形状(链接已经在盘上),所以迁移必须:

1. 客户端先加护栏:`place_asset` 若目的地的任一祖先是符号链接 → 拒绝 + warn
   (或先拆掉那个链接)。**这条先于索引改动发布。**
2. 索引侧两个 recipe **在同一次发布里一起改**,不能一前一后。

实测当前"目录资产吞掉别人叶子资产"的组合数为 **0**,所以这个坑现在还没被踩到
——它是**这次修复引入的**,不是既有的。

**D5 与 #423 是两个缺陷,建议单独开 issue。** 它放在这里只因为
§4.2 的冲突分支必须知道它的存在,并且 §4.2 **修不了它**:
回收侧只能在"卸载之后还剩谁声明"里做选择,两个包都在的时候丢的那 3 个头文件,
是声明粒度的问题。

---

## 3. 为什么现存的两道"补救"都不成立

| 补救 | 为什么不成立 |
|---|---|
| `self doctor --fix` 的 SysrootDangling sweep | 深度 1(§2.3);Windows 上判据恒假(§2.5);对 D2 留下的**非悬空**链接原理上无效 |
| recipe 在 `uninstall()` 里手写 `rm` | ① 39 个用声明式资产的 recipe 里**只有 8 个**写了(§7),X11/graphics 栈 26 个一个都没有;② detach 路径**根本不执行 `uninstall()` 钩子**——`installer.cpp:3065-3076` 在创建 executor(`:3083`)之前就 return 了 |

第二条值得单独强调:索引侧那段注释写着"确认客户端会回收后再删这几行",
但它保护的只是**单 subos 的完整卸载,且只覆盖 8 个包**。
**多 subos 下它从未生效过。**

---

## 4. 修复方案

### 4.1 地基:一个问题,一个回答者

新增(`xlings.core.xvm.bindings`):

```cpp
// 这个 release 往 subos 里放的全部文件资产,逐成员解析。
// 空 vector 有两种含义(release 不解析 / 确实没有资产),调用方都按"没有"处理——
// 这与 snapshot_removal_context 对不可解析 release 的退让方向一致。
std::vector<FilePlacement> release_file_placements(const VersionDB& db,
                                                   const std::string& target,
                                                   const std::string& version);
```

实现就是 `inspect.cpp:16-31` 的 `release_declares_file_assets_` 已经在做的
成员遍历,只是返回资产而不是 bool。**并把 `release_declares_file_assets_`
改写成 `!release_file_placements(...).empty()`**,这样"有没有"和"有哪些"
两个答案不可能漂移。

同时给 `FilePlacement` 补一个来源坐标(`target` / `version`),
下游需要它来报告与去重。

> 这一步是方案里唯一不可省的部分。省掉它,D1/D2/D4 就会各写一份成员遍历,
> 三份将来会分头漂移——这正是 §0 表格里那三个回答者的来历。

### 4.2 D1:`cleanup_removed_xvm_file_artifacts`

与 lib/program 两个同族函数同形、同签名风格,放在 `installer.cpp:695` 之后:

```cpp
void cleanup_removed_xvm_file_artifacts(
        const std::filesystem::path& subosDir,
        const xvm::VersionDB& dbBeforeRemoval,
        const xvm::VersionDB& currentDb,
        const xvm::Workspace& currentWorkspace,   // ← 比同族多这一个,理由见下
        const xvm::RemovalBatchResult& removalResult);
```

步骤:

1. 从 `dbBeforeRemoval` 取 `removalResult.removed` 中 `effective_kind == "files"`
   的条目,收集 `fileDst`(顺带校验 `is_permitted_file_destination`,
   拒绝一个被手改过的记录把删除引到 subos 外面)。

2. **幸存声明索引**——对 `currentDb` **一次遍历**建 `fileDst -> {target,version}`
   的表,而不是像 lib 版那样对每个目的地重扫整个 DB。
   lib 版的 O(n·m) 在 15 个库上无所谓,在 274 个资产上不是。

3. 三分支决定每个目的地的去向(**这里比 lib 版更严,理由在下面**):

   | 幸存声明 | 动作 |
   |---|---|
   | 无 | 删除 |
   | 有,且在**本 subos 的 active** 里 | **重新放置**(`place_asset`),让链接指向幸存者的 payload |
   | 有,但本 subos 没有 active | 删除 |

   为什么不能照抄 lib 版的"有幸存者就跳过":实测本 home 里
   `usr/include/scsi` 同时被 `xim:glibc` 和 `xim:linux-headers` 声明
   (**唯一一处冲突,但它是真的**,详见 §2.6)。当前链接指向 linux-headers;
   若卸载的是 linux-headers,"跳过"留下的链接指向已删除的 payload
   —— 把泄漏换成了悬空,而 glibc 明明还声明着它。
   重新放置恢复的是**仅存的那一份声明**,不是替用户挑赢家。

   `cleanup_removed_xvm_library_artifacts` 现在就有这个潜在问题,
   只是暂时没有 lib 侧的冲突把它暴露出来。**建议同时收敛,但可以拆成两个 PR。**

   > **这一条只处理"卸载之后谁还声明它"。冲突本身是另一个缺陷,
   > 回收侧解决不了,也不该解决——见 §2.6。**

4. 删除动作:`fs::remove`(**不是 `remove_all`**)。
   `doctor.cpp:2050-2052` 已经为这件事写过理由:
   "如果它碰巧可跟随,`remove_all` 会把 payload 删掉"。
   现有的 `xvm::remove_asset`(`commands.cpp:192-198`)用的是 `remove_all`,
   `use` 路径已经在用它——**不在本次改它**,新代码直接用 `fs::remove`,
   并在 §6 的待办里记一条一致性收敛。

   POSIX 上的额外护栏:如果目的地是符号链接且指向 payload store **之外**,
   跳过并 `log::warn`——那是别人换掉的,属于 `xvm-sysroot-drift`,
   不该由卸载来裁决。Windows 上没有这个判据,由元数据单独决定(§2.5)。

5. 空父目录回收:从资产父目录向上,`fs::remove`(非递归,非空会失败,
   所以拿不走别人的东西),**深度 ≥ 3 才删**——即 `usr/include/xkbcommon` 可删,
   `usr/include` / `usr/lib` / `etc/ssl` 这类两段路径保留。
   `remove_headers(HeaderAsset&)`(`commands.cpp:143-146`)已经是同样的手法,
   只是它只清一层前缀。

调用点:`installer.cpp:3236` 之后、`remove_payload_dir`(`:3255`)之前。

### 4.3 D2:detach 路径改走成员

`detach_current_subos_` 的三行(`installer.cpp:1307-1319`)全部换成成员遍历:

```cpp
auto selection = xvm::resolve_binding_selection(db, target, version);
// 不解析时退回到指名的那一个,与 snapshot_removal_context 同理(#421 的口径)
for (auto& [mt, mv] : members) {
    for (auto& asset : group_header_assets(db, mt, mv)) remove_headers(asset, sysroot_include);
    if (auto p = library_placement(db, mt, mv); !p.empty()) remove_library(p.name, sysroot_lib);
}
for (auto& f : release_file_placements(db, target, version)) { /* 同 4.2 的三分支 */ }
```

**并且**:detach 目前只把 `target` 从本 subos 的 `installed[]`/`active` 里摘掉,
成员留在原地(实测 `subos/default/.xlings.json` 的 268 个 workspace 条目里,
**140 个是 `.files.` 成员**)。这些残留最终靠 `doctor --fix` 的
`apply_subos_metadata_repair` 扫走——那是"另一个命令替它收尾",不是 detach 做完了。
**同一处一起修:detach 时按成员摘除。**

> 这也解释了 §1.3 里的怪事:`xkbverify` 的 workspace 干净但链接还在——
> 元数据被 doctor 事后扫走了,文件系统没人管。

### 4.4 D3:doctor 从"扫悬空"改成"对账"

现在的扫描是"找 dangling",它有两个原理性缺口:看不见 D2 留下的**非悬空**残留,
Windows 上判据恒假。改成对账:

- **应有集**:本 subos active 选择里所有成员的 `fileDst`(已经有一半了,
  `doctor.cpp:1396-1412`,只是它只看 `st.ws[target]` 自己那条记录,
  没有走成员——**和 D2 是同一个 bug 的第三个副本**)。
- **实有集**:递归遍历 `<subos>/{usr,etc,share}`,收集指向 payload store 的条目
  (POSIX:符号链接目标以 `dataDir/xpkgs` 开头;Windows:见下)。
- 实有 − 应有 = **孤儿**(新 finding,可 `--fix` 删除)。
  悬空只是孤儿的一个子集,继续单独报以保留现有措辞。

成本实测(真实 home 的 default subos):

```
$ cd ~/.xlings/subos/default && time find usr etc share -mindepth 1 | wc -l
1178
real 0m0.023s
```

**不是性能问题。** 不要为它引入缓存——`0810` 那次 doctor 195s→0.63s 的开销
在 ELF 读取和子进程,不在目录遍历。

Windows 上没有"指向 payload store"这个判据(硬链接/副本)。可行的替代是
比对 `fs::hard_link_count` 或内容哈希,但那是另一个量级的工作。
**本轮的诚实做法:Windows 只做元数据侧的对账(应有集里缺链接 → 报),
孤儿检测标注为 POSIX-only,并在 finding 文案里说明。** 不要假装覆盖了。

### 4.5 D4:先不动,但把决定写下来

`stranded` 的现有语义是"报告,绝不代为处置"(`switch_plan.cppm:96-102`),
理由是"替用户猜意图"。这个理由对 program/lib 成立(名字有归属之争),
对 `files` 不成立——一个文件资产的目的地只由声明决定,没有第二个主人。

**建议:`kind == "files"` 的搁浅成员由 `use` 直接回收,不进 `stranded` 报告。**
但**放到 D1/D2 落地并验证之后**再做,理由是它改的是 `use` 的可见行为,
而 D1/D2 只是补上从未发生过的清理。

### 4.6 存量修复

已经泄漏的链接(本 home 110 条)**没有元数据可以归因**——DB 和 workspace 记录
都已经被 purge/扫走。只能靠 §4.4 的孤儿检测(指向 payload store、无人声明)
+ 悬空检测(指向不存在的路径)兜底。这两条合起来能覆盖 §1.2 的全部 110 条。

发布说明里需要写明:升级后跑一次 `xlings self doctor --fix`,
并且**每个非活跃 subos 要单独跑**(`XLINGS_ACTIVE_SUBOS=<name>`)——
现有 finding 已经带了这个 remedy 文案(`doctor.cpp:1785-1794`)。

---

## 5. 验证方案

### 5.1 判据(issue 里点名的那条,以及为什么它还不够)

`[ -e ]` 跟随符号链接,悬空链接读作"不存在"——这是让原测试通过的原因。
用 `-L` 或 `find -xtype l`。

**但只用 `-xtype l` 只能验 D1,验不了 D2**(payload 还在,链接不悬空)。
D2 需要**对账式**判据:

```bash
# 应有:本 subos active 选择声明的 fileDst
# 实有:subos 下所有指向 payload store 的链接
# 差集必须为空
comm -13 <(declared_dsts | sort) <(find "$SUBOS"/{usr,etc,share} -type l \
          -lname "$XLINGS_HOME/data/xpkgs/*" -printf '%P\n' | sort)
```

差集非空即失败。这条判据同时覆盖 D1(悬空的也在实有集里)和 D2。

### 5.2 单元测试(`tests/unit/`)

`cleanup_removed_xvm_*` 三个函数都是纯数据 + 一个目录路径,
且已经从 `installer.cppm:200-211` 导出——直接进 `test_xim_install.cpp`:

1. 移除一个带 3 个 files 成员的 release → 3 个目的地被删,空父目录被回收。
2. **`usr/include/scsi` 冲突场景**:两个 provider 声明同一 `fileDst`,
   移除其中一个 → 目的地重新指向幸存者的 payload(不是"跳过",也不是"删掉")。
3. 幸存者存在但本 subos 未 active → 删除。
4. 深度 ≥ 3 才回收父目录:`usr/include` 必须留下。
5. 目的地是指向 payload store 之外的符号链接 → 跳过 + warn(POSIX)。

`release_file_placements`:成员遍历、release 不解析时退回单条、
`is_permitted_file_destination` 拒绝的记录不产出。

D5 的护栏(§2.6,`place_asset`):目的地祖先是指向 payload 的符号链接时
**不得写入** —— 断言对方 payload 里没有多出文件。这条不是回归测试,
是**在缺陷被引入之前**先立的门。

### 5.3 e2e

新增 `tests/e2e/declared_file_assets_removal_test.sh`,**必须注册进 `run_all.sh`**
(`reference_e2e_set_e_silent_death`:不注册的 e2e 等于不存在;
命令替换赋值在 `set -e` 下会静默退出)。

场景,用隔离 home(`reference_isolated_home_test_traps` / `..._mirror_cn`:
新 home 默认 GLOBAL,先设 CN;不要预置 `data/`):

| # | 场景 | 断言 |
|---|---|---|
| S1 | 单 subos 装+卸一个 `declare_headers_tree` fixture | §5.1 差集为空;父目录已回收;`usr/include` 仍在 |
| S2 | **两个 subos 都装,在其中一个 remove**(detach 路径) | 该 subos 差集为空;**另一个 subos 一条不少**;payload 仍在 |
| S3 | 两个包声明同一 `dst`,移除其中一个 | 目的地指向幸存者的 payload,**不悬空** |
| S4 | 存量修复:手工造出孤儿链接 → `doctor` 报告 → `--fix` 清除 → 再报告为 0 | 报告与修复用**同一个谓词**(`reference_reporter_repairer_predicate_drift`) |

S2 是这次的关键场景——它是唯一能测到 D2 的,而 D2 是现有测试与现有
recipe 补救**都完全没覆盖**的那一半。

### 5.4 真实 home 上的落地后验证

```bash
# 升级前:110(current 6 / default 6 / gfxbuild 98 / xkbverify 6)
# 升级 + 每个 subos 跑 doctor --fix 后:0
# 然后装一个 glib 再卸,S1 判据必须为空
```

---

## 6. 落地顺序与风险

| 步 | 内容 | 阻塞关系 |
|---|---|---|
| 1 | `release_file_placements` + 改写 `release_declares_file_assets_` | 后面全部依赖 |
| 2 | D1 `cleanup_removed_xvm_file_artifacts` + 单测 | — |
| 3 | D2 detach 走成员(含成员元数据摘除)+ e2e S2 | 依赖 1 |
| 4 | D3 doctor 对账 + e2e S4 | 依赖 1 |
| 5 | 发布,真实 home 验证(§5.4) | — |
| 6 | D4 `use` 回收搁浅的 files 成员 | 依赖 5 的实测 |
| 7 | 索引侧撤回手写 `rm` | 依赖 5,见 §7 |

**目标版本:2026.8.26.1**(步 1-5)。

**D5(§2.6)是独立缺陷,走自己的线**,不要塞进上表:

| 步 | 内容 | 归属 |
|---|---|---|
| a | `place_asset` 祖先符号链接护栏 + 单测 | xlings,**必须先发布** |
| b | `sysroot.declare_headers(..., {merge={...}})` | xim-pkgindex |
| c | glibc / linux-headers **同一次发布**加 `merge={"scsi"}` | xim-pkgindex,依赖 a+b |
| d | 判据:`ls <subos>/usr/include/scsi` 必须同时有 `sg.h` 与 `scsi_netlink.h` | — |

a 可以和步 1-5 同批发布(它们互不依赖),但 c **不能早于 a 到达用户**。

### 风险

- **误删**。唯一的删除依据是"这个目的地由我们刚删掉的条目声明,且没有活的
  幸存者声明它"。宿主 bind-mount 进来的东西从来不在 DB 里,不会被选中。
  POSIX 上还有"链接必须指向 payload store"这道额外护栏。
- **`remove_all` vs `remove`**。新代码用 `remove`。现存的 `xvm::remove_asset`
  用 `remove_all`,`use` 路径在用——**本次不改,记一条待办**。
  Windows junction 在 `remove_all` 下是否会跟随进 payload,**未验证**,
  不要在没测过之前声称它安全。
- **并发**。删除发生在 `Config::save_versions()` 附近,受 home 锁保护;
  但目录回收是文件系统操作,需要确认它在锁的范围内
  (`reference_lock_timeout_couples_to_tests`:改锁范围会牵动 CI 时长)。
- **一致性收敛的诱惑**。`cleanup_removed_xvm_library_artifacts` 的
  "有幸存者就跳过"同样会留下悬空链接(§4.2 步 3)。**建议修,但拆开走**,
  不要和本 issue 混在一个 PR 里——它影响的是另一批用户的另一种状态。

---

## 7. 索引侧后续(xim-pkgindex)

`libs/sysroot.lua:62-64` 那句注释——声明式资产"removed with the release
instead of by a hand-written mirror of this call in uninstall()"——
**在客户端修好之前不成立**,在修好之后对**老客户端**仍然不成立。

实测:39 个用声明式资产的 recipe 里,**只有 8 个**在 `uninstall()` 里手写了 `rm`

```
libselinux libxml2 openssl ca-certificates zlib glib freetype util-linux
```

(前 5 个来自 2026-07-27 那批迁移,后 3 个是为本 issue 在 #680 里专门加的。)

**剩下 31 个——包括整个 X11 / graphics 栈的 26 个——一道补救都没有,无条件泄漏。**
所以"手写 `rm` 是当前的兜底"这个说法只对 20% 的面成立。撤回那 8 处的条件:

1. 客户端修复已发布并在真实 home 上验证(§5.4);
2. **确认支持的最低客户端版本已越过该发布**——这里没有 Lua 侧能力可探测
   (`xvm.files` 早就存在,变的是客户端的回收行为,Lua 看不见),
   所以 `reference_recipe_capability_probe` 的探测手法**不适用**,
   只能靠版本下限决策。

在此之前:手写 `rm` 是幂等的(删一个已经删掉的链接是 no-op),**保留**。
但注释要改——现在写的是"确认客户端会回收后再删这几行",
应当补上"且它在 detach 路径上从未生效过"(§3)。

### 7.1 D5 的索引侧改动(与上面无关,见 §2.6)

1. `libs/sysroot.lua`:`declare_headers` 加 `opts.merge`,列出的名字改走
   `__declare_tree` 而不是整目录声明一个节点。老客户端不受影响
   ——它走的是 `install_headers` 回退路径。
2. `glibc.lua` / `linux-headers.lua` 各加 `{ merge = { "scsi" } }`,
   **同一次发布**。`glibc.lua:371-383` 那段"接受 last-one-wins"的注释
   要改成说明为什么这一个名字走合并——它现在的结论是错的
   (它说"变成 doctor 能看见的状态",实际上没有任何 finding 报告它)。
3. 判据不是"装完是绿的",是 §6 表里的 d:两个包的 `scsi` 头必须**同时**可见。

---

## 8. 落地结果(2026.8.26.1)

### 8.1 实现了什么

| # | 内容 | 位置 |
|---|---|---|
| T1 | `release_file_placements()` —— 唯一的成员遍历入口;`release_declares_file_assets_` 改写成它的封装 | `xvm/bindings.{cppm,cpp}`、`xvm/inspect.cpp` |
| T2 | `cleanup_removed_xvm_file_artifacts` —— 同族第三个成员,接入**完整卸载**与**安装(重新注册)**两处 | `xim/installer.cpp` |
| T3 | `detach_current_subos_` 三行全部改走成员;成员元数据一并摘除;回退版本的资产补齐 | `xim/installer.cpp` |
| T4 | `place_asset` 祖先 payload 链接护栏(**转换而非拒绝**) | `xvm/commands.cpp` |
| T5 | doctor 悬空扫描递归覆盖 `usr/`/`etc/`/`share/`;渲染按 payload 折叠;`--fix` 与回收路径共用空目录回收 | `xself/doctor.cpp` |
| T6 | `use` 回收进入方不声明的资产,不再进 `stranded` 报告 | `xvm/switch_plan.{cppm,cpp}`、`xvm/commands.cpp` |
| — | **回收策略只有一份**:`xvm::reclaim_declared_assets`,四条路径共用 | `xvm/commands.{cppm,cpp}` |

测试:`tests/unit/test_sysroot_assets.cpp`(14 例)、
`tests/e2e/declared_file_assets_removal_test.sh`(S1-S4,已注册为 E2E-94)。
规范:`docs/spec/xlings-json-schema.md` 增加「声明式文件资产的生命周期」一节,
写明命名规则、四条路径的统一回收表、以及**判据不能用 `-e`**。

### 8.2 被实测推翻的五条判断(原文保留,这里逐条纠正)

**① §4.4 说 doctor 的「应有集」只看 `st.ws[target]`、没走成员,是「同一个 bug 的第三个副本」。错了。**
files 成员**本身就是 workspace 条目**(实测 `default` 的 268 条里 140 条是
`.files.`),所以 `doctor.cpp:1396-1412` 那个循环早就是完整的。
缺的只有「实有集」的递归。这条判断如果照做,会去改一段本来就对的代码。

**② §4.4 提的「全量对账 → 孤儿 finding → `--fix` 删除」不能做。**
实测:`default` 有 **463** 条指向真实 payload、但本 subos 无任何声明的链接
(`gfxbuild` 183 条),来源是 `xorgproto`/`musl`/`alsa-lib` 等**在别的 subos
才 active** 的包。它们当前可用;按孤儿清掉会删掉 463 个正在工作的头文件。
那是**另一个缺陷**(资产被放进了没有声明它的 subos),不是 #423。
所以 T5 收敛为只递归悬空扫描 —— 悬空的已经坏了,删除零风险。
未声明但可用的那一类**本轮不做**,留作独立 issue。

**③ §4.2 的三分支判定是对的,但漏了一处入口。**
`Installer::execute` 的安装路径**早就**调用 lib/program 两个 cleanup
(`installer.cpp:1935/2151`),同样缺 files 那个。补上之后,一次
**重新注册**(新版本不再声明某个目的地)也会回收 —— 这正是 §2.6 粒度迁移
能够收敛的原因,而且与安装顺序无关。原方案没看到这处。

**④ §2.6 说护栏应当「拒绝写入」。拒绝会把迁移堵死。**
两个包共享一个目录时,先改的那一方要往对方的目录链接里写叶子,拒绝 =
头文件永远落不下去。改成**无损转换**:把目录链接换成真目录,并把原 payload
目录里的每一项各自补一个链接。两个包无论谁先装,都不丢东西。
(空写测试实测过:不加护栏时 `sg.h -> /etc/hostname` 真的落进了对方 payload。)

**⑤ e2e 的「对账」判据单独用不足以证伪 detach 那一半。**
拿**修复前**的二进制跑 S2,对账**通过**了 —— 因为旧行为把
`leafpkg.files.1..3` 也留在 workspace 里当 active,于是泄漏的链接按对账的
定义就是「已声明」。**一个 bug 能满足的判据不是判据。**
补了一条不依赖元数据的断言:交出一个发布之后,本 subos 不得再有任何链接
指向那个发布的 payload。旧二进制在这条上失败,新二进制通过。

### 8.3 实测(真实 home 切片,`.agents/tools/slice-real-home.sh`)

```
doctor 报告(修复前二进制)  dangling sysroot link × 0      ← 深度 1 看不见
doctor 报告(修复后二进制)  2 行,覆盖 7 条链接           ← 按 payload 折叠
doctor 耗时                 1.32s                          ← 无性能回归
doctor --fix 后             悬空 0 条,空目录已回收,usr/include 保留
verify-untouched            OK: 真实 payload store 未被改动
```

单测 48 个二进制全过;12 个最相关的 e2e 全过
(`declared_file_assets_removal` / `subos_alias_sysroot` / `xvm_files_probe_compat` /
`subos_install_remove_isolation` / `remove_orphan_payload` / `doctor_fix_convergence` /
`self_doctor` / `self_doctor_depth` / `self_doctor_multi_subos` /
`group_switch_report` / `subos_env_use_switch` / `install_use_semantics`)。

### 8.4 本轮**没有**做的

- **D5 的索引侧**(`declare_headers` 的 `merge` 选项 + glibc/linux-headers 的
  `scsi`)—— 客户端护栏必须先发布(§6 的 a 先于 c)。客户端这一半已在本次。
- **未声明但可用的 sysroot 链接**(§8.2 ②,实测 646 条)—— 独立 issue。
- **`cleanup_removed_xvm_library_artifacts` 的「有幸存者就跳过」** —— 同样会
  把泄漏换成悬空,但影响的是另一批状态,拆开走(§6 风险)。
- **`xvm::remove_asset` 仍用 `remove_all`** —— 新代码一律 `fs::remove`;
  旧函数还在 `use` 路径上用着,Windows junction 在 `remove_all` 下是否跟随
  **未验证**,不在没测过之前动它。
