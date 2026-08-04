# xlings 作为用户态发行版:hermetic 策略与 GPU/GLIBC 边界设计

**日期**: 2026-08-05
**类型**: 策略 (strategy) — 尚未细化到实施计划,先立框架
**触发**: mcpp-community/mcpp#352(Fedora 44 上 GLFW/OpenGL 程序静默 exit 255,三层根因:观测性 + `compat.glx-runtime` symlink 到宿主 `/usr/lib` 32-bit 库 + 沙盒 glibc 2.39 与宿主 Mesa 要求 GLIBC_2.43 冲突)
**关联**:
- `2026-05-22-subos-sandbox-gpu-passthrough.md`(sandbox 内 GPU 设备节点透传,本文的**下层配套**)
- `2026-06-21-linux-root-usability-survey.md`(用户身份/权限边界)
- `2026-06-26-multiarch-package-description-design.md`(payload 的多架构表达,本文所有新包都要沿用)
- mcpp-index `pkgs/c/compat.glx-runtime.lua`(现存"半 hermetic"的典型样本)
- xim-pkgindex `pkgs/g/glibc.lua`(只有 2.39,`XLINGS_RES` payload)

---

## 0. TL;DR

**将 xlings 明确定位为"用户态 Linux 发行版",执行"能不依赖宿主就不依赖宿主"策略。**
物理上不可自带的资源(kernel syscall、wire protocol、GPU vendor 私有栈)显式列入 `capabilities_host` 白名单,除此之外一律走 xlings 内自建 payload。glibc 版本冲突是"任何宿主 `.so` 被拽进 xlings loader 进程"的**症状**,而非独立问题;策略解决"依赖宿主的范围",症状自动消失。

**近期最优选择(不锁死路径)**:
- Intel/AMD GPU 场景 → 走 hermetic-full(自带 Mesa/libdrm/GLVND)
- NVIDIA 场景 → 走版本化 payload(参考 Flatpak `org.freedesktop.Platform.GL.nvidia-XXX-YY`)
- Kernel module 编译**不接管**(留给宿主/DKMS),xlings 只负责 userspace 侧对齐

**保留的未来迁移路径**(不现在做,但**不封堵**):
- linker namespace 分层(参考 Steam pressure-vessel `libcapsule`)—— 长期解 NVIDIA 无版本 payload 兜底
- 接管 kernel module 编译(参考 NixOS `hardware.nvidia.package`)—— 若 xlings 自建 kernel/subos 深化时再评
- Release 套装模型(参考 NixOS channel / Flatpak runtime `//24.08`)—— 版本轴纪律

**明确不做**:
- 不做完整 Linux distro(不发行 init、systemd、包管理器替代)—— xlings 是**"用户态 app 侧的发行版"**,不是 rootfs 侧的发行版
- 不接管 kernel(不发行 kernel、不管 firmware 装载、不管 udev)

---

## 1. 深度分析:问题与现状

### 1.1 issue #352 的三层根因(具体表症)

| 层 | 归属 | 缺陷性质 | 修复形态 |
|---|---|---|---|
| L1 观测性静默 | `sudoevolve/EUI-NEO` `glfw_app_main.cpp` | 无 `glfwSetErrorCallback` + 各失败路径无 stderr | 上游 30 行 PR |
| L2 glx-runtime 布局假设错 | `mcpplibs/mcpp-index` `compat.glx-runtime.lua:63-127` | 候选目录顺序把 `/usr/lib` 放最后,`ln -sf` 覆盖 64-bit symlink 为 32-bit | 单 patch |
| L3 glibc 版本鸿沟 | `xim-pkgindex` `glibc.lua` + mcpp 引擎 | 沙盒 glibc 2.39 vs 宿主 Mesa 要求 GLIBC_2.43;`LD_LIBRARY_PATH=/usr/lib64` 变通亦不可用(ld.so 与 libc.so.6 版本必须成对) | **架构级** |

