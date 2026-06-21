# Root / 特权身份感知 架构设计

> 日期: 2026-06-21
> 类型: 设计 (design)
> 调研依据: [`2026-06-21-linux-root-usability-survey.md`](./2026-06-21-linux-root-usability-survey.md)
> 目标: 让 xlings 在 Linux root / sudo 下行为正确,**且不影响任何现有平台(Linux 非 root / macOS / Windows)与功能**

## 1. 问题本质

调研结论:xlings 全链路**没有任何 EUID/root/sudo 感知**(唯一的 `::getuid()` 在 `subos.cppm:1416`,仅用于 NSS 模板)。「执行身份」这个横切关注点被散落成隐式 ambient 读取——到处 `getenv("HOME")`、硬编码 `"sudo "`、裸 `fs::create_directories`,没有一处「知道我是谁、文件该归谁」。

由此产生 7 个症状(详见调研),核心两个:
- **P0-1**: `subos.cppm:319/327/337` 硬编码 `sudo mount/umount/chown`,纯 root 容器(常无 sudo)直接 `sudo: command not found`。
- **P0-2**: `sudo` 安装把 `root:root` 文件写进用户 `~/.xlings`,后续非 sudo 调用 `EACCES`,且错误被吞(`installer.cppm:1228-1230`)。

## 2. 业界最佳实践对标

| 传统 | 代表 | 做法 | 对应 xlings |
|---|---|---|---|
| 用户态安装器 | Homebrew / makepkg | **拒绝**以 root 运行 | install 路径可选「拒绝/警告」 |
| | npm | 以 root 运行时**降权**到目录属主(读 `SUDO_UID`) | install 路径首选「降权 / chown-back」 |
| | pip / nvm | **警告** + 文档 | 最低成本止血 |
| 特权原语工具 | Podman / bubblewrap | **窄 setuid 助手**只包单个特权原语;rootless + FUSE | subos image:`priv` 边界 + 二期 rootless |
| | sshd / polkit | **特权分离**:特权部分隔离成极小组件 | `priv::run()` 单一出口 |

**提炼**: *用户态工作以真实用户身份做(拒绝 root / 降权 / chown-back);特权只用窄边界包住,能 rootless 就 rootless。* `sudo` 故意保留 `SUDO_UID/GID/USER` 正是为了让程序能降权——这是机制基础。

## 3. 架构设计

### 3.1 核心:`ExecIdentity` 一等概念

执行身份**解析一次、不可变**,集中在 `platform` 层,消除散落的 ambient 读取。

```cpp
// src/platform/  —— :unix 分区实现 (linux+macos),:windows 分区给 stub
namespace xlings::platform {

struct SudoInvoker {        // 仅当「root 且经 sudo 提权」时存在
    unsigned int uid;       // SUDO_UID
    unsigned int gid;       // SUDO_GID
    std::string  user;      // SUDO_USER
};

bool is_root();                         // Linux/macOS: geteuid()==0 ; Windows: false
std::string priv_prefix();              // is_root() ? "" : "sudo "
std::optional<SudoInvoker> sudo_invoker();  // is_root() && SUDO_UID 存在 ? {...} : nullopt
void chown_to_invoker(path, recursive); // 仅当 sudo_invoker() 有值时执行,否则 no-op
}
```

### 3.2 操作二分 + 特权边界

| 类别 | 操作 | 策略 |
|---|---|---|
| 用户态(99%) | download / extract / 写 `~/.xlings` / shim / 版本 DB | 经 sudo 时 **chown-back 到 invoker**;并在入口**警告** |
| 特权(仅 subos image) | `mount -o loop` / `umount` / `chown` | 全部经 `priv_prefix()`:已是 root → 不加 sudo;否则加 |

### 3.3 不影响现有平台/功能的安全保证(关键)

这是本设计的硬约束,逐条对齐:

1. **非 root 行为零变化**:`priv_prefix()` 在非 root 时返回 `"sudo "`,与现有硬编码**逐字节相同**。所有现有 Linux 非 root 流程、CI(runner 非 root)、macOS 完全不变。
2. **新路径门控**:`chown_to_invoker` / 警告只在 `is_root() && SUDO_UID` 这个**全新条件**下触发;现有用户从不进入。
3. **Windows 编译安全**:`:windows` 分区提供同签名 stub(`is_root()=false`、`priv_prefix()=""`、`sudo_invoker()=nullopt`、`chown_to_invoker()=no-op`),subos 特权路径本就是 Linux-only。
4. **macOS 复用 POSIX**:`:unix` 分区(linux+macos 共享)用 `geteuid`/`::chown`,与现有 `query_terminal_is_light` 同模式。
5. **纯 root 容器**:无 `SUDO_UID` → `chown_to_invoker` no-op,文件 root 属主一致、无害;`priv_prefix()` 去掉冗余 sudo → 修复 P0-1。

## 4. 实施方案(分期)

### Phase 1 — 本 PR(止血 + 地基,低风险)
- [ ] **P1.1** `platform` 增加 identity helpers(`:unix` 实现 + `:windows` stub + `platform.cppm` 再导出)。
- [ ] **P1.2** subos `319/327/337` 三处 `sudo ` → `platform::priv_prefix() + ...`。(修 P0-1)
- [ ] **P1.3** `self install` 成功后:`sudo_invoker()` 有值则 `chown_to_invoker(XLINGS_HOME)`;入口在 root 下打印警告(纯 root info / sudo warn)。(修 P0-2/P1-3 主入口)
- [ ] **P1.4** 单元测试:`priv_prefix()` 非 root 返回 `"sudo "`;`parse_sudo_env_` 纯函数解析。
- [ ] **P1.5** e2e:`root_usability_test.sh` —— root 下 self install + install fixture 包 + 校验可用性 + sudo 场景属主回归。
- [ ] **P1.6** 新增 CI `xlings-ci-linux-root.yml`:mcpp 构建最新 xlings → root 场景验证。

### Phase 2 — 后续 PR(完整覆盖)
- [ ] xim `cmd_install` 后对 package store / runtimedir chown-back;`installer.cppm:1228` 不再吞 `ec`。
- [ ] `shim.cppm` 区分 `EACCES` 与「未注册」,提示属主问题(修 P2-5)。
- [ ] sandbox root passwd 去重(`user=="root"` 不写第二条,修 P1-4)。
- [ ] extract mode 位掩码 `& ~(S_ISUID|S_ISGID|S_IWOTH)`(修 P2-6)。
- [ ] `ExecIdentity` 注入 `Config`,解析一次而非按需 syscall。

### Phase 3 — 进阶(架构升级)
- [ ] 用户态命令在 `via_sudo` 时**降权**到 invoker(npm 模式),属主由构造保证。
- [ ] subos image 改 rootless(user namespace + FUSE,如 `fuse2fs`),彻底去 `mount` 的 root 依赖(Podman 模式)。
- [ ] 入口策略门:root 但 home 属主是别人 → 明确拒绝(Homebrew 模式)。

## 5. 测试策略

- **单元**(`tests/unit/`,非 root 即可跑):纯函数 `parse_sudo_env_`、`priv_prefix()` 在非 root 的返回。
- **e2e**(`tests/e2e/root_usability_test.sh`):用构建出的 release 二进制,在受控 `HOME` 下以 root / 模拟 sudo(设 `SUDO_UID/GID/USER`)跑 self install + install,断言:① 二进制可用;② 经 sudo 时安装产物属主 = invoker;③ `priv_prefix` 行为正确。
- **CI**(`xlings-ci-linux-root.yml`):先用 mcpp 构建最新 xlings,再在 root 场景跑上述 e2e,**持续验证当前版本在 root 下的可用性**(回归护栏)。

## 6. 关键代码位置

- `src/platform/unix.cppm` / `windows.cppm` / `platform.cppm` — identity helpers + 再导出
- `src/core/subos.cppm:319/327/337` — sudo 硬编码 → `priv_prefix()`
- `src/core/xself/install.cppm` — self install 后 chown-back + root 警告
- `tests/unit/test_identity.cpp`、`tests/e2e/root_usability_test.sh`、`.github/workflows/xlings-ci-linux-root.yml`
