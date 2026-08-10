# #532 / #534 / #537 / #540 / #541 深度分诊:两个根因,一个假问题

> 全部结论在 2026-08-11 于**用户真实 home**(`/home/speak/.xlings`,124 包 / NVIDIA
> RTX 4080 / 550.144.03 / X11)上实测复现,客户端 `2026.8.10.5`(分支
> `fix/doctor-hang-and-537`,PR #538 未合并;已发布最新版仍是 `2026.8.10.4`)。
>
> 本文只写**被测量证实的**部分。凡是我推理出来但没测的,都标了「未测」。

## 0. 一句话

五个 issue 里,**#534 的根因是假的**(它测的是探针自己的标签),**#532 是它的真身**;
剩下三个各自真实,但 #541 的四条缺陷是**同一个根因的四张脸**:
「这个包装了吗?」在一个二进制里有**四个回答方、四份数据源、零对账**。

顺带在实测中发现一件比这五个 issue 都严重、且**没有任何 issue 记录**的事:
这台机器上图形栈**装了个寂寞**——四个载荷在盘上,版本库里一条记录都没有,
每个 subos 的 `lib/` 里**零个 GL 库**,而 `xlings info` 说 `graphics` 已安装、
`.wiring` 说六个 vendor 全部 `native`(=通过)。**四条独立通道一致地报告健康。**

---

## 1. 逐条分诊

| Issue | 判定 | 依据 |
|---|---|---|
| **#534** EGL/GLES 降级到 CPU | **根因为假,症状归入 #532** | interposer 今天带的是 DT_RPATH;矩阵探针自己是 DT_RUNPATH |
| **#532** 构建产物跑不起来 | **真,今日逐字复现** | 产物 `RUNPATH` 只有 glibc+gcc;`-lz` 链接成功、运行失败 |
| **#537** 面板低报 | **真,已修但未发布** | 读取侧完整;**写入侧(index)不在任何本地快照里** |
| **#540** mcpp 要标签不要路径 | **真,是设计约束不是缺陷** | `gcc -print-prog-name=ld` → `ld`(走 PATH)已复现 |
| **#541** 四条闭环 | **真,但根因比四条更浅** | 四个回答方读四份源,实测同时互相矛盾 |

### 1.1 #534:根因是假的,而且假在一个我们已经踩过三次的地方

issue 说「interposer 携带 DT_RUNPATH,而 RUNPATH 不传递」。**今天盘上不是这样**:

```console
$ readelf -d <store>/xim-x-nvidia-gl-host-link/0.1.2/lib/libEGL_nvidia.so.0
 (RPATH)   Library rpath: [...]          ← 不是 RUNPATH
 (NEEDED)  Shared library: [/lib/x86_64-linux-gnu/libEGL_nvidia.so.0]
```

那 48 格矩阵为什么全是 llvmpipe?**因为探针自己是 DT_RUNPATH**:

```sh
# .agents/tools/graphics/matrix.sh:240
gcc -O0 -o '$bin' '$src' -ldl        # 没有 --disable-new-dtags ⇒ 产物是 DT_RUNPATH
```

`report_tag_differential`(后加的)只测 **egl 一个探针**,主表从未在 DT_RPATH 下重跑过。
所以矩阵里每一个 subos 格子测的都是「一个 DT_RUNPATH 消费者看到什么」。

**这是同一个陷阱在这个文件里的第四种形态**:`graphics-acceptance.sh` 已经因为它错过一次
(#537 顺带记录),现在轮到 `matrix.sh` 的主表。**测量工具与被测对象共享同一个错误假设时,
它们会一致地错。**

**#534 的真实残留有两块,都不该留在 #534 里:**

- **用户自己构建的程序**确实是 DT_RUNPATH,确实拿不到 GPU —— 这**就是 #532**,不是另一个 bug。
- **`egl-device` 无 DISPLAY 时 `eglInitialize` 失败** —— 从未在 DT_RPATH 下测过。
  按 harness 自己的规矩(`not-measured` 一律算失败),这一格现在是**未知**,不是「坏」。

> **动作**:#534 关闭,正文改写为「根因见 #537,构建侧见 #532」;`egl-device` 那一格
> **单独开一个 issue**,并且**在重测之前不许写根因**。

### 1.2 #532:真,且是本轮唯一「用户天天撞、我们完全没修」的那个

今天在 `default` subos 里逐字复现,两条都成立:

```console
$ xlings subos use default --cmd "gcc -o /tmp/t /tmp/t.c && ..."
$ readelf -d /tmp/t
 (RUNPATH)  [<store>/xim-x-glibc/2.39/lib64:<store>/xim-x-gcc/16.1.0/lib64]
                                            ↑ 只有两个载荷,没有 <subos>/lib

$ ... --cmd "gcc -o /tmp/z /tmp/z.c -lz && /tmp/z"
LINK_Z_OK
/tmp/z: error while loading shared libraries: libz.so.1: cannot open shared object file
```

**B 完全成立**。`-lz` 能链上是因为 `<subos>/lib/libz.so` 在 `-L` 里;跑不起来是因为
`<subos>/lib` 不在产物的 RUNPATH 里。**链接期看得见、运行期看不见**——这个不对称本身
就是缺陷的形状。

**A 今天没能以原样复现**,但不是因为它被修了:

```console
$ ... --cmd "gcc -o /tmp/gl /tmp/gl.c -lGL"
ld: cannot find -lGL: No such file or directory
```

不是「libGLdispatch 找不到」,是**根本没有 libGL.so 可链**——`<subos>/lib` 里
一个 GL 库都没有(见 §2)。A 的机制(ld 不搜输入 `.so` 自己的 DT_RUNPATH)没有被证伪,
它只是**被一个更大的缺陷盖住了**。修完 §2 之后必须重测 A。

### 1.2.1 落地期实测追加:subos 里**连 hello world 都链接不了**

比 #532 说的更靠前一步。#532 说「链得上、跑不起来」;实测这台机器上今天**连不上**:

```console
$ xlings subos use default --cmd "gcc -o /tmp/t /tmp/t.c"
ld: cannot find crt1.o: No such file or directory
ld: cannot find crti.o: No such file or directory
```

**根因:subos 工具链没有任何指向自己 farm 的库搜索路径。**

```console
$ ... --cmd 'echo "[$LIBRARY_PATH]"'
[]                                        ← 空
$ ... --cmd 'gcc -v -o /tmp/t /tmp/t.c 2>&1 | grep -oE "\-L[^ ]+"'
-L<gcc载荷>/lib/gcc/... -L/lib -L/usr/lib  ← 没有 <subos>/lib,也没有 glibc 载荷
```

`crt1.o` 就在 `<subos>/lib/crt1.o`(指向 glibc 2.44 载荷,链接有效)。**补一条搜索路径就好:**

```console
$ ... --cmd 'LIBRARY_PATH="$XLINGS_SUBOS_LIB" gcc -o /tmp/t /tmp/t.c && /tmp/t'
WITH LIBRARY_PATH: BUILD+RUN OK
```

**不是本轮改动引入的**:用**已发布的 2026.8.10.4** 逐字复现同样的失败。

> **未落地,需要决定。** 一行 `LIBRARY_PATH=$XLINGS_SUBOS_LIB` 就能修好,而且它**不写进产物**
> ——与 `-rpath-link` 同一类。但它**会改变链接期选中哪一个库**(farm 里的 libz 还是消费方
> 自己的),这一点比 `-rpath-link` 强,属于 #540 要求「必须可声明退出」的那一类语义。
> 所以放进 E2 一起谈,不在本轮悄悄塞进去。

### 1.3 #537:修对了,但只修了一半的仓库

xlings 侧(PR #538)**完整**:三态判定 `needs-transitive-consumer`、面板渲染
(`graphics.cppm:412`、`subos.cppm:1751`)、单测、`graphics-acceptance.sh` 双标签探测,
都在。

**但写入侧不在**。`.wiring` 的 state 是 **xim-pkgindex 的 `graphics` recipe 在装机时写的**,
xlings 只读。本地五个索引快照里:

```console
$ grep -rl "needs-transitive-consumer" ~/.mcpp/registry/index-snapshots/xim-pkgindex/*/pkgs/g/graphics.lua
(无)
$ grep -n "state=broken" .../df01c87/pkgs/g/graphics.lua      # df01c87 = 最新快照
263:  ... " state=broken reason=runpath-not-transitive")
```

**最新快照仍然写 `broken`。** 所以今天把 PR #538 合并发布,面板**仍然低报**——
它能渲染新状态,但没有人写这个状态。

> **动作**:#538 与 xim-pkgindex#598 必须**成对发布**,且发布前用 §5 的判据验证
> 记录里真的出现了 `needs-transitive-consumer`。

### 1.4 #540:约束成立,而且有一个双方都不用让步的解

已复现 #540 的关键测量:

```console
$ xlings subos use default --cmd "gcc -print-prog-name=ld"
ld                      # 载荷里没有自带 ld ⇒ 从 PATH 解析 ⇒ 就是 xim binutils 那个
```

所以 E2b 的包装器**确实会作用到 mcpp**,#540 的担心不是假设。

**但这条谈判其实不必发生**,因为 E2b 追加的三样东西里,**只有一样会写进产物**:

| 追加项 | 写进产物? | 对 mcpp 有害? | 结论 |
|---|---|---|---|
| `-rpath-link <dir>` | **否**(纯链接期) | 否 | **无条件加**。它单独就修掉 #532 A |
| `--disable-new-dtags` | 是(标签) | 否(mcpp 自己也在做,同向) | 默认加 |
| `-rpath <dir>` | **是(路径)** | **是**(带 libc 的目录进 RPATH;`pack` 必须剥) | **必须可声明退出** |

`-rpath-link` 只影响链接期解析输入 `.so` 的 DT_NEEDED,**不产生任何 DT_RPATH/RUNPATH 条目**。
这意味着 **#532 A 的修法对 mcpp 完全无害**,可以先落地、不需要等这场谈判。

> **动作**:E2b 拆成两个可独立控制的部分(标签组 / 路径组),`-rpath-link` 归入无条件组。
> 详见 §4。

### 1.5 #541:四条缺陷是一个根因的四张脸

见 §3。

---

## 2. 实测中发现的、没有任何 issue 记录的事:图形栈装了个寂寞

这是本轮**最严重**的发现,也是解释用户「为什么还是软件渲染」的直接原因。

### 2.1 现场

```console
$ xlings info xim:graphics
  selected installed  yes          ← 报告已装

$ xlings info xim:nvidia-gl-host-link
  selected installed  no (package not installed)
$ ls <store>/xim-x-nvidia-gl-host-link/0.1.2/lib | wc -l
50                                 ← 但盘上有 50 个库
```

版本库(`~/.xlings/.xlings.json`,387 KB)里:

```
"nvidia"   出现 0 次
"libglvnd" 出现 0 次
"graphics" 出现 0 次
```

每一个 subos 的 `lib/` 里:**零个 GL 库**(`libGL.so*` / `libEGL*` / `libGLES*` 全无)。

而 `.wiring` 说:

```
vendor=libEGL_nvidia.so.0     state=native     ← native = 「我们自己的构建,没有宿主闭包要查」
vendor=libGLX_nvidia.so.0     state=native
vendor=libGLESv1_CM_nvidia.so.1 state=native
vendor=libGLESv2_nvidia.so.2  state=native
```

`native` 在面板上是**通过**(`is_ok()` 对 `native` 返回 true)。

**四条独立通道——`info`、版本库、subos farm、`.wiring`——全部报告健康或沉默,
而这个栈没有接到任何地方。**

### 2.2 为什么 `info` 说 graphics 装了

`collect_package_inventory` 的数据源是 xvm workspace 记录,**外加一条例外**:
「没有可运行目标的包没有 workspace 记录,靠浅扫两个 store root 找**打了戳的载荷**」。

```console
$ ls -A <store>/xim-x-graphics/0.1.3/
.xim-installed        ← 有戳
$ ls -A <store>/xim-x-nvidia-gl-host-link/0.1.2/
lib                   ← 无戳
```

**戳是一句声明,不是一份证据。** `graphics` 有戳 ⇒ 报告已装;它的每一个组件无戳 ⇒
报告未装。而戳是 `installer.cppm:2863` 的 auto-stamp 在 install_dir 为空时打的——
**它恰恰是「这个包什么都没装」的标记,却被当成「这个包装好了」的证据。**

### 2.3 `.wiring` 描述的是别人,却没有任何失效机制

时间戳把顺序说得很清楚:

```
01:37:16  <store>/xim-x-graphics/0.1.3/.xim-installed     戳
01:37:17  <libglvnd>/lib/glx-vendor/.wiring               记录写下
01:38:14  <store>/xim-x-nvidia-gl-host-link/0.1.2/        它描述的载荷,晚 57 秒才写完
23:32(前一天) <subos>/default/lib                        整轮安装根本没碰 subos
```

**记录在它所描述的载荷写完之前 57 秒就写好了。**

根子在于:`.wiring` 由 `graphics` 的 config hook 写,内容却是**关于 mesa / libglvnd /
nvidia-gl-host-link 的**。这些包升级时,没有任何机制让 `graphics` 的 hook 重跑。
memory 里已经记过这条的一半(「必须换版本号 0.1.3 而不是改 0.1.2,否则 hook 不会重跑」),
但那只解决了 **graphics 自己**变化的情况,没解决**它描述的别人**变化的情况。

### 2.4 写入侧还有两个独立的静默错判

**(a) 工具缺失与「是我们自己的」不可区分。**

```lua
function graphics.host_vendor_behind(interposer)
    local dyn = os.iorun([[readelf -d "..."]])
    if dyn == "" then return nil end          -- ← 工具没跑成
    ... if soname:sub(1,1) == "/" then return soname end
    return nil                                 -- ← 真的是我们自己的
end
```

两个 `nil` 含义相反,调用方一律判成 `state=native`(通过)。
**它的兄弟函数 `vendor_closure_gaps` 恰好防住了这一条**,注释写得清清楚楚:
「`os.iorun` 失败时返回 ""……它不是闭包为空的证据」。守卫存在,就在隔壁一个函数。

**(b) 越是该查的 vendor,越会被判成 native。** 0.1.2 之后 GLX/GLESv1/GLESv2 是
**直接指向宿主库的裸符号链接**:

```console
libGLX_nvidia.so.0 -> /lib/x86_64-linux-gnu/libGLX_nvidia.so.0     (无绝对 DT_NEEDED)
libEGL_nvidia.so.0  = 21 KB 真文件                                  (有绝对 DT_NEEDED)
```

裸符号链接没有绝对 DT_NEEDED ⇒ `host_vendor_behind` 返回 nil ⇒ `native` ⇒
「我们自己的构建,没有宿主闭包要查」。**它字面上就是宿主驱动。判定对这三个 vendor 是反的。**

---

## 3. 根因:「这个包装了吗?」有四个回答方

#541 的四条不是四个 bug,是一个结构在四个命令上的投影。

| 谁在问 | 读哪份源 | 代码 |
|---|---|---|
| `install`「要不要跑 hook」 | **store 目录存在且非空** | `catalog.cppm:474` → `resolver.cppm:144` → `installer.cppm:2695` |
| `info` / inventory | **xvm workspace 记录 + 打戳载荷浅扫** | `inventory.cppm` |
| `remove`(不带版本) | **当前 active binding** | #541 ② |
| 加载器(真正决定能不能跑) | **`<subos>/lib` 盘上的符号链接** | — |

这四份源今天在这台机器上**同时互相矛盾**:

| 包 | store 载荷 | `.xim-installed` | 版本库 | subos 接线 | `xlings info` |
|---|---|---|---|---|---|
| graphics 0.1.3 | 4 项 | **有** | 无 | 无 | **已装 ✓** |
| libglvnd 1.7.0.1 | 5 项 | 无 | 无 | 无 | 未装 |
| nvidia-gl-host-link 0.1.2 | **50 个库** | 无 | 无 | 无 | 未装 |
| mesa 25.0.7.2 | 6 项 | 无 | 无 | 无 | 未装 |

**四条症状是这一条根因的四个出口:**

- **① 失败仍登记 installed、且不可重试。** hook 失败留下非空目录 ⇒ `install` 读的那份源
  说「已装」⇒ 永远跳过 hook。**修复命令自己的前置条件,被它要修的残骸满足了。**
- **② `remove` 不带版本打的是 active。** 因为 remove 读的是第三份源,它指向的版本
  和坏掉的那个不是同一个。
- **③ `use` 不重放 files。** ⚠️ **实现期证伪,见 §3.2。**
  今天这台机器是它的极端形态:**盘上一个 GL 库都没有**。
- **④ install 用了旧索引快照。** 换了个轴(时效),同一个族:命令读的不是刚更新的那份。

> **判据**:一个包的「已安装」必须**只有一个回答方**,其余全部改成向它提问。
> 载荷在盘上但没有记录 —— 这不是「已安装」,是**需要修复**,而且必须**说出来**。

## 3.2 更正:#541 ③「use 不重放 files」在 xlings 侧不成立

落地过程中去查代码,结论与 issue 相反,记在这里而不是悄悄绕过:

* `xvm/commands.cppm:549-551` 对每个 switch 都 materialize `installLibSource`
  **和** `installFileSource` —— `use` 会重放 placement。
* 真实 home 里 **381 个 `files` 节点的 `fileDst` 全部在 `usr/` 下**,没有一个落在
  白名单之外,也就没有一个被 `file_placement` 静默丢掉。
* `<subos>/lib` 这个 farm **不是 `files` 建的**,是 `lib`-kind 节点经
  `library_placement` 建的(实测 `libz.so -> <store>/xim-x-zlib/1.3.1/lib/libz.so`,
  DB 里 `kind: "lib"`),而 `use` 同样重放它。

那报告者看到的「`use` 之后盘上还是 0.1.1」是什么?**最可能是 recipe 的 `config()`
用 `sysroot.declare_libs` **直接创建**了符号链接,而那些链接根本不在 xvm 的 placement
模型里。`use` 不可能重指它不知道的东西。**这条属 index 侧,不属 xlings 侧。**

xlings 侧真正缺的那一条是可观测性:**没有任何检查发现 `<subos>/lib` 里存在无人管理的
条目**。列为后续(doctor:unmanaged farm entry),本轮不做——因为这台机器上
nvidia 根本没有注册记录,做了也验证不了,而没验证过的守卫就是下一个静默成功。

## 3.3 另一个已量化的欠账:E1a 迁移从来没做

```console
$ 统计 <store>/*/*/bin/* 的标签
   17 (RPATH)      ← 0.0.57 之后装的
   24 (RUNPATH)    ← 之前装的,没人重打
```

#537 的前提是「0.0.57 之后已装程序都是 DT_RPATH」。**在真实机器上这句话 41% 成立。**
设计文档点过这笔账(「唯一真实成本:需要一次全量重打或迁移策略」),然后没有做。
这意味着**今天仍有 24 个已装可执行文件拿不到 GPU**,而面板不会为它们说任何话。

---

## 4. 方案

按「先让状态可信,再让构建可用」排序。**不建议先修 #532** —— 在一个接线为空的
home 上做构建侧验收,拿到的结论不可信。

### P0 —— 让「已安装」只有一个回答方(解 #541 全部四条 + §2)

**P0-1 统一判据。** 定义一个 `installation_state(pkg, version)`,返回三态而非布尔:

| 状态 | 条件 | 三个命令各自怎么做 |
|---|---|---|
| `absent` | 无载荷、无记录 | install 跑 hook;info 说未装;remove no-op |
| `installed` | 载荷有内容 **且** 记录齐全 | install 跳过;info 说已装;remove 走完整路径 |
| `incomplete` | **载荷与记录不一致(任一方向)** | install **强制重跑 hook**;info **明确报出**;remove 按盘清理 |

`incomplete` 是关键的新格子——今天这个状态存在于机器上,却没有任何词能描述它,
于是被两个命令各自归入相反的布尔。

**P0-2 `install` 不再把「目录非空」当作已装。** 目录非空只说明**有东西**,不说明
**装成了**。判据换成「记录齐全」,`incomplete` 一律重跑 hook。这一条单独就解开 #541 ① 的闭环。

**P0-3 hook 失败必须回滚记账,或登记为 `incomplete`。** 二选一,不能两样都不做。

**P0-4 auto-stamp 不再冒充证据。** `.xim-installed` 今天的语义是「install_dir 为空但这是合法的」。
它**不能**同时充当 inventory 的「已装」证据。要么给它加上「这个包声明了自己没有载荷」的
显式来源,要么 inventory 改为不信它。

**P0-5 `use` 对 `files` 节点重放落盘**(#541 ③)。`active` 这个词对程序和对库必须同义。

**P0-6 `remove` 不带版本时**:要么清该包全部版本,要么在 hint 里带上版本。
现在的 hint(「uninstall it first, then install again」)在 ① 的场景下是**错的指引**。

### P1 —— 让派生记录不能比它描述的东西更旧

**P1-1 `.wiring` 记录 provenance。** 记下它读到的每个 vendor 载荷的 **identity + version**。
读取方发现盘上的版本与记录不符 ⇒ 报 `stale`,**不报健康**。

**P1-2 `host_vendor_behind` 区分「工具没跑」与「是我们自己的」**(§2.4a)。
照抄它兄弟函数已有的守卫。工具缺失 ⇒ `unverified`,不是 `native`。

**P1-3 裸符号链接 vendor 不判 `native`**(§2.4b)。`native` 的判据应当是
「**这个载荷由我们构建**」,不是「readelf 里没有绝对路径」。前者是事实,后者是它的
一个不可靠代理。

**P1-4 #538 与 xim-pkgindex#598 成对发布**(§1.3)。

### P2 —— 构建侧(#532 + #540),E2 落地

**先答 #532 的三个未定问题:**

1. **写 `<subos>/lib` 还是逐载荷绝对路径?** → **`<subos>/lib`,单条。**
   它跟随 `xlings use`;逐载荷会把版本冻进用户的每一个二进制。与三层模型不冲突,
   因为**冻结属于发布(`mcpp pack`),不属于开发(`gcc`)**。单条也让 RPATH 有界——
   #540 附的 specs 现场(~40 条来自已删除沙箱的 `/tmp` 路径)就是无界累积的样子。

2. **RPATH 还是 RUNPATH?** → **可执行文件 DT_RPATH,共享库 DT_RUNPATH。**
   与 E1a 打包侧**同一条分界**。两个写入者按构造一致,而不是碰巧一致。

3. **谁是唯一回答方?** → **契约(E2a `XLINGS_SUBOS_LIB`)是唯一事实源,两个写入者都读它。**
   elfpatch 管打包期,包装器管构建期;**标签策略同源,rule E 验证**。这本来就是设计文档
   §3 的架构,只是构建侧一直没实现。

**再给 #540 一条不必谈判的退出:**

E2b 拆成两组,**只有会写进产物的那一样需要退出**:

```sh
# 无条件组(不写进产物,mcpp 无损)
-rpath-link "$XLINGS_SUBOS_LIB"          # 修 #532 A,可先独立落地

# 标签组(默认开,同向,后出现者胜出)
--disable-new-dtags

# 路径组(写进产物 ⇒ 必须可声明退出)
-rpath "$XLINGS_SUBOS_LIB"               # XLINGS_SUBOS_LD_PATHS=0 关闭
```

- 退出是**声明**(`subos_info.envs` 里的显式开关),不是推断,也不是靠改写 spec 对抗默认值 —— 满足 E1c 立的规矩。
- 包装器必须**记录它做了什么**(仿 mcpp `resolution.json` 的 `loader_tags`)。
  否则「没加路径」和「包装器根本没跑」又会输出相同的东西——本仓库的老毛病。
- **`-rpath-link` 可以先走**:它单独修掉 #532 A,对 mcpp 零影响,不需要等这场谈判。

### P3 —— 测量工具不能和被测对象共享假设

**P3-1 `matrix.sh` 主表两种标签各跑一遍**(§1.1),不能只在 `report_tag_differential` 里
测一个 egl 探针。**这是让 #534 那类结论不再自我印证的唯一办法。**

**P3-2 E1a 迁移**(§3.3):对 24 个 DT_RUNPATH 的已装可执行文件做一次全量重打,
或提供 `xlings self doctor --fix` 能修的路径。先跑一遍矩阵存基线。

---

## 5. 验收判据(每条都必须先证伪)

| 项 | 判据 |
|---|---|
| P0 | 造一个「载荷在盘、记录全无」的包:`info` 必须报 `incomplete`(不是「未装」),`install` 必须重跑 hook(不是「已装」跳过) |
| P0 | 故意让 install hook 返回 false:再装一次**必须**重跑 hook |
| P0-5 | `xlings use <pkg> <ver>` 后,`readlink -f <subos>/lib/<lib>` 必须指向新版本 |
| P1-1 | 手工把 vendor 载荷换成另一版本:`subos info` 必须报 `stale`,不得报健康 |
| P1-2 | 把 `readelf` 从 PATH 拿掉再装:记录必须是 `unverified`,不是 `native` |
| P1-4 | 发布后在真实 home 上 `grep needs-transitive-consumer .wiring` 必须命中 |
| P2 | 用户零 flag:`gcc -lz && ./a.out` 跑通;`gcc -lGL` 链接通过 |
| P2 | `XLINGS_SUBOS_LD_PATHS=0` 时产物**不含**任何 store 绝对路径,但标签仍是 RPATH |
| P3-1 | 矩阵主表在 DT_RPATH 下重跑,48 格无 `UNMEASURED` |
| §1.1 | `egl-device` 无 DISPLAY 一格在 DT_RPATH 下重测后**才**允许写根因 |

**「先证伪」不是仪式**:§2 里四条通道一致地报告健康,正是因为没有人问过
「如果它其实是坏的,这条通道会变红吗?」

---

## 6. issue 处置建议

| Issue | 建议 |
|---|---|
| **#534** | **关闭**,正文改为「根因见 #537(消费者标签),构建侧见 #532」。`egl-device` 无显示一格另开 issue,重测前不写根因 |
| **#532** | **保留,升为 P2 主线**。补记:A 今天被「farm 为空」盖住,修完 P0 必须重测 |
| **#537** | **保留至 #538 + xim-pkgindex#598 成对发布**;补记写入侧未随读取侧落地 |
| **#540** | **保留**,补上 §4 的两组拆分——`-rpath-link` 无需谈判即可落地 |
| **#541** | **保留,升为 P0**;正文补上「四个回答方」这一层根因,四条降为它的表现 |
| **新开** | **图形栈已安装但未接线**(§2)——比 #541 更严重,且四条通道全部报告健康 |
| **新开** | **E1a 迁移欠账**(§3.3)——24 个已装可执行文件仍是 DT_RUNPATH |

---

## 7. 这份分诊自己的边界

- **一台机器,一种硬件**(NVIDIA / X11 / x86_64)。E5 的跨硬件覆盖仍然没做。
- **§2 的现场可能是 #541 手工复现的残留**。它证明了这些状态**可以**存在、并且
  存在时四条通道都不报警;它**没有**证明一次干净安装也会这样。
  **P0 落地前应当先在隔离 home 上做一次干净安装,确认是否复现**——如果干净安装也这样,
  严重性还要再升一级。
- **#532 A 的机制未被证伪也未被重新证实**,它当前被更大的缺陷遮住。
- 写入侧(xim-pkgindex)的改动都在**另一个仓库**,§1.3 的成对发布是硬约束。