**L1、L2 是叶子,L3 是骨** —— 只补 L1/L2 会让用户从"静默 exit 255"变成"至少能看到 GLIBC_2.43 not found",但**不解决根本问题**。

### 1.2 L3 的真实性质:glibc 是症状,不是成因

- 崩溃的必要条件是:xlings loader(`xim-x-glibc/2.39/ld-linux-x86-64.so.2`)加载了**一份别人编译时锁死 glibc 版本的 `.so`**
- issue #352 的具体路径:用户 exe `dlopen("libGLX.so.0")` → symlink → 宿主 `libGLX_mesa.so.0` → 后者 `DT_NEEDED libc.so.6` 且 symver 表含 `GLIBC_2.43` → 我们的 2.39 libc 提不出该符号 → SIGABRT
- **如果 loader 全程只加载"xlings 自建、都链到 2.39"的 `.so`,`GLIBC_*` 这个词根本不会出现在任何 error message 里**

这个观察的**推论**:
- 问题不是"glibc 太老要追新",而是"何时应该跨越到宿主库"这个边界不清晰
- 追 glibc 版本是**追不完的**(发行版每半年一版,rawhide 永远领先),且**老 mcpp 二进制不受益**(PT_INTERP 编译时烙定)
- **正解是收敛"跨越边界"的地方**,不是提高沙盒 glibc 的版本

### 1.3 生态里已经在"实质做 hermetic"但没写成明文

现有历史决策(散落在若干 issue/PR)已经**在无意贯彻这个策略**,只是从未被明确提炼:

| 记录 | 决策 | 隐含语义 |
|---|---|---|
| xim-pkgindex termux musl DNS | musl-static `getaddrinfo` 读不到 `$PREFIX/etc/resolv.conf` → 手写 UDP DNS | 不依赖宿主 `/etc/resolv.conf` |
| mcpp e2e elfpatch 前缀替换 | 测试 elfpatch 曾"写穿 `ln -sf`"烙死 `mktemp -d` 路径 → 改为前缀替换 | payload 不能借宿主路径当锚 |
| mcpp macOS 静态 libc++ SIOF | 归档成员 `.init_array.<prio>` 排最后 → 静态 vs 动态是分界线 | 自带 C++ 运行时的 ABI 独立性 |
| xim-pkgindex llvm22 slim asset | slim 删了 `libatomic.so.1`、libc++ 硬依赖它、沙盒 glibc loader**不回退系统** → 崩 | loader 不回退系统 = 隐式 hermetic |
| mcpp Windows DLL deploy | PE 无 RPATH → 拷 DLL 到 exe 旁,`mcpp run` 会塞 PATH 掩盖漏洞 → 直接执行 `.exe` 才是发布态 | Windows 上早就是 hermetic |

**结论**:"用户态发行版 + hermetic 优先"不是新方向,是**给已经在做的事一个明确的名字和判据**,以后新决策不用重新推导。

---

## 2. 策略陈述:边界与优先级

### 2.1 一句话原则

> **只有 kernel syscall 和显式声明的 wire protocol 允许穿越到宿主;所有 `.so`(除 GPU vendor 私有栈)必须来自 xlings 自建 payload。**

### 2.2 允许穿越的边界(**枚举而非例外**)

| 边界类型 | 具体 | 为什么无法自带 |
|---|---|---|
| kernel syscall | 全部 syscall,含 `/dev/dri/*` ioctl、`/dev/snd/*`、futex、io_uring、`/dev/nvidia*` ioctl | 内核 ABI,唯一入口 |
| wire protocol(socket 字节流)| X11(`/tmp/.X11-unix/`)、Wayland(`$XDG_RUNTIME_DIR/wayland-*`)、D-Bus(`/run/user/*/bus`)、PulseAudio/PipeWire(socket)、CUPS | 协议稳定,进程外通信,**不引入宿主 libc** |
| 硬件/固件 | `/lib/firmware`(kernel 自动加载) | 硬件绑定 |
| GPU vendor 强制耦合 | NVIDIA 专有栈(`libGLX_nvidia.so`、`libEGL_nvidia.so`、`libcuda.so` 等) | vendor 用私有 IOCTL 与 `nvidia.ko` 校验版本;`.so` 与内核模块必须严格对齐 |

