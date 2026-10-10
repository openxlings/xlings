# Luban OS 设计 Part 2：包设计 v2、环境升级、启动层、宿主矩阵与交付（2026-10-10）

- 前文：
  - Part 1：`2026-10-09-luban-os-and-agent-private-design.md`，含 §F 实施记录；
  - 总图：`2026-10-10-luban-ecosystem-overview.md`；
  - SubOS 架构 part 1–3；
  - PR #650 / #651 的自我 review。
- 基线：
  - xlings 2026.10.10.2 已发布；
  - #653（bwrap 探测）未合入，内容并入本轮；
  - xim-pkgindex #945 未合入，本轮按本文重构。
- 本文覆盖：xim-pkgindex 上的 Luban 包怎么设计（不改 xpkg 规范）、xlings 的配套、CI 与资源等基础设施、单 PR 的检查点与依赖。

---

## 0. 本轮已确认的决策

| # | 决策 |
|---|---|
| D1 | **xpkg 规范不改。** Luban 的包全部用现有的 `package` 表、`xpm`、hook、`type`、索引 `libs/` 表达；新能力放在 edition manifest（`.xlings.json`）的字段里，由客户端读取 |
| D2 | **edition 写成数据。** 写盘代码只在 `libs/luban.lua` 里一份 |
| D3 | **环境升级**（`luban upgrade`）进入本轮 |
| D4 | **agent 工具在 edition 里不带版本**，创建时解析并锁定到实例；换 agent 不加新参数 |
| D5 | **启动层成包**（generic / virt），用 `luban export … --boot <profile>` 选择；`luban try` 默认 virt |
| D6 | **limine 预编译**，发布到 xlings-res（GitHub），本地 gtc 补 GitCode |
| D7 | **新增 luban-tiny-musl**；aarch64 做到 tiny；luban-desktop 改为 preview |
| D8 | **edition、策略、启动层用自己的发布日期作版本**，与 xlings 版本独立；上游软件保持上游版本 |
| D9 | **宿主矩阵 CI**：真实用户的宿主（AppArmor 限制、SELinux、没有免密 sudo、setuid 与否）成为测试维度，参考 mcpp 的 `ci-fresh-install` |
| D10 | 每个仓库一个 PR，commit 作为检查点；cmdline 库不动（luban 的命令只加在它自己的命令树里） |
| D11 | **宿主矩阵的 VM 腿是测试基础设施**，放在 xlings 的 `tests/host-matrix/` 和可复用的 `xlings-ci-host-matrix.yml`（索引调用同一套），不是包，也不进 `modules/`、`luban/` |
| D12 | **upgrade 从不放宽隔离**：新策略有任何一项放宽，就保留旧策略并给出 `subos config <n> --sandbox <ref>`；不加 `--accept-policy` |
| D13 | `linux-kernel-virt` 用 6.12 LTS（6.12.112） |
| D14 | 版本日期按实际合入当天，xlings 与索引的 edition 可以不同日 |

---

## 1. 现状与本轮要解决的问题

| 问题 | 证据 | 本轮的解决 |
|---|---|---|
| P1 宿主差异：三次修复都在发布后才发现 | 2026.10.10.1（AppArmor 下 netns 没有 caps）、.2（net=proxy 改由 bwrap 建 netns）、#653（setuid bwrap 不支持 `--disable-userns`） | §5 宿主矩阵；探测测策略用到的能力（#653 已做） |
| P2 验收滞后于发布 | #945 的验收只能测已发布的 xlings | §5.4 索引验收接受 xlings 的 PR 构建 |
| P3 环境停在快照上 | edition 精确锁定，没有升级路径 | §3 `luban upgrade` |
| P4 recipe 重复、混着代码 | 4 份 `write()` / os-release；tiny 用 Lua 字符串写 rcS | §2.1 `libs/luban.lua` |
| P5 limine 在用户机器上编译 | 没有 cc 时只警告，BIOS 启动静默缺失 | §2.5 预编译资源 |
| P6 内核只是 edition 里的提示 | 真机和 VM 用同一个 generic 内核 | §2.4 启动层 |
| P7 一致性 | tiny 不是 from nano；新版本还写 xlings-init；desktop 停在 0.1.0；nano 声明 aarch64 而其余不支持 | §2.6 |
| P8 间歇失败 | stage-0 200 ms 预算：1.6 s / 2.2 s | §5.6 |

