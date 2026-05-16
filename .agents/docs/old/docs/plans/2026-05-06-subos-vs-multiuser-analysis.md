# subos 各档 vs Linux 多用户机制 —— 边际价值分析

> 把 Linux 多用户机制(`useradd` / `sudo -iu`)考虑进 subos 的设计版图,逐档质问"这一档相比开新用户多出来的价值是什么"。
>
> 与之配套的两份文档:
> - [`2026-05-06-subos-four-tier-comparison.md`](./2026-05-06-subos-four-tier-comparison.md) —— 四档逐维度对比
> - [`2026-05-06-subos-mode-implementation-details.md`](./2026-05-06-subos-mode-implementation-details.md) —— 各档实现细节

---

## 0. 为什么这个问题尖锐

Linux 自 1973 年起就是真多用户操作系统,每个 UID 天然有:
- 私有 `$HOME`(`/home/$USER` 物理上独立目录)
- 私有 dotfile / 缓存 / 历史(`.bashrc` / `.cache` / `.local` 都在自己 HOME 里)
- 进程隔离(无 root 不能见/杀别人进程)
- 文件权限隔离(其他用户的 700 权限文件读不到)
- 私有 ssh / gitconfig(在各自 HOME 里)
- shell 级 PATH 定制(`.bashrc` 里 export)

如果"多个上下文"等于"多个 UID",那 `useradd alice-rust && sudo -iu alice-rust` 已经覆盖一大半 subos 的卖点。**问题是:这套机制覆盖不到的恰恰是 xlings 的核心场景**,但具体哪些档被覆盖、哪些没,值得逐项过一遍。

---

## 1. Linux 多用户能做什么 / 不能做什么(事实清单)

### 1.1 多用户**天然能做**

| 能力 | 怎么做 | 备注 |
|---|---|---|
| 私有 `$HOME` | `useradd alice-class` 后 alice-class 的 `~` 就是 `/home/alice-class` | 物理独立目录,完全隔离 |
| 私有 dotfile (`.cache` / `.config` / `.local`) | 都在私有 HOME 内 | 无副作用 |
| 私有 ssh-key / gitconfig | 同上 | 切用户即切身份 |
| 私有 shell rc / PATH | 各 `.bashrc` / `.zshrc` 自己写 | |
| 进程互不可见 | UID 隔离 | 无 root 不能 ptrace 别人 |
| 私有 cron jobs / systemd-user services | 各自 `crontab -e` / `systemctl --user` | |
| 不同 default shell | `chsh -s /bin/zsh` 单用户生效 | |
| 软件装到 `~/local`、`~/.cargo`、`~/.npm-global` | 每个用户各自一份 | 业内主流 |

### 1.2 多用户**做不到**

| 做不到的事 | 为什么 |
|---|---|
| **同一用户、同一 shell 内切工具版本** | UID 不变就是同一身份,PATH 改了但全局工具版本还得手动管 |
| **同一用户内多套独立上下文**(alice 想要 work / class / research 互不干扰) | 一个 UID 只有一个 HOME |
| **跨用户共享部分数据 + 隔离另一部分** | 要么全共享要么全隔离,粒度只有"账户"这一层 |
| **/usr/local /etc /usr/lib 看到不同内容** | 这些是系统级,所有用户共享 |
| **替换 libc / glibc** | 系统库共享 |
| **跨发行版** | UID 是 Linux 概念,跨不出当前内核镜像 |
| **discardable / 一键回滚** | 多用户没有"丢弃这次会话所有改动"的语义 |
| **不要 root 就开新上下文** | `useradd` 必须 root |
| **macOS / Windows 上做同样事** | 平台多用户语义不同,概念不能直接搬 |
| **快速切换**(切上下文不重 ssh / 不重打开 IDE) | `sudo -iu` 不复用当前会话的 ssh-agent / TTY / IDE 进程 |
| **per-project tmpfs** | `/tmp` 在多用户下仍是共享(只有 sticky bit 防误删) |
| **per-project /opt/<name>** | `/opt` 是系统级 |

---

## 2. 各档相比 `useradd` 的边际价值速览

