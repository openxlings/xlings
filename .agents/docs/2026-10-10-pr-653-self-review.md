# PR #653 / xim-pkgindex #945 自我 review（Luban OS part 2，2026-10-10）

- 范围：xlings PR #653（2026.10.10.3）与 xim-pkgindex PR #945 的重构。
- 依据：
  - `.agents/docs/2026-10-10-luban-os-design-part2.md`（含 §9 实施记录）；
  - 总图 `2026-10-10-luban-ecosystem-overview.md`；
  - 两个仓库的 CI；
  - 本机安装验证（limine、busybox、linux-kernel-virt、luban-boot-virt 从索引 checkout 经 CN 镜像安装）；
  - qemu 启动 virt 内核。

## 1. review 中发现并已修复

| # | 发现 | 怎么发现的 | 修复 |
|---|---|---|---|
| R1 | setuid bwrap 能进入，但不支持 `--disable-userns`；探测只测"能否进入" | #945 在 Ubuntu 24.04 runner 上的真实验收 | 探测测策略用到的能力；拒绝在进入之前；doctor 判定 |
| R2 | 不带版本的模板引用取"已安装的随便哪个" | 实现 upgrade 时读到 `locate_base_pkg_` | 取索引的 `latest`；退回时按版本排序 |
| R3 | `boot --now` 只认 `xlings-init` | 迁移 tiny 到 luban-init 时查引用 | 两者都认；新 tiny 的 `min_client` 指向有这个修复的客户端 |
| R4 | libc=musl 被一律拒绝 | 写 tiny-musl 时 | musl 可用；没有载荷的 libc 点名拒绝 |
| R5 | `-y` 是全局选项，被提前过滤 | 升级 e2e 失败（给了 -y 仍询问） | 传入 `yesGiven` |
| R6 | upgrade 复制模板文件时只跳过 `bin/`：一个带 `home/` 的模板会覆盖用户数据 | 用户数据规则下 review | 只替换 `usr/` 下的文件 |
| R7 | "up to date" 实际取决于本机索引的新旧 | UX review | 提示 `xlings update` |
| R8 | 容器里创建的文件归 root，清理 trap 失败使成功的构建报失败 | res-build CI | 容器结束前 chown |
| R9 | 本机安装写出的 `.xlings-index-cache.json` 被提交 | 提交前检查 | 恢复，原样保留 |
| R10 | 启动层是普通 `package`，规范要求 config 注册包名 | 索引静态测试 `test_spec_d1` | 加上 config / uninstall |
| R11 | hook 中 `os.arch()` 未绑定，按架构选 console 不可靠 | 读 glibc recipe 的注释 | virt 写两个 console，内核跳过不存在的 |

## 2. 分角度检查

**架构**
- 升级的判断（`edition::plan`）和"是否放宽"（`policy::loosened`）都在 `modules/subos`，是纯函数，有单元测试。cmd 只负责编排。
- 资源构建只有一个入口（`tools/res/build.sh`），发布只有一条路径（`xpkg_ci.py mirror`），和索引已有的 mirror 流程是同一套。
- xpkg 规范没有改动。

**稳定性**
- 宿主矩阵把"没有 sudo 的普通用户"和"AppArmor 限制开着"变成测试维度，PR 上跑 smoke。
- 一次性设置的场景由用户自己通过 sudo 执行（只开放这一步），和真实用户走的路径一样；以 root 执行会测不到限制。
- 资源的输入按上游摘要校验，产物在两个镜像上按内容核对。

**简洁**
- 用户只多了一个命令 `upgrade`。`--boot` 默认看不到：edition 会选好，`luban try` 选 virt。
- recipe 只剩数据，写盘代码只有一份。

**用户体验**
- 升级前列出计划；不确认就什么都不改（agent 模式下退出码 2，并给出 `-y`）。
- 用户自己的包和 edition 去掉的包保留，并说明原因。
- 放宽隔离不会随升级发生，会给出切换的命令。
- 没有 edition 记录的环境会如实说明，不去猜。

**兼容性与无感升级**
- `instance.json` 只追加字段。已发布的 0.1.0 写出的字节不变（golden）。
- 新字段在旧客户端上都能安全退化：`boot.profile` 被忽略时退回 `boot.kernel`；不带版本的包被旧客户端当作最新。
- 新 edition 用 `min_client` 要求 2026.10.10.3，从这个版本起由客户端执行。
- busybox 的 x86_64 下载地址和 sha256 不变，只新增 aarch64。

**跨平台**
- tiny 及其依赖都有 aarch64 包。
- aarch64 的镜像不在本轮（§9.3），如实记录。
- 宿主矩阵包含 24.04-arm。

**一致性**
- 日期版本只用于 Luban 自己的包；上游软件保持上游版本。
- 新 edition 全部 from nano，并使用 luban-init。

**安全与隐私**
- agent-private 要求的客户端会拒绝 setuid bwrap 带来的隐性降级。
- upgrade 在结构上不会放宽隔离。
- 资源有可复现的构建记录（README.provenance、config、输入摘要）。

**生态**
- 合入顺序：先 xlings 发布，后合入索引。
- 索引验收可以传入 xlings 的 PR 构建，在合入前就跑真实验收。

## 3. 保留的已知限制
- aarch64 镜像：`luban try` 只跑 x86_64，驱动器只放 BOOTX64.EFI，generic 启动层只有 x86_64。
- A7（mountinfo 露出宿主路径）与非 root 的 agent，见总图 R6。
- 宿主矩阵的 VM 腿和容器腿只在 full 中运行（每天定时，或手动触发）。PR 上只跑 smoke。