---

## 2. xim-pkgindex 包设计 v2（在现有 xpkg 规范内）

### 2.1 共享库 `libs/luban.lua`（`import("xim.pkgindex.luban")`）

索引已有的机制（sysroot、selfcontain 就是这样）。导出三个函数：

```lua
-- 写一个 edition：.xlings.json、factory /etc、os-release（由 variant 与版本生成）。
-- versions[v] = { manifest = <JSON 字符串>, files = { ["etc/<rel>"] = { content, mode } } }
luban.edition(variant, versions)        -- 返回 true；版本缺失时 error
-- 写一个策略：policy.json
luban.policy(json)
-- 写一个启动层：share/luban/boot.json
luban.boot(json)
```

规则：
- **manifest 保持为 JSON 字符串**，不在 Lua 里拼 JSON。这样静态测试（python）可以逐字取出，写出的字节也可以预测。
- os-release 的格式固定为：`NAME="Luban"`、`ID=luban`、`VARIANT`、`VARIANT_ID`、`VERSION_ID`、`PRETTY_NAME`、`HOME_URL`。
- 文件模式只允许 `0644` 和 `0755`。写入用 `os` 的接口；只有设置模式时才调用 `chmod`，而且路径要加引号。

### 2.2 recipe 的形状（以 core 为例）

```lua
-- Luban Core: daily command-line development (luban-tiny + GNU tools, toolchain, mcpp, git, editors, shells).
package = {
    spec = "2", name = "luban-core", namespace = "subos", type = "subos",
    description = "...", archs = {"x86_64"}, status = "stable", licenses = {"Apache-2.0"},
    categories = {"subos", "distribution"},
    xpm = { linux = {
        ["latest"] = { ref = "2026.10.12.1" },
        ["0.1.0"] = {}, ["2026.10.10.1"] = {}, ["2026.10.12.1"] = {},
    } },
}

import("xim.pkgindex.luban")

local versions = {
    ["0.1.0"]        = { manifest = [[...原样...]], files = { ["etc/shells"] = { "/bin/sh\n/bin/bash\n" } } },
    ["2026.10.10.1"] = { manifest = [[...原样...]], files = { ... } },
    ["2026.10.12.1"] = { manifest = [[{ "subos_kind": "rootfs", "min_client": "2026.10.12.1",
                                        "from": "subos:luban-tiny@2026.10.12.1", ... }]], files = { ... } },
}

function install() return luban.edition("Core", versions) end
```

- **已发布版本的 manifest 原样搬进 `versions`。** 迁移 commit 前后，每个已发布版本写出的文件字节相同，由 §5.4 的 golden 证明。
- 旧客户端也能用：`libs/` 的 import 从 2026.10.8.1 起就支持，与 `index-compat.json` 的下限一致。

### 2.3 包清单

| 包 | type | 版本 | 本轮内容 |
|---|---|---|---|
| **luban-nano** | subos | 2026.10.10.1 → **2026.10.12.1** | 迁移到库；`abi: {kernel: linux, libc: none}`；`boot.profile` 指向 generic；archs x86_64 + aarch64 |
| **luban-tiny** | subos | 新增 **2026.10.12.1** | **from nano**；glibc + busybox + CA + patchelf；busybox init；inittab 用 `luban-init`；archs x86_64 + **aarch64** |
| **luban-tiny-musl**（新） | subos | 2026.10.12.1 | from nano；`abi: x86_64-linux-musl`；busybox（上游就是静态 musl 构建）+ `xim:musl` + CA；证明 libc 是可选的 |
| **luban-core** | subos | 新增 2026.10.12.1 | from tiny@2026.10.12.1；内容同 2026.10.10.1 |
| **luban-agent-workspace** | subos | 新增 2026.10.12.1 | from core；`"xim:claude"` **不带版本**；`policy: xim:agent-private@2026.10.12.1`；motd 写明三档 |
| **luban-desktop** | subos | 0.1.0 保持不变 | `status = "preview"`；luban 的短名表去掉 desktop；迁移留到以后 |
| **agent-private** | subos-policy | 新增 2026.10.12.1 | 内容同 2026.10.10.1；`min_client` 改为本轮的客户端（因为要求 #653 的探测）；注释写明 macOS/Windows 在 wsl2/vz 承载里 |
| **agent-confined** | subos-policy | 2026.10.10.1 | 迁移到库，内容不变 |
| **luban-boot-generic**（新） | package | 2026.10.12.1 | deps：`linux-kernel@6.8.0-71`、`limine@12.9.3-r1`；boot.json：`console=tty0 console=ttyS0`，`initramfs` / `disk` 两种根 |
| **luban-boot-virt**（新） | package | 2026.10.12.1 | deps：`linux-kernel-virt`、limine；boot.json：`console=ttyS0`，virtio，没有图形 |
| **linux-kernel-virt**（新） | package | 上游版本 | virt 内核（x86_64、aarch64），来自 xlings-res 构建（§4.2） |
| **limine** | package | **12.9.3-r1** | 预编译 tarball（启动文件 + 静态的 `limine` 工具）；x86_64 / aarch64 的工具二进制；recipe 不再调用 cc |
| busybox、linux-kernel | package | 补 aarch64 | per-arch URL；busybox 上游没有 aarch64 静态构建时，由 xlings-res 构建 |