| 档位 | 多用户能近似替代吗? | 边际价值 | 是否值得保留 |
|---|---|---|---|
| **default**(env 注入) | **完全不能**(同一 UID 内切版本) | **极高** —— 这是 multi-user 根本不解决的问题 | 必须 |
| **medium**(HOME/tmp/opt 真隔离) | **能近似** —— 等同于 `useradd alice-class` | 中:省 root + 省切账户摩擦 + 跨平台 | 有保留价值,定位需收窄 |
| **heavy**(overlay,discardable) | **完全不能**(多用户无丢弃语义) | 高 —— 但人群窄 | 有意义但非 M1 |
| **full**(独立 rootfs) | **完全不能**(跨发行版/换 libc/教学) | 极高 | 必须(战略级,但晚做) |

---

## 3. 详细对比:逐档 PK 多用户

### 3.1 default ⟶ 多用户根本不解决的问题

**核心场景**:`alice` 想给 project-A 用 python 3.12 / node 20,给 project-B 用 python 3.10 / node 18。

| 方案 | 可行吗 | 痛点 |
|---|---|---|
| 多用户:`useradd alice-A && useradd alice-B`,各自装版本 | 可行 | 不同账户,IDE 项目要重开;ssh-key、gitconfig 要在两边复制;`sudo` 烦;切换=登出登入(或 `sudo -iu`,但丢失当前会话) |
| 单用户 + `pyenv` / `nvm` / `direnv` | 可行,业内主流 | 每语言一个工具,生态碎;CMake/`pkg-config` 不跟着切 |
| **xlings default** | 可行 | `xlings subos use A` 一行命令切;`xpkg install` 装哪都行;PATH/LD/CMAKE 一起切;新开 shell 自动对 |

**结论**:default 解决的"同一用户内多上下文切工具链"是 multi-user 根本碰不到的领域。**multi-user 不是 default 的替代,是不同维度**。

### 3.2 medium ⟶ 大致等价于 useradd,但有四个独有优势

medium 提供的"私有 HOME / 私有 /tmp / 私有 /opt"恰好就是 `useradd` 创建新账户时天然得到的。直接 PK:

| 维度 | `useradd alice-class` + `sudo -iu alice-class` | xlings medium subos |
|---|---|---|
| 私有 HOME | ✓ `/home/alice-class` | ✓ bind 到 `subos/<n>/home` |
| 私有 dotfiles | ✓ | ✓ |
| 私有 ssh-key | ✓ | ✓(默认无,可 `--share-ssh` 切回) |
| 私有 gitconfig | ✓ | ✓(同上) |
| 私有 /tmp | ✗(共享 /tmp,只是权限隔离) | ✓ tmpfs |
| 私有 /opt/<name> | ✗(/opt 共享) | ✓ |
| **需要 root** | ✓ 必须 | ✗ |
| **需要重 ssh / 切账户** | ✓(丢当前 TTY,重连重 ssh-agent) | ✗ 同一 TTY,`exit` 即返回 |
| **IDE / 编辑器复用** | ✗(IDE 进程在原账户,得重开) | ✗(进 ns 后 IDE 也要 wrap,但比"重开账户"成本低) |
| **跨平台** | ✗(macOS / Windows 多用户语义不同) | ✓ 退到 default 可用 |
| **快速创建/销毁多上下文** | 慢:`useradd` + chown + ssh-key 拷贝 + ... | 快:`xlings subos new --mode medium` 几秒 |
| **学生 / 受限用户机器**(无 sudo) | ✗ 不可行 | ✓ |
| **CI / 容器内** | 复杂(容器内 useradd 受限) | ✓ |
| **可移植**(把上下文带到另一台机器) | 复杂(要建同名用户、调 UID) | tar 整个 subos 即可 |

medium 真正的差异化卖点:

1. **无 root** —— 这条对学校机房 / 共享 dev 机 / rootless 容器决定生死。
2. **不切账户** —— 当前 ssh-agent / GPG-agent / IDE / 浏览器登录全保留。
3. **跨平台对称**(虽然非 Linux 上降级到 default,但 UX 一致)。
4. **轻量创建/销毁** —— `subos new` / `subos remove` 是秒级,不需要 chown 一堆文件。

