# Issue #605 分析:xlings 的 node 加载不了依赖 libresolv.so.2 的原生模块(sharp)

- Issue: https://github.com/openxlings/xlings/issues/605 (FarnaHerry, 2026-09-16)
- 结论:**是 xim-pkgindex 侧可修的问题**(node 配方),根因在 xlings 加载器设计与动态链接器搜索规则的交汇处;不是 sharp 依赖声明问题,也不是安装失败。
- 修复 PR:https://github.com/openxlings/xim-pkgindex/pull/851(分支 `fix/node-glibc-compat-sonames`)

## 1. 复现(本机实测,x86_64,xlings 2026.9.16.1)

```
npm install sharp            # sharp 0.35.4, @img/sharp-libvips-linux-x64 1.3.3
<xim-x-node/<ver>/bin/node> -e "require('sharp')"
→ ERR_DLOPEN_FAILED: libresolv.so.2: cannot open shared object file
```

| node 载荷 | node 的 tag | 结果 |
|---|---|---|
| 22.17.1 | DT_RUNPATH | 失败 |
| 24.15.0 | DT_RUNPATH | 失败 |
| 26.7.0  | **DT_RPATH** | **同样失败** |

最后一行是关键:0810.4 的 tag 契约(可执行文件打 DT_RPATH)**救不了这个场景**。
这和 2026-09-07 的 node-pty/libutil.so.1 不是同一种形状(那次 pty.node 自己没有
RUNPATH,换成 DT_RPATH 的 node 就好了)。

## 2. 根因:三个条件同时成立

加载链:`node --dlopen--> sharp-linux-x64.node --NEEDED--> libvips-cpp.so.8.18.6 --NEEDED--> libresolv.so.2`

```
libvips-cpp.so.8.18.6   NEEDED libresolv.so.2   RUNPATH [$ORIGIN/]
sharp-linux-x64.node    RPATH  [$ORIGIN/../../sharp-libvips-linux-x64/lib:...]
node (elfpatch 后)       INTERP xim-x-glibc/<v>/lib64/ld-linux-x86-64.so.2
                        RPATH/RUNPATH [node/lib : xim-x-glibc/<v>/lib64 : gcc-runtime : subos/lib]
```

1. **携带 DT_RUNPATH 的对象只按自己的 RUNPATH 查找。** glibc 的规则:请求方有
   DT_RUNPATH 时,整条加载者链(包括可执行文件)的 DT_RPATH 全部跳过。libvips-cpp
   的 RUNPATH 是 `$ORIGIN/`,所以 node 的路径(无论什么 tag)根本不会被问到。
2. **我们的 ld.so 没有兜底。** xim-x-glibc 的 ld.so 编进去的缓存和默认目录是
   指向不存在的路径(`strings` 可见):2.39/2.44 是构建机路径(意外),2.44.2 是有意保留的
   `/nonexistent/xlings-use-rpath-not-default-search/...`。两种情况都不读 `/etc/ld.so.cache`、不搜 `/lib64`(详见 §9)。宿主系统上靠 ld.so.cache 兜住的
   `libresolv.so.2`,在我们的加载器下不存在任何路径能找到。
3. **sharp 预编译产物仍 NEED libresolv.so.2**(glibc 2.34 之前的链接习惯)。
   xim-x-glibc 2.39 / 2.44 / 2.44.2 的 lib64 **都自带**这个文件,只是找不到。

issue 里的根因分析基本正确;两处需要更正:
- "node 的 RUNPATH 只对直接依赖生效" —— 准确说法是"**请求方**带 RUNPATH 时不看任何人的
  RPATH"。即使 node 是 DT_RPATH 也照样失败(见上表 26.7.0)。
- 临时绕过方案把 `xim-x-glibc/2.44.2/lib64/libresolv.so.2` 软链进 sharp 目录:
  **node 实际跑在哪个 glibc 上要看它的 INTERP**(本机的 node 全部指向 2.39,
  同一台机器上 2.44/2.44.2 也装着)。混用不同版本的 libresolv 与 libc 有撞
  `GLIBC_PRIVATE` 的风险;软链应指向 `patchelf --print-interpreter node` 所在目录。

## 3. 各层归属判断