> 版本日期 `2026.10.12.1` 是占位，实际用发布当天的日期。

### 2.4 启动层的数据（`share/luban/boot.json`）

```json
{ "profile": "virt", "kernel": "xim:linux-kernel-virt@6.12.12",
  "cmdline": ["console=ttyS0", "panic=-1"],
  "roots": { "initramfs": {}, "disk": { "root": "PARTUUID" } },
  "loader": "xim:limine@12.9.3-r1" }
```

- edition 的 manifest 写 `"boot": { "profile": "xim:luban-boot-generic@2026.10.12.1", "init": "/sbin/init" }`。
- 旧字段 `boot.kernel` / `kernel_min` 继续读：没有 profile 时，按旧方式处理。
- 导出时把 profile 装进根（与现在装内核的方式相同）。
  - `--boot virt` 是 `luban-boot-virt@latest` 的短名；也可以写完整引用。
  - 第三方可以写自己的 profile。

### 2.5 limine 资源

- 构建由 xim-pkgindex 的 workflow 完成（`res-limine.yml`，手动触发）：
  1. 下载上游 `limine-binary.tar.gz`，校验 sha256；
  2. 在 musl 静态工具链里编译 `limine.c`（x86_64 和 aarch64）；
  3. 打包成 `limine-12.9.3-r1-linux-<arch>.tar.gz`：`share/limine/*` 加 `bin/limine`。
- 上传到 `xlings-res/limine` 的 GitHub release。GitCode 由本地 `gtc` 补，再用 **GET** 校验 sha256（HEAD 会得到 401）。
- recipe 用 `GLOBAL` / `CN` 两个 URL，与 glibc 的写法相同。

### 2.6 一致性规则（写进索引的 `docs/`）

1. Luban 自己的包（edition、策略、启动层）用发布日期；上游软件用上游版本，可加 `-rN`。
2. **已发布版本的 manifest 永不修改**（golden 强制）。
3. **edition 用到新客户端字段时，写 `min_client`。** 新字段一律设计成"旧客户端忽略后仍然安全"：忽略 `boot.profile` 时退回 `boot.kernel`；把不带版本的包当作"最新"处理。
4. 新 edition 只引用 `luban-init`；xlings-init 只为已发布版本保留。
5. 每个 edition 都 `from` 一个官方层（nano 除外），`abi` 必须写明。

---

## 3. xlings 的配套（客户端）

### 3.1 manifest 字段（只追加）

| 字段 | 含义 | 旧客户端 |
|---|---|---|
| `min_client` | 低于这个版本的客户端拒绝使用，提示 `xlings self update` | 忽略，按 §2.6-3 安全退化 |
| `boot.profile` | 启动层引用 | 忽略，用 `boot.kernel` |
| 不带版本的 `packages` 项 | 创建时解析为最新版，并锁定到实例 | 已经按最新版处理（需要验证） |

### 3.2 `instance.json.edition`（只追加）

```json
"edition": { "ref": "subos:luban-core@2026.10.12.1",
             "chain": ["subos:luban-tiny@2026.10.12.1", "subos:luban-nano@2026.10.12.1"],
             "packages": { "xim:gcc": "16.1.0", "xim:claude": "2.1.300", ... },
             "policy": "xim:agent-private@2026.10.12.1" }
```