**结论**:medium ≈ "**给单用户 / 无 root / 跨平台用户的 useradd 替身**"。如果用户**有 root**且**只用 Linux**且**愿意用 `sudo -iu`**,medium 的边际价值就薄了。**它的目标人群应该明确写在文档里**,不能含糊地"主打开发隔离"。

### 3.3 heavy ⟶ 多用户完全做不到 discardable

**多用户做不到的事**:
- "我 sudo -iu alice-test 进去装一堆 deb,改 /etc 试配置,玩完一键回到刚 useradd 的状态" —— `sudo -iu` 没这能力。要恢复就得 `userdel -r` 重建,所有的 dotfile、git history 同时全失。
- "我作为 alice 想试着 modify `/usr/lib/libc.so.6` 看效果" —— UID 不能写系统库。
- "改 `/etc/resolv.conf` 测 DNS,完了一键回退" —— 没法。

heavy 的卖点都是 multi-user 维度上的真空地带:

| 能力 | useradd | heavy |
|---|---|---|
| 改任何路径(包括 /usr、/etc、/bin) | ✗ | ✓ 写到 upper |
| 改完一键丢弃 | ✗ | ✓ `discard <n>` |
| 真宿主完全没动 | UID 隔离也做到了,但你只是没碰系统级 | ✓ overlay 强保证 |
| 不需要重启 / 不需要快照工具(Btrfs/ZFS) | — | ✓ |

**结论**:heavy 在 multi-user 维度上是**纯增量能力**,不重叠。但其受众是"想做系统级折腾又想回滚的人",窄。值得保留,但优先级低于 default 和 full。

### 3.4 full ⟶ 多用户连边都摸不着

`useradd` 在哪个发行版就是哪个发行版。要 Alpine 的 musl 行为、要 Debian Trixie 的 apt、要 RHEL 8 的旧 glibc —— 多用户帮不了。

| 能力 | useradd | full |
|---|---|---|
| 跨发行版交付作业 | ✗ | ✓ |
| 调研旧 glibc 行为 | ✗ | ✓ |
| 教学:Alpine 干净环境给学生 | ✗(每学生开账户也只是 Linux 主机的 distro) | ✓ |
| 替换 `/bin/sh`(dash vs bash vs busybox) | ✗ | ✓ |
| CI 跨机器一致 | ✗(机器 distro 不同就不一致) | ✓ |

**结论**:full 是 multi-user 维度上 100% 真空,无替代。

---

## 4. 总览表 —— 多用户视角下的"是否有意义"

| 维度 | default | medium | heavy | full |
|---|:-:|:-:|:-:|:-:|
| 多用户能近似替代 | **不能** | 能(useradd) | 不能 | 不能 |
| 主要差异化 | 同用户多上下文切版本 | 无 root + 不切账户 + 跨平台 + 轻量 | discardable 系统级折腾 | 跨发行版 / 换 libc |
| 目标人群广度(估) | ~80% | ~10% | ~3% | ~5% |
| 没了它谁会痛 | 切版本党(全员) | 学生机房 / rootless 容器 / 跨平台用户 | 系统折腾爱好者 | 教学 / CI / 跨发行版分发 |
| 多用户视角下的"独立性"评分(1-5) | 5 | 2(强重叠 useradd) | 5 | 5 |

---

## 5. 关键判断:哪些档"真的有意义"

把多用户考虑进来后:

1. **default 的地位反而更稳**。
   它是 multi-user 完全不覆盖的领域(同 UID 多上下文切版本),也是 90% 用户的真实场景。**没有 default 等于 xlings 价值减半**。

2. **medium 的定位必须收窄**。
   不能笼统说"主打开发隔离",而应明确:
   > **medium 是给"没 root / 不能切账户 / 跨平台 / 想要轻量上下文"的用户用的 useradd 替身。**
   有 root 且只用 Linux 的开发者,**直接 `useradd` 多账户实际上更标准、更易于调试和团队化**。文档应该坦诚指出这一点,而不是无差别地推 medium。