| 层 | 是否能修 | 结论 |
|---|---|---|
| sharp 上游 | 能(去掉 -lresolv) | 不在我们控制内;且同形状的包很多(canvas 等),环境侧必须兜底 |
| xim-pkgindex glibc.lua | 不能 | ld.so 的默认路径是编译期常量,家目录路径因人而异,无法做成可重定位;`relocate_build_paths` 只改脚本不改二进制 |
| xlings / libxpkg elfpatch(通用) | 能 | 对所有切换到我们 INTERP 的可执行文件做同样的事。影响 70+ 个载荷,需要 libxpkg 发版 + 客户端升级链。**作为后续选项记录,本次未做** |
| **xim-pkgindex node.lua** | **能,本次修复** | node 是第三方预编译原生模块的宿主,暴露面最大;变更局部、可验证、可回滚 |
| 用户侧 `LD_LIBRARY_PATH` | 不行 | 会被 node 派生的宿主程序(/bin/sh 等)继承,导致其加载我们的 libc 崩溃(issue 已实测) |
| shim 注入 env(godot 那种 program-scope) | 不行 | 同上,env 会传给子进程 |

## 4. 修复方案(PR #851)

**机制:** 动态链接器在按路径搜索之前,先看同 SONAME 的库是否已经加载。
给 node 加上 `NEEDED libresolv.so.2 libutil.so.1 librt.so.1 libanl.so.1 libdl.so.2 libpthread.so.0`
(glibc 2.34 并入 libc、但仍保留独立文件的那一组),它们随 node 启动从 node 自己的
libc 同目录加载;之后任何深度的原生模块再 NEED 这些名字,都直接命中,且与 node
的 libc 同一版本。没有任何环境变量,不影响子进程。

**在 `config()` 里做,因为:** 安装流程是 install 钩子 → elfpatch → config 钩子
(`installer.cpp:3237` 之后),只有 config 能看到 elfpatch 写好的 INTERP/RPATH;
并且 config 在"已安装"时也会重跑,所以**现有载荷在下一次 `xlings install node@<ver>` 时自愈**。

**守卫(稳定性):**
- 仅当 node 的 INTERP 目录出现在 node 自己的 RPATH 中(= 跑在我们的加载器上)才动;
  宿主加载器(aarch64,glibc 载荷只有 x86_64;或未 elfpatch)不动 —— ld.so.cache 已兜底
- 只加该目录真实存在、且尚未 NEEDED 的 soname → 幂等
- patchelf `--output` 到候选文件,候选必须能跑 `--version`,再 `mv` 原子替换
  (避免 ETXTBSY;正在运行的 node 不受影响)
- 任一步失败:保留原文件、打印带 #605 的一行警告、config 仍返回成功;整个函数另包 `pcall`
- 非 Linux 直接返回;macOS / Windows 行为不变
- patchelf 优先用载荷(`pkginfo.tool_payload_dir`,type() 探测),否则 PATH;都没有则跳过

## 5. 验证

本地(真实家目录切片,`slice-real-home.sh verify-untouched` 确认真实家目录未改动):

| 项 | 结果 |
|---|---|
| `xlings install local:node@24.20.0`(issue 同版本) | NEEDED 增加 4 项;sharp 生成 PNG;`/bin/sh` 子进程、dns、worker_threads 正常 |
| 启动开销 `node -e 0` ×20 | 22/24/26 各自前后一致(12/14/14 ms) |
| 重复 install | inode 与 mtime 不变(幂等) |
| 对已安装的 22.17.1 / 23.6.0 / 24.15.0 / 26.7.0 真实载荷副本跑 config | 全部自愈,sharp 全部可用 |
| 安装期间有 node 进程在跑 | 进程正常结束 |
| `dep-closure-check.sh` | all accounted for |
| `xlings self doctor` | 无 node 相关 finding |
| `pytest -m "static or isolation"` | 3615 passed |
| 新增 5 个行为测试对 main 的 node.lua | 2 个失败(非空转),本分支全过;宿主 gcc 与 xlings gcc shim 两种 `cc` 都跑过 |
| `program-checks/node.sh` | 修复后的 node 通过;去掉 soname 的副本失败 |

**实测到的已知限制(由回退路径兜住):** patchelf 0.18.0 对一个被原地多次修改过的
二进制做 `--add-needed` 会断言失败(`patchelf.cc: shiftFile: 522`)。此时保留原文件并
打印警告,node 本身不受影响。正常载荷(elfpatch 一次)上未出现。