- 这里记录的是 **edition 层**：模板在创建时带来的包。之后用户装的包是 **用户层**：workspace 里有、而 edition 层里没有的包。
- 没有这个字段的旧实例（2026.10.10.3 之前创建，或不是从 edition 创建）：`upgrade` 如实拒绝并说明原因，不推导——当时用的是哪个顶层模板没有记录，推导出来的"edition 层"可能把用户的包当成 edition 的。

### 3.3 `luban upgrade <环境>`（xlings：`subos upgrade`）

```
$ luban upgrade dev
dev: Luban Core 2026.10.10.1 → 2026.10.12.1
  upgrade  openssl 3.1.5 → 3.5.1
  add      ninja 1.12.1
  keep     your packages: cmake 3.31, node 22   (yours, not the edition's)
  policy   unchanged
proceed? [Y/n]
```

**规则**
- 一次升级 = 一个新的代。`luban rollback dev` 回到上一代。
- **用户层优先：** 用户自己装的某个包，如果和 edition 层是同名的另一个版本，保留用户的版本，并报告。
- **策略方向（D12）：**
  - 新策略不放宽任何一项（`policy::loosened` 为空）时随升级生效；
  - 有任何放宽（包括收紧与放宽混合）时保留旧策略，列出放宽的项，并给出 `xlings subos config <n> --sandbox <新策略>`——放宽只能由所有者显式切换。
- **不带版本的包**（agent）重新解析为最新版。
- **非 rootfs 的 SubOS**（没有 edition）：提示没有可升级的 edition。

**参数**（luban 的命令树里加 `upgrade`；xlings 侧是 `subos upgrade`）
- `--to <版本>`
- `--dry-run`
- `-y`（全局选项）

### 3.4 其他

- `luban export … --boot <profile>`；`luban try` 默认 virt profile，找不到时退回 generic。
- **#653 的内容**：bwrap 探测测 `--disable-userns`；选择器优先能做到的那个 bwrap；需要它的策略在进入前拒绝，并给出修复命令；doctor 在受限宿主上不满足时判为不可用。
- **stage-0 预算**（§5.6）。
- luban 的短名表去掉 desktop（ns:name 的完整写法照常可用）。

---

## 4. 资源与发布基础设施

### 4.1 发布流程（xlings，保持不变并写清楚）

1. squash 合入（`--admin`）。
2. 取消 main 上的 CI（gitee-sync 除外）。
3. `gh workflow run release.yml`。
4. `tools/mirror-latest.sh xlings`（gtc），再用 GET 校验 sha256。
5. 合入 bump PR：4 个平台的 sha256 加上 `latest`。
6. **等待已发布的索引产物**（`xlings-res/xim-index` 的 latest 指针）跟上，见 §5.3。
7. 重跑 main CI。
8. 定时或手动跑宿主矩阵和索引验收。

### 4.2 xlings-res 构建流水线（新资源）

| 资源 | 构建位置 | 产物 |
|---|---|---|
| limine 12.9.3-r1 | xim-pkgindex `res-limine.yml` | x86_64、aarch64 |
| busybox aarch64（上游没有静态 musl 构建时） | `res-busybox.yml`（musl 交叉工具链，配置固定） | 静态 `busybox` |
| linux-kernel aarch64 / linux-kernel-virt | `res-kernel.yml`（固定 config，`make -j`，记录 config 的哈希） | bzImage / Image 加 modules（virt 不需要 modules） |

- 所有流水线都 **只上传到 GitHub**。GitCode 一律由本地 gtc 补，再用 GET 校验。
- 构建脚本和 config 放在索引的 `tools/res/` 下，保证可复现、可审查。

---

## 5. CI 架构：宿主矩阵与覆盖

### 5.1 原则

- **测试宿主要像用户的机器。** 至今三次事故都来自 CI 宿主太"干净"：
  - 关掉了 AppArmor 限制；
  - sudo 免密（recipe 因此把 bwrap 装成 setuid）；
  - 前台终端。