3. **heavy 的价值未被多用户撼动,但它的受众窄**。
   discardable + overlay 是 multi-user 没有的能力。但需要这个能力的用户少,且要内核 ≥5.11 + bwrap,**应该作为 M3+ 的可选档**,不抢 M1 焦点。

4. **full 是无可替代的战略品**。
   multi-user 完全不能跨发行版,也不能换 libc。**full 是 xlings 唯一能区别于"系统包管理 + 多用户"组合拳的高端能力**。

---

## 6. 三种可能的架构精简建议

### 方案 A —— 维持四档,但 medium 文档诚实定位

```
default     ← 主推,multi-user 不替代
medium      ← 标注"useradd-like, 无 root / 跨平台时用"
heavy       ← M3+,可选
full        ← 战略级
```

优点:范围齐全;缺点:medium 仍在主推路径上,而它跟 useradd 重叠多。

### 方案 B —— 三档:default / heavy / full(把 medium 砍掉)

逻辑:medium 跟 useradd 重叠太多,**让需要 medium 那种隔离的用户直接 useradd**;xlings 只覆盖 multi-user 不能覆盖的三个空间。

```
default     ← 同 UID 多上下文切版本
heavy       ← discardable 系统级折腾
full        ← 跨发行版 / 换 libc
```

优点:每档都跟 multi-user 不重叠,定位清晰;缺点:失去"无 root 私有 HOME"用户(数量不算少)。

### 方案 C —— 三档:default / isolated / advanced

把 heavy 和 full 合并成"advanced"(用户在 `--mode advanced --base alpine` 时进 full,空 base 时进 heavy),把 medium 重命名为 isolated 表明它就是"useradd 替代品"。

```
default      ← 同 UID 切版本
isolated     ← useradd 替身(无 root / 跨平台 / 轻量)
advanced     ← discardable 或 跨发行版
```

优点:命名直白;缺点:advanced 内部分支还是 heavy/full 两种实现,本质没省。

---

## 7. 推荐:方案 A,但 medium 文档诚实标注

倾向 **方案 A** 而非 B/C,理由:

1. **medium 的目标人群虽小但真实**:学生机房、共享 dev VM、rootless 容器、macOS/Windows 用户 —— 这些场景下 useradd 不可用。砍掉 medium 等于砍掉这部分用户,得不偿失。
2. **medium 的实现成本(+310 LOC)在已有 default 骨架上摊得开**,不像 heavy/full 是大跃进。
3. **更好的做法是文档诚实**:在 medium 章节的开头明确写:
   > 如果你**有 root**、**只在 Linux**、**能接受切账户**,直接 `useradd new-user` 实际上更标准、更易团队协作、更易调试。
   > medium 服务于:学生机房、rootless 容器、macOS/Windows、不想切账户的快速上下文切换。

这种"主动告诉用户什么时候不该用 medium"的文档,反而能筑起信任和清晰边界,是好开源工具的特质。

---

## 8. 给后续设计文档的具体回流建议

### 8.1 `four-tier-comparison.md` 增补

在 §1 总览之后插一节"§N. 与 Linux 多用户机制的关系",把 §1.1 / §1.2 / §4 的核心表搬进去。

### 8.2 `mode-implementation-details.md` medium 章开头加 callout

```markdown
> **使用建议**:medium 提供的"私有 HOME / 私有 /tmp / 私有 /opt"在 Linux 多用户机制下,本可以通过 `useradd new-user && sudo -iu new-user` 近似实现,且后者是更标准的解法。
> medium 的目标用户是:
> - 没有 root 权限的环境(学生机房、共享 dev VM、rootless 容器)
> - macOS / Windows(多用户语义不同,medium 自动降为 default 也保证 UX 一致)
> - 不想切账户、希望复用当前 ssh-agent / IDE / 浏览器登录的快速上下文切换
> - CI / 容器内的临时隔离上下文
> 如果以上都不是你,**直接 useradd 更合适**,xlings medium 不会比它做得更好。
```

### 8.3 不要做的事

- 不要把 medium 包装成"轻量 docker / chroot"等容器隐喻 —— 让用户混淆 mental model。
- 不要在 README 主推 medium,主推 default。
- 不要假装 multi-user 不存在 —— 跟 useradd 比较反而能让文档更可信。