CI(PR 新增 `node native modules` 工作流):ubuntu-24.04 / macos-15 / windows-2022 真装 node(arm 腿见 §7);Linux x64 用一个带 `DT_RUNPATH=$ORIGIN`、NEED
libresolv+libutil 的探针 .so 做 dlopen 差分(对照组副本必须 ENOENT,证明探针确实走到
#605 的路径);三个平台都让 sharp 生成 PNG,均已通过。

测试过程中踩到的一个坑:测试机上 `cc` 是 xlings gcc 的 shim,编出来的二进制本来就
指向 xlings 加载器,导致"宿主加载器"用例实际成了第二个"我们的加载器"用例。
已改为显式设置 INTERP/RPATH,不信任编译器默认值。

## 6. 给用户的补救

PR 合并、索引发布后:

```bash
xlings update
xlings install node@24.20.0    # config 重跑,已安装的载荷就地打补丁
```

发布前的临时方案(比 issue 里的软链更安全,版本与 node 实际的 libc 一致):

```bash
N=$(readlink -f ~/.xlings/data/xpkgs/xim-x-node/24.20.0/bin/node)
PE=~/.xlings/data/xpkgs/xim-x-patchelf/0.18.0/bin/patchelf
"$PE" --add-needed libresolv.so.2 --add-needed libutil.so.1 --output "$N.new" "$N" \
  && "$N.new" --version && mv "$N.new" "$N"
```

## 7. CI 中新发现的既有缺陷:linux-aarch64 上 xim:node 起不来

PR 第一轮 CI 的 `ubuntu-24.04-arm` 腿失败,原因与本修复无关、main 上同样存在:
node.lua 声明 `archs = {"x86_64","aarch64"}`,linux 依赖 `xim:glibc@>=2.39` 与
`xim:gcc-runtime@15.1.0`,而这两个配方都是 `archs = {"x86_64"}`。在 aarch64 上
gcc-runtime 下载 404,glibc 装进来的是 x86_64 产物,node 被锚定到我们的加载器后
缺 `libstdc++.so.6 / libgcc_s.so.1 / ld-linux-aarch64.so.1`(安装日志 rule D 警告),
`node --version` 本身就跑不了。

处理:
- 钩子加了一条守卫:原始 node 自己跑不起来时静默退出 —— 否则警告会把"依赖缺失"
  误报成"预加载失败"(新增测试 `test_preload_is_silent_about_a_node_that_never_started`)
- 工作流矩阵移除 arm 腿并注释原因;宿主加载器分支由伪造载荷测试覆盖
- **aarch64 的 node 依赖问题需另开 issue**(glibc/gcc-runtime 缺 aarch64 资源,或 node 在
  aarch64 上不声明这两个依赖、走宿主加载器)

## 8. 后续(未做)

- **同形状的其他宿主:** 任何跑在我们加载器上、会 dlopen 第三方预编译产物的程序
  (python 的 wheel、bun、electron 类)都有同样暴露面。若再出现,应把这件事上移到
  libxpkg elfpatch,作为"切换 INTERP 到 xlings glibc"的一部分统一处理(一个写者),
  而不是逐个配方复制 —— 0810.4 tag 契约的教训。
- tests/n/test_node.py 的 `test_node_version` 仍断言 `v24.19.0`,而 latest 已是 24.20.0
  (verify 层 CI 不跑,属既有问题,未在本 PR 改动)。

## 9. 追问：最新 glibc 是否会读 xlings 生态的路径？(实测)

`strings` 三个载荷的 ld.so:

| 版本 | ld.so.cache 路径 | 定制 |
|---|---|---|
| 2.39 | `/home/xlings/.xlings_data/.../fromsource-x-glibc/2.39/etc/ld.so.cache`(构建机路径，意外) | 无 |
| 2.44 | 同上(2.44) | 无;还会读宿主 `/etc/ld.so.preload` |
| 2.44.2(latest) | `/nonexistent/xlings-use-rpath-not-default-search/etc/ld.so.cache`(有意保留) | 仅 `glibc-2.44-preload-follows-sysconfdir.patch`:preload 文件跟随 sysconfdir,可用 `XLINGS_LD_PRELOAD_FILE` 重定向 |

结论:2.44.2 的定制**只针对 ld.so.preload**,不涉及库搜索路径;载荷不带 `etc/ld.so.cache`,
默认目录是 `/nonexistent/.../lib/`。**没有任何一个版本会读 xlings 生态路径。**
xlings 生态的库只经 elfpatch 写进的 RPATH(`<dep>/lib64`、`subos/<name>/lib` 农场)被找到。
另:本机 4 个 node 载荷的 INTERP 全部仍是 2.39。

## 10. 核心原因与最佳修复

核心原因：我们的 ld.so 丢了 glibc 自己的"系统库目录"(slibdir)。系统 glibc 找
`libresolv.so.2` 靠的是它在默认目录 `/lib64` 里 —— 与 RPATH/RUNPATH 无关，任何对象都能兜底。
xlings glibc 为了可重定位把 prefix 设成 `/nonexistent`,于是"glibc 自家的库"
也和"宿主的库"一起被切断了。前者是误伤。

| 方案 | 覆盖面 | 代价 |
|---|---|---|
| A. 配方级预加载(PR #851) | 仅 node | 已完成，立即生效 |
| B. libxpkg elfpatch 切 INTERP 时统一加 NEEDED | 所有锚定到我们加载器的可执行文件 | libxpkg 发版 + 客户端链;仅覆盖固定 soname 列表 |
| **C. glibc 补丁：默认搜索目录 = ld.so 运行时所在目录** | 所有进程、所有 glibc 自家库，任意 RUNPATH 深度 | 需重新构建发布 glibc;消费者要重新 elfpatch 到新版本才生效(node 目前全在 2.39) |
| D. glibc 补丁：`XLINGS_LD_LIBRARY_PATH` 仅我们的 ld.so 读取 | 同 C,且可扩展到其他目录 | 依赖环境变量注入;顺序语义要另定 |

推荐 **C 为根治，A 为过渡**。理由：C 恢复的正是系统 glibc 的语义(slibdir 是默认目录),
而 lib64 里只有 glibc 自身的库(实测 56 个文件无第三方库),不会把宿主库放回来，
不破坏"不回落宿主"的设计。实现点：`elf/dl-load.c` 的 `_dl_init_paths` 里 system_dirs
由编译期常量改为 `dirname(GL(dl_rtld_map).l_name)`,并在 secure-exec 下沿用 trusted-dir 规则。
**未原型验证**;C 发布并完成迁移后，A 的预加载变为冗余但无害。

## 11. 方案 C 已落地(2026-09-17)

- PR:openxlings/xim-pkgindex#852(squash `a6c2fe6`),glibc **2.44.3**,`latest` 同步切换;索引发布成功
- 资产:`xlings-res/glibc` release 2.44.3,GitHub 与 GitCode 两端下载后逐字节比对一致,sha256 `b84de544…72845`
- 补丁:`.agents/tools/graphics/patches/glibc-2.44-default-dir-follows-loader.patch`(`elf/dl-load.c` `_dl_init_paths`)
  - 正常启动：取 PT_INTERP 名的目录;直接调用(ldd / `ld.so --list`):`_dl_get_origin()`(/proc/self/exe)
  - 只替换第一个默认目录并同步 `max_dirnamelen`;cache、其余编译期目录、secure 模式可信目录列表不变
  - rtld 内没有 `strrchr`,第一次构建链接失败，改为手写循环
- `build-glibc.sh`:新增行为断言(只有 RUNPATH 的程序能启动并从加载器目录解析 libresolv;宿主独有的 libz 仍不可达;对未打补丁的构建报 2 个问题);新增 `--without-selinux`(configure 链接探测到宿主 libselinux 但 subos 编译器看不到头文件)

### 验证
| 项 | 结果 |
|---|---|
| 探针差分 | 2.44.3 运行成功;2.44.2 `libresolv.so.2: cannot open shared object file` |
| `LD_DEBUG=libs` | system search path = `<payload>/lib64`(含 glibc-hwcaps 子目录) |
| 无 #851 预加载的 node + sharp | 2.44.3 通过;2.44.2 失败 |
| glibc `elf` 测试集(同源码树，打补丁 vs 不打) | 各 533 项，集合完全一致:510 PASS / 17 FAIL / 6 UNSUPPORTED;17 个失败为 ldconfig/cache/container/ABI 尺寸类，与补丁无关 |
| 真实资产经索引安装(切片) | `ldd` 从加载器目录解析 libc;新 subos 记录 `runtime: glibc@2.44.3`(source `index`);node 23 + sharp(有无 #851 均可)通过;g++ 16 线程 + libresolv 程序构建运行通过;去掉 RPATH 仍因 libstdc++ 失败(只有 glibc 自家库获得兜底);doctor 无新增 |
| PR CI | 全绿;linux-install-test 实际安装了 `xim:glibc@2.44.3` |
| 发布后 | `xlings update` 后新建 subos 绑定 `glibc@2.44.3`(source `index`) |

### 生效范围
新 subos、新安装生效;已有 subos 保持原 runtime(pin-to-active),已 elfpatch 的载荷保留原 INTERP,需要 `xlings subos runtime glibc@2.44.3 <name>` 或新建 subos 后重装。

### 过程事故(已修复)
切片测试中在新 subos 里 `install gcc@16.1.0`,gcc 的 config 钩子原地重写了 `specs`、binutils 重写了 `xlings-wrappers/ld`,
切片是硬链接，于是写穿到真实家目录。已按 inode 定位(同 inode = 本次写入)、断开硬链接、用会话早先由真实 gcc 编出的二进制
里的 INTERP/RPATH 作为改动前快照还原，并重新编译比对一致;扫描真实 store 已无切片路径残留。
同期真实家目录里 gradle/jdk-temurin 的改动(00:05)与本次无关(inode 不同、切片中不存在)。

### 顺带发现(未修)
`src/core/xim/commands.cpp:705`:安装失败时仍打印 `X@V installed, but '…' still resolves to …`(提示只看激活版本与请求版本是否不同，不看安装是否成功;退出码正确为 1)。