- **矩阵的维度是"宿主的事实"，不是发行版的名字**：LSM（AppArmor 限制开/关、SELinux）、bwrap 来源（payload / 系统包 / root 所有 + profile）、setuid 与否、sudo（免密 / 需要密码 / 没有）、KVM 有无、glibc 版本。
- **同一套场景脚本在每个宿主上跑**，输出机器可读的证据，汇总成一张"宿主 × 场景"表（写进 job summary 和 xdev 报告）。
- **按触发分层：**
  - PR 只跑最小的高风险子集；
  - 每天定时跑全量；
  - 发布后在等到索引产物之后跑全量。
  
  沿用 mcpp `ci-fresh-install` 的做法：`fail-fast: false`；被测版本只推导一次，所有 job 用同一个；发布后先等已发布的索引产物（而不是 git）跟上。

### 5.2 两层宿主

**K 层：内核与 LSM 的真实宿主。** 用 VM 或 runner 自己。容器共享 runner 的内核和 LSM，测不到这一层。

| 腿 | 怎么得到 | 关键事实 |
|---|---|---|
| k-ubuntu-2404-restricted | runner `ubuntu-24.04` 原样 | AppArmor 限制开 |
| k-ubuntu-2404-open | 同上，`sysctl apparmor_restrict_unprivileged_userns=0` | 对照组 |
| k-ubuntu-2204 | runner `ubuntu-22.04` | 没有限制，较旧的内核和 bwrap |
| k-ubuntu-2404-arm | runner `ubuntu-24.04-arm` | aarch64 原生 |
| k-fedora | runner 上用 KVM 启动 Fedora Cloud 镜像（cloud-init） | SELinux enforcing，较新内核 |
| k-debian-12 | KVM 启动 Debian 12 genericcloud | 默认允许 userns，没有 AppArmor 限制 |
| k-arch | KVM 启动 Arch cloud 镜像 | 最新内核 |

- VM 腿共用一个脚本 `tests/host-matrix/vm.sh`：
  1. 下载镜像（按镜像 URL 和 sha256 缓存）；
  2. cloud-init 建一个没有免密 sudo 的普通用户；
  3. 9p 或 virtiofs 共享 xlings 产物；
  4. ssh 进去以那个用户运行场景。
- 每条 VM 腿的时长控制在 15–25 分钟。

**U 层：用户态差异。** 用容器，便宜，覆盖面广。

| 腿 | 镜像 | 关键事实 |
|---|---|---|
| u-fedora / u-arch / u-tumbleweed / u-debian-testing / u-debian-12 / u-ubuntu-2004 | 同 mcpp 的矩阵 | 发行版自己的 bwrap、pasta 包，glibc 范围 |
| u-centos7（已有） | centos:7 | 很旧的 glibc；只测包管理部分 |

- 容器以 `--security-opt seccomp=unconfined --cap-add SYS_ADMIN` 运行，否则 Docker 默认的 seccomp 会挡住 userns，测到的就是 Docker 而不是发行版。
- 每条腿跑两种 bwrap：发行版包和 xim payload。setuid 与非 setuid 各测一次。

**真实用户的模拟**（K 层和 U 层都适用）
- 场景以一个 **没有免密 sudo** 的普通用户运行。
- 需要一次性设置的场景分两步：
  - 先以 agent 模式运行，期望退出码 2，并且错误里给出确切命令；
  - 再由"管理员"（root）执行那条命令，然后继续以普通用户运行。
- 这样既测到"不能提权时的体验"，也测到"设置之后的结果"，而不会因为 sudo 免密改变 recipe 的行为。

### 5.3 场景（`tests/host-matrix/scenarios/*.sh`，每个输出 `evidence.json`）

| 场景 | 内容 | covers |
|---|---|---|
| S-install | quick_install；`xlings --version`；`luban --version` 一致 | FRESH-* |
| S-doctor | `self doctor --isolation --json`：判断与实际进入的结果一致（判可用就能进；判不可用就给修复命令） | DOC-ISOLATION |
| S-presets | 每个预设（dev / private / locked）以及 agent-confined、agent-private：创建、进入、`id`、网络接口、`unshare -U` 被禁止（需要时） | ISO-*、PRIVATE-USERNS-RESTRICTED-HOST |
| S-proxy | net=proxy：只有 lo；经代理成功；代理挂掉时不回落 | PROXY-NET-RESTRICTED-HOST |
| S-nat | net=nat（有 pasta 时） | ISO-NET-NAT |
| S-edition | `luban new` nano / tiny / tiny-musl / core：先检查宿主后下载、一个计划、进入、`luban status` | LUBAN-* |
| S-agent | `luban new a agent-workspace --proxy`：persona 稳定，时区不来自宿主，在进入前就已经私有 | AGENT-* |
| S-upgrade | 在旧版本 edition 上创建，`luban upgrade`，保留用户层，回滚 | LUBAN-UPGRADE（新） |
| S-image | `export --iso` / `--drive --boot virt`，qemu（有 KVM 时）启动到 init | LUBAN-IMAGE |
| S-terminal | 伪终端下 `luban new` / `enter` 不卡住（SIGTTOU） | TERM-* |