### 2.3 不允许穿越的(即使"就一次也不行")

- 任何 `/usr/lib*` `/lib*` 下的 `.so`,含 `libc`、`libssl`、`libGL`(Mesa)、`libX11`、`libpulse`、`libdbus`、`libcups`、`libfontconfig`、`libfreetype`、`libgio` …
- `/usr/share/*` 下的**运行期数据**:zoneinfo、locale、CA bundle、fontconfig cache、图标主题
- xlings 有观点的 `/etc/*` 项:`/etc/ssl/certs`(信任链应归 xlings 管)、`/etc/resolv.conf`(DNS 已在 musl 场景踩过)

### 2.4 每一条穿越必须**在包元数据里写理由**

新增字段 `capabilities_host`,每一项带 `reason`:

```lua
capabilities_host = {
    { "kernel.drm",    reason = "GPU ioctl,唯一入口" },
    { "wire.wayland",  reason = "compositor 是宿主实例" },
    { "hw.nvidia-gpu", reason = "vendor 私有 IOCTL 版本绑定" }, -- 仅在探到 nvidia.ko 时激活
}
```

引擎侧在 target 分析阶段:**任何 dlopen 目标不在包自身链接闭包 ∪ `capabilities_host` 隐含允许集**,报错拒绝。当前 `compat.glx-runtime.lua:76-80` 那种"直接 `ln -sf /usr/lib64/*`"就会在这一步被拦下。

---

## 3. 场景枚举(不只是 GPU)

hermetic 策略的判据要在多个场景上自洽,不是只解 GPU:

| 场景 | 现状 | hermetic 策略下的形态 |
|---|---|---|
| **GPU/GL** (issue #352) | `compat.glx-runtime` symlink 宿主 | Intel/AMD 自带 Mesa;NVIDIA 版本化 payload |
| **音频** (PulseAudio/PipeWire) | 未系统性处理;示例包偶发使用 | `compat.libpulse`(client-side)+ `capabilities_host = wire.pulse`(server 是宿主) |
| **字体/i18n** | 依赖宿主 `/usr/share/fonts`、`fontconfig` | `compat.fontconfig` + `xim-x-fonts-default` payload(挑一套 Noto/DejaVu) |
| **HTTPS/CA** | 静态 OpenSSL 编译时 OPENSSLDIR=/etc/ssl → Debian 挂 | `xim-x-ca-certificates` payload(Mozilla bundle),客户端库改从这里读 |
| **DNS** | musl-static 已改手写 UDP | 已 hermetic,补写策略文档确认此为**原型** |
| **DBus** | 应用 dlopen 宿主 libdbus | `compat.libdbus`(client)+ `wire.dbus` |
| **打印** (CUPS) | 未涉及 | 若涉及则 `compat.libcups` + `wire.cups` |
| **timezone/locale** | 用宿主 `/usr/share/zoneinfo` `/usr/share/locale` | `xim-x-tzdata` + `xim-x-locale` 小 payload |
| **CLI 工具运行** | 已 hermetic(自带 glibc/gcc/llvm) | 现状即目标态,无变化 |

**重要**:并非所有场景要一次性做完。策略给的是**判据**;每次新包 review 时按判据决定 —— 这样以后不会有人再走"这次借一下 host"的短路。

---

## 4. 方案对比:GPU 边界四条路

聚焦最难的 GPU 场景,横向对比业界四种解法:

### 4.1 Flatpak — 版本化 runtime,一驱动一份

- **机制**:extension `org.freedesktop.Platform.GL.nvidia-575-42-01` 等,一版一份;首次运行探 `/proc/driver/nvidia/version` 拉对应
- **关键细节**:extension 里的 `.so` **重新链接到 Flatpak runtime 的 glibc**,不是 NVIDIA `.run` 原样;所以沙盒里 loader 加载它 glibc 天然对齐
- **代价**:CDN 存几百份(每份 ~200MB)、老驱动 extension 永远不能删
- **优点**:工程简单、5+ 年验证、有现成 CI 模板可参考

### 4.2 NixOS — 驱动是 nix derivation

- **机制**:`hardware.nvidia.package` 从 NVIDIA `.run` 抽源码,**用 nixpkgs 的 gcc + kernel headers 一起 build**,产出 `nvidia.ko` + 所有 `.so`,挂 `/run/opengl-driver/lib/`
- **glibc 对齐方式**:整个 NixOS 是**同一版 nixpkgs channel**,glibc 全局一致
- **代价**:xlings 必须接管 kernel module 编译(每 kernel 版本 × 每 driver 版本的构建矩阵)+ nixpkgs 级别的社区规模
- **不适合近期**:接管 kernel module 是 xlings 团队规模两个数量级之外的承诺

### 4.3 传统发行版(Debian/Fedora)— 没沙盒,自然没冲突

- **机制**:`nvidia-driver-575` / `akmod-nvidia`,DKMS 在每次 kernel 升级时重 build,userspace `.so` 按发行版 glibc 编,落 `/usr/lib*`
- **glibc 对齐方式**:整个系统只有一个 glibc,不存在两份
- **代价**:xlings 需要成为**真正的发行版**(rootfs 级、init 级、包管理器级),超出定位

### 4.4 Steam pressure-vessel + libcapsule — linker namespace 分层

- **机制**:利用 glibc `dlmopen` + `LM_ID_NEWLM`,让**同一进程内同时容纳两份 glibc** —— 沙盒 glibc 归应用,宿主 glibc 归 GL 驱动,由 `libcapsule` proxy 做跨 namespace marshaling
- **glibc 对齐方式**:根本不需要对齐,通过 linker namespace 隔离
- **代价**:GL 近千函数需生成 proxy stub(可自动化,但每次 GL 版本升要跟);目前只 Valve/Collabora 在维护
- **技术上最优雅**,但**工程量对小团队不现实** —— 作为未来兜底方案保留

### 4.5 决策矩阵

| 方案 | 存储代价 | 首次运行延迟 | 工程复杂度 | Kernel 侧责任 | xlings 定位契合度 |
|---|---|---|---|---|---|
| Flatpak | 高(几百份) | 高(下 ~200MB) | **低** | 无 | ★★★★ |
| NixOS | 中 | 无 | 中 | **接管 `.ko` 编译** | ★★(超定位) |
| 传统发行版 | 低 | 无 | 低(政策成本高) | 全接管 | ★(超定位) |
| pressure-vessel | 低(宿主单份) | 无 | **极高** | 无 | ★★★★★(但远期) |

**近期最优 = Flatpak-style,兼顾工程可行与定位契合**。

**长期不封堵 pressure-vessel-style**,一旦 GLVND 抽象已就位、包元数据已齐备,`libcapsule` 只是"实现细节替换",不需要改架构。这就是"最优选择 + 未来可迁移"的具体含义。

---

## 5. 推荐近期方案(P0 → P1)

### 5.1 P0 — 三个最小落地(2-4 周)

1. **明文写策略**(本文即是初稿),在 xlings 主 repo `docs/design/` 下发一份 policy doc
2. **补 L1/L2 止血**:
   - L1:向 sudoevolve/EUI-NEO 上游提 PR,`glfwSetErrorCallback` + 各失败点 stderr(**约 30 行**)
   - L2:改 `compat.glx-runtime.lua` 候选目录顺序 + ELF class 探测(约 10 行)—— **不解决 L3,但让用户至少能看到真实错误**
3. **发 `compat.mesa` payload**(Intel/AMD 路径的第一个 non-trivial 落地):
   - 内容:libgallium + libGLX_mesa + libEGL_mesa + libdrm(必要项)+ shader compiler 依赖的 llvm-runtime
   - 大小预算:< 500MB 压缩后
   - 依赖闭包必须审:任何 `.so` `DT_NEEDED` 指向宿主 = 缺包
   - **capabilities**:`opengl.glvnd`、`opengl.mesa.driver`
   - **capabilities_host**:`kernel.drm`(附 reason)

### 5.2 P1 — 4-6 周

4. **加 `capabilities_host` 元数据字段** + 索引侧校验:
   - `xim-pkgindex` 与 `mcpp-index` 的 lua 包描述加字段
   - 校验脚本(参考 `check_version_pins.sh`、`check_cross_package_refs.lua` 的做法):静态扫描每个包的 dlopen list,与 `capabilities_host` 允许集比对,不匹配报错
5. **NVIDIA 分支**(Flatpak-style):
   - `xim-x-nvidia-driver-575` `xim-x-nvidia-driver-570` 等一组 payload
   - xlings-res CI 自动从 NVIDIA `.run` 抽包 → 重链到 xim-x-glibc → 发 release
   - 首装时探测:读 `/proc/driver/nvidia/version` → 拉对应
   - **kernel module 不接管**:如果用户 kernel 上没有 `nvidia.ko`,报明确错误("请通过发行版安装 nvidia driver kernel module"),不试图代劳
6. **CI 空-host 校验**:
   - bwrap 构一个 host 只有 `/dev /proc /sys /tmp` 的 rootfs
   - 在其中跑 `mcpp build && mcpp test && mcpp run`(选一组 examples)
   - 任何 `LD_DEBUG=libs` 显示的 search path 命中 `/usr/lib*` 都失败
   - 参考 mcpp `link-argv-max-arg-strlen` 教训:**没有真实覆盖 = 没有验证**

### 5.3 P2 — 3-6 个月

7. **其他 hermetic 缺口**(章节 3 的表)按优先级补:CA bundle、fontconfig、pulseaudio client → dbus → cups
8. **Release 套装模型**(参考 Flatpak runtime `//24.08`):
   - 引入 `xlings-platform-2026.08` 这样的**同步版本集合**
   - Mesa + libdrm + libX11 + xim-x-glibc + libc++ 等作为**一组**发布,内部互测通过
   - `.xlings.json` 可 pin 平台版本而非每个包单独 pin
9. **考虑 linker namespace 兜底**(P3,不承诺):
   - 如果 NVIDIA payload 矩阵开始成本失控(比如 Chinese fork 分支太多),或用户"就是插了没见过的 GPU"的场景增多,启动 `libcapsule` 抽象研究

---

## 6. 未来可迁移路径(**不封堵、不预实现**)

以下都是**在近期方案架构上可增量演化**的迁移方向,现在**不做**但**不封堵**:

### 6.1 → NixOS-style 接管 kernel module

- **触发条件**:xlings subos 深化到自建 kernel(现在只是 sandbox 层),或**社区规模足以维护 driver × kernel 编译矩阵**
- **迁移复杂度**:`xim-x-nvidia-driver-XXX` payload 现有元数据结构可原地扩展,增加 `kmod` 段,由 xlings 侧编译
- **保留的选择**:即使做了,也可以保留 Flatpak-style 作为 fallback(用户没有 build toolchain 时)

### 6.2 → pressure-vessel-style linker namespace

- **触发条件**:NVIDIA payload 矩阵爆炸,或 pre-built 二进制场景增多
- **迁移复杂度**:GLVND 层已经就位(方案 4.1 已在做 GLVND 分发),`libcapsule` 只替换分发层实现
- **保留的选择**:可以只对 GL 一族做 capsule 化,其他包(音频、DBus)仍用普通 hermetic

### 6.3 → Release 套装 → Rolling / LTS 双通道

- **触发条件**:企业/教育场景要求"两年不动"
- **迁移复杂度**:`xim-platform-YYYY.MM` 的语义已经支持"套装",加个 `.lts` 标签即可
- **保留的选择**:rolling 用户零感知

### 6.4 → 完整 rootfs distro

- **触发条件**:xlings 定位主动扩大(未来讨论,不现在承诺)
- **迁移复杂度**:显著,涉及 init/包管理器/kernel
- **本文明确**:P0-P2 不做这一步

---

## 7. 明确不做(non-goals)

以下事项即使技术可行也**不做**,避免任务蔓延:

- **不发行 kernel**:kernel 由宿主提供,`capabilities_host = kernel.*` 是永久边界
- **不接管 NVIDIA kernel module 编译**(近期):DKMS 交给宿主发行版
- **不发行 systemd/init**:xlings 是用户态,不管进程 1
- **不做 Flatpak-style 应用沙盒**:xlings 是**开发工具生态**,不是应用分发;subos 有 sandbox 但那是隔离,不是应用运行时
- **不追 glibc 版本**:2.39 就是 2.39,不发 2.40/2.41/...(除非有独立强 need);策略从"追新"转向"收敛边界"

---

## 8. 开放问题(等实施前决策)

1. **NVIDIA payload 命名 schema**:
   - Flatpak: `nvidia-575-42-01`(3 段)
   - 是否需要区分 open-kernel-module vs proprietary?(NVIDIA 从 R515 起有 open kernel module,但 userspace 仍闭源)
   - 建议参考 Flatpak 命名,后续按需扩展
2. **capabilities_host 声明的 review 边界**:
   - 谁批准新增一条?
   - 是否需要每季度 audit 一次 `capabilities_host` 集合避免膨胀?
3. **兼容"混合环境"用户**:
   - 一台机器同时有 Intel iGPU 和 NVIDIA dGPU(Optimus)
   - GLVND 分发能处理(靠 `__NV_PRIME_RENDER_OFFLOAD` 或 `DRI_PRIME`),但需要 payload 都装齐
   - 探测策略:探到 `/proc/driver/nvidia/version` 就装 NVIDIA payload,同时也装 Mesa payload
4. **payload 首次下载体验**:
   - Mesa payload ~500MB、NVIDIA payload ~200MB,首次运行 GUI 项目会明显停顿
   - 是否 seed 到 xlings quick_install?或让用户显式 `xlings install compat.mesa`?
   - 建议参考 Flatpak:第一次 `mcpp run` 触发下载,显示进度条,明确告知
5. **`compat.mesa` 与 `xim-x-mesa` 的归属**:
   - mcpp-index 归属 → 用户项目侧
   - xim-pkgindex 归属 → 工具链侧
   - GL 库属于 runtime,理论上两侧都可以;建议放 **xim-pkgindex**(更接近"用户态发行版底座")
6. **老 mcpp 二进制的兼容**:
   - 已发布 mcpp 二进制 PT_INTERP 是烙定的
   - 新策略下产出的二进制才 hermetic
   - 需要**发版说明明确**:某个 mcpp 版本起 GUI 项目走新路径

---

## 9. 与已有 xlings 文档的关系

- **不冲突**:本文是**上位策略**,不推翻任何已有实施设计
- **补齐**:
  - `2026-05-22-subos-sandbox-gpu-passthrough.md` 解决"sandbox 里能看到 GPU 设备节点",本文解决"看到之后跑什么 GL 库"—— 一上一下,配套
  - `2026-06-26-multiarch-package-description-design.md` 提供 payload 的多架构表达,本文新增的所有 payload 沿用
  - `2026-06-21-linux-root-usability-survey.md` 讨论权限边界,本文的 `capabilities_host` 是"资源边界"版本,理念一致
- **更新**:本文落地后,`compat.glx-runtime.lua` 那种"直接 symlink 宿主"的模式应逐步淘汰,mcpp-index 的 `docs/CONTRIBUTING` 应加"新包不得直接 symlink 宿主 `.so`"约束

---

## 10. 一句话总括

> **xlings 是"用户态 Linux 发行版",执行"能不依赖宿主就不依赖宿主"策略。glibc 冲突是策略未明确时的症状;把边界写清楚,症状自然消失。近期抄 Flatpak-style 的做法把 GPU 边界收敛;长期不封堵 NixOS-style / pressure-vessel-style / rolling+LTS 等演化路径。**