### 5.4 各仓库的 CI 分层

**xlings**

| 触发 | 内容 |
|---|---|
| PR | 现有 lanes 加上 **host-matrix-smoke**：k-ubuntu-2404-restricted（普通用户，非免密）× S-doctor / S-presets / S-proxy / S-edition，再加 u-fedora × S-presets。被测的是本 PR 的构建 |
| PR（可选标签 `host-matrix`） | 全量 K 层和 U 层 × 全部场景 |
| 每天 06:30 UTC | 全量，被测的是已发布的 latest |
| 发布后（`workflow_run`） | 等索引产物跟上，然后跑全量 |

**xim-pkgindex**

| 触发 | 内容 |
|---|---|
| PR | 静态测试（由库的数据驱动）：`from` 链、ABI、包能否解析、archs 交集；**已发布版本不变的 golden**（`tests/fixtures/luban-published.json`：每个版本写出文件的 sha256）；min_client 不能低于用到的字段所要求的版本 |
| PR（改动 `pkgs/l/luban-*`、`pkgs/a/agent-*`、启动层、limine、`libs/luban.lua`） | 真实验收 = S-edition / S-agent / S-image，跑在 k-ubuntu-2404-restricted 上；workflow 接受输入 `xlings_artifact`（xlings PR 的 run id），默认用已发布的 latest |
| 每天定时 | 全部 edition × K 层（x86_64 和 arm） |

xlings 的 PR 改动了 SubOS 或 luban 时，可以手动触发索引验收并传入本 PR 的构建，这样在发布之前就能看到真实结果（解决 P2）。

### 5.5 证据与报告

- 每个场景写一个 `evidence.json`：宿主事实（LSM、sysctl、bwrap 来源和模式、sudo、KVM、内核、glibc）、步骤、退出码、关键输出。
- xdev 汇总：
  - 生成"宿主 × 场景"矩阵表（job summary）；
  - 和 `tests/requirements.toml` 的 covers 对接："required 的 requirement 必须在至少一个 K 层宿主上有通过证据"。
- 失败时保存 VM 串口日志和场景日志。

### 5.6 stage-0 预算（P8）

- stage-0 输出各阶段的耗时（`stage0_us` 拆成 mount、read boot.json、resolve、exec 几段）。
- 只在有 KVM 时强制 200 ms；没有 KVM 时只报告。
- 两次越界都发生在 distro 车道。查清是哪一段之后，再决定是修代码还是调整预算。

---

## 6. 交付：单 PR、检查点与依赖

### 6.1 xlings：一个 PR（版本 `2026.10.<d>.1`）

| # | commit（每个都能编译、CI 绿） | 依赖 |
|---|---|---|
| X1 | bwrap 探测测 `--disable-userns`（#653 的内容） | — |
| X2 | stage-0 分段计时；预算只在有 KVM 时强制 | — |
| X3 | manifest 的 `min_client`；不带版本的包在创建时锁定 | — |
| X4 | `instance.json.edition`（创建时写；旧实例推导） | X3 |
| X5 | `subos upgrade` 的核心放在 `modules/subos`（差异、用户层、新的一代、策略方向）；`luban upgrade` | X4 |
| X6 | `boot.profile`；`export --boot`；`try` 默认 virt | X3 |
| X7 | 短名表去掉 desktop；文档（quick-start/luban.md、AGENTS.md） | — |
| X8 | `tests/host-matrix/`：场景、vm.sh、容器腿；`xlings-ci-host-matrix.yml`（PR smoke、标签、定时、发布后） | X1–X6 |
| X9 | 自我 review 文档；requirements（LUBAN-UPGRADE、BOOT-PROFILE、HOST-MATRIX-*） | 全部 |

测试用 fixture recipe（新 edition、启动层、limine 资源的假 tarball），不依赖索引 PR，以避免两个 PR 循环等待。

### 6.2 xim-pkgindex：#945 重构为一个 PR

| # | commit | 依赖 |
|---|---|---|
| I1 | `libs/luban.lua`；现有 edition 和策略迁移过去（已发布版本字节不变）；golden | — |
| I2 | 资源流水线 `tools/res/` 和 `res-*.yml`；limine 12.9.3-r1、busybox aarch64、kernel aarch64、kernel-virt（产物上传，gtc 补 GitCode） | — |
| I3 | limine-r1、linux-kernel-virt、busybox/kernel 的 aarch64 URL | I2 |
| I4 | luban-boot-generic / luban-boot-virt | I3 |
| I5 | 新版本：nano / tiny（from nano、aarch64）/ tiny-musl / core / agent-workspace（不带版本的 claude）/ agent-private | I1、I4 |
| I6 | desktop 改为 preview；索引 `docs/` 的 Luban 包规则（§2.6） | I1 |
| I7 | 验收 workflow 改为场景化：接受 `xlings_artifact`；定时；arm 腿 | I5 |
| I8 | #945 原有的 gcc/binutils/openssl/xz build deps 改动保留为单独的 commit，并在 PR 描述里分区列出 | — |

### 6.3 顺序

```
I2（资源，可以最先开始，和 xlings 并行）
X1..X9 ──► xlings CI 全绿 ──► 自我 review ──► squash（--admin）──► 取消 main CI ──► release
      ──► gtc 补 GitCode（GET 校验）──► bump PR ──► 等索引产物 ──► 重跑 main CI
I1..I8 ──► 索引验收（用已发布的 xlings 和本 PR）全绿 ──► 合入 #945 ──► 等索引产物
      ──► xlings 宿主矩阵全量（定时 workflow 手动触发）──► 真实验证报告
```

---

## 7. 自我 review

| 角度 | 结论 | 保留的风险 / 应对 |
|---|---|---|
| 架构 | xpkg 规范不变；新能力是 manifest 字段（客户端的格式）和索引库（单一实现）；upgrade 的核心放进 `modules/subos`，不让 cmd.cpp 继续变大 | upgrade 的"用户层"依赖 `instance.json.edition`；旧实例要推导，推导的结果要说明是推导出来的 |
| 稳定性 | 宿主矩阵把"用户的机器"变成测试维度；探测测策略用到的能力；验收前移到 PR | VM 腿慢，而且镜像下载可能不稳定：按 URL 和 sha256 缓存；单腿失败不影响其他腿（fail-fast false）；PR 只跑 smoke |
| 优雅简洁 | 4 份写盘代码合成 1 份；用户看到的只多了 `upgrade` 和 `--boot` | 启动层对用户默认不可见 |
| 用户体验 | 升级前给出差异，用户的包优先，可以回滚；放宽隔离必须确认 | 第一次升级旧实例时的推导提示，措辞要清楚 |
| 兼容性 / 无感升级 | 只追加字段；已发布版本不变（golden）；旧客户端安全退化 | 2026.10.10.2 及更早的客户端不认识 `min_client`：新字段按"忽略后仍然安全"设计（§2.6-3），有测试守住 |
| 跨平台 | 策略包的跨平台语义写明是承载里的 Linux；镜像和写驱动器只在 Linux 上 | vz 助手没有发布之前，文档不承诺 macOS 可用 |
| 一致性 | 日期版本的规则只有一条；luban-init 统一；from 链统一从 nano 出发 | desktop 是唯一的例外（preview），写明 |
| 安全 / 隐私 | 策略放宽要确认；agent-private 的 min_client 要求新的探测；golden 防止已发布内容被悄悄修改 | A7（mountinfo 里的宿主路径）不在本轮，按总图排在 R6 |
| 范围 | 两个 PR 都偏大 | 用 commit 检查点推进；资源流水线（I2）最先开始；aarch64 的交叉构建如果卡住，I5 先交付 x86_64，aarch64 留到同一个 PR 的后续 commit |

---

## 8. 请 review 的点

1. **§5.2 K 层的 VM 腿**（Fedora / Debian / Arch cloud 镜像，跑在 runner 的 KVM 里）：放在 xlings 仓库（推荐），还是单独建一个 host-matrix 仓库？
2. **§5.4 PR 上的 smoke 子集**（k-ubuntu-2404-restricted 普通用户 + u-fedora）够不够？全量靠标签加定时。
3. **§3.3 放宽策略在 agent 模式下用 `--accept-policy` 确认**：可以吗？
4. **§2.3 linux-kernel-virt 的上游版本**：用 6.12 LTS（推荐），还是与 generic 保持 6.8？
5. **§6 版本日期**：按实际合入当天的日期，xlings 和索引的 edition 可以不同日。

---

## 9. 实施记录（2026-10-10，xlings PR #653 / xim-pkgindex PR #945）

### 9.1 已实现

| 项 | 内容 | 验证 |
|---|---|---|
| X1 | bwrap 探测测 `--disable-userns`；setuid 的 bwrap 不能禁止嵌套命名空间，需要它的策略在进入前拒绝并给出修复 | `test_confine`；`isolation_doctor_fix_test.sh`（setuid 与修复后） |
| X2 | stage-0 分段计时（`stage0_phases_us`）；200 ms 只在有 KVM 时强制 | `rootfs_boot_test.sh` |
| X3 | edition manifest 的 `min_client` 在下载前检查 | `test_subos_new_template` |
| X4 | `instance.json.edition`（ref、from 链、包的实际版本、策略） | 同上 |
| X5 | `subos upgrade` / `luban upgrade`；`xlings.subos.edition::plan`；`policy::loosened` | `test_subos_edition`，升级 e2e（1.0.0 → 2.0.0） |
| X6 | 启动层：`export --boot`，edition 的 `boot.profile`；`luban try` 默认 virt | `luban_image_test.sh`（fixture 启动层，qemu 启动） |
| X7 | desktop 改为 preview（无短名） | `test_luban_cli` |
| X8 | 宿主矩阵：`tests/host-matrix`（场景、run.sh、vm.sh），`xlings-ci-host-matrix.yml`；PR 跑 smoke | CI |
| I1 | `libs/luban.lua`；edition 与策略改成数据；已发布版本不变的 golden | `test_luban_published` |
| I2 | `tools/res/build.sh`、`res-build.yml`：limine（静态工具）、linux-kernel-virt 6.12.112、aarch64 busybox；经 `xpkg_ci.py mirror` 发布到 GitHub 与 GitCode，两边按内容校验 | res-build CI；本机安装 |
| I3–I5 | limine 改为 XLINGS_RES；linux-kernel-virt；busybox aarch64；luban-boot-generic / virt；tiny（from nano、luban-init、无架构的 ABI、aarch64、boot.profile）；tiny-musl；agent-workspace 的 claude 不带版本 | 静态测试；`test-luban-editions.sh`（ISO 经 virt 启动） |
| I6 | 索引 `docs/luban-packages.md` | — |
| I7 | 验收接受指定的 xlings 构建；editions 任务 | CI |

### 9.2 实施中发现并修复的缺陷

1. **不带版本的模板引用取的是"已安装的随便哪个"**（目录遍历的最后一项，而且没有排序）：`luban new box tiny` 装过一次旧模板后永远用旧的，`upgrade` 也永远看不到新版本。现在取索引的 `latest`，退回时按版本排序。
2. **`subos boot --now` 只认 inittab 里的 `xlings-init`**：新 edition 改用 `luban-init` 后会被无理由拒绝。两者都认。
3. **libc=musl 被一律拒绝**（"the index publishes gnu ones today"）：tiny-musl 发布后不再成立。musl 可用，索引没有载荷的 libc 仍拒绝并点名。
4. **`-y` 是全局选项**，在分发前被过滤；`subos upgrade` 改为接收记录下来的 `yesGiven`（第一版因此"给了 -y 仍询问"）。

### 9.3 与本文的差异

1. 新 edition 的版本仍是 `2026.10.10.1`：这些版本此前从未合入索引 main（未发布），所以可以修改；已发布的只有 tiny / core / desktop 的 `0.1.0`，golden 守住它们。
2. aarch64 做到 tiny 这一层（edition 与包）；aarch64 的镜像不在本轮：`luban try` 只跑 x86_64，驱动器的 ESP 只放 BOOTX64.EFI。`luban-boot-generic` 只有 x86_64（没有 aarch64 的 generic 内核），aarch64 用 `--boot virt`。
3. virt 启动层同时写 `console=ttyS0 console=ttyAMA0`：recipe 的 hook 里拿不到架构（`os.arch()` 在 hook 中未绑定），内核会跳过机器上没有的那个。
