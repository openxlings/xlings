# subos sandbox GPU 透传设计（`--gpu` 开关）

**日期**: 2026-05-22
**状态**: Design — ready to implement
**关联代码**: `src/core/subos.cppm`、`src/core/subos/gpu.cppm`（新增）
**关联讨论**: 调试 session 2026-05-22（NVIDIA 设备在 sandbox 内不可见）

---

## 1. 背景与动机

### 1.1 现状

`xlings subos use <name> --sandbox`（bwrap 后端）启用 mount namespace 隔离时，bwrap argv 包含：

```cpp
"--dev", "/dev",     // src/core/subos.cppm:1170
```

`bwrap --dev /dev` 的语义是**新建 tmpfs**，只填入硬编码的最小节点白名单：`null / zero / full / random / urandom / tty / console / stdin / stdout / stderr / ptmx / shm/ / pts/`。

宿主机的任何自定义字符设备 — 尤其是 NVIDIA 的 `/dev/nvidia*`、`/dev/nvidiactl`、`/dev/nvidia-uvm*`、`/dev/nvidia-modeset`，以及 DRM 的 `/dev/dri/*` — **不在白名单内 → sandbox 内全部不可见**。

同时 sandbox 完全不挂 `/sys`（`sandbox_binds_()` 和 `build_bwrap_argv_()` 都没绑），导致 libcuda / nvml 即便有设备节点也无法通过 `/sys/bus/pci/devices/...` 枚举到 GPU。

后果：sandbox 内 `nvidia-smi`、CUDA、Vulkan workloads 全部失败。

### 1.2 用户场景

- 在 sandbox 里跑 PyTorch / TensorFlow 推理与训练
- 在 sandbox 里编译 CUDA 项目（nvcc 不需要设备节点，但运行测试需要）
- 在 sandbox 里运行 ML notebook、本地大模型推理

### 1.3 为何不能默认开

直通字符设备会把宿主 GPU 资源直接暴露给沙箱进程。`sandbox-v5-dual-backend-design` 明确"MINIMAL host exposure"原则。GPU 透传只能 opt-in。

## 2. 设计

### 2.1 CLI 表面

```
xlings subos use <name> --sandbox --gpu                # 启用 GPU 透传
xlings subos use <name> --sandbox bwrap --gpu          # 显式后端，等价
xlings subos use <name> --sandbox proot --gpu          # 静默忽略（proot 已透传 /dev /sys）
xlings subos use <name> --gpu                          # 报错：--gpu requires --sandbox
```

**语义**：`--gpu` 是一个 bool flag，无参。开启后**对存在的设备节点进行透传**；不存在的节点静默跳过（不是错误）。

不引入 `auto` 模式 — 用户显式声明意图，避免"为什么我的 sandbox 看到了 GPU"的意外。

### 2.2 模块切分

新增 `src/core/subos/gpu.cppm`（与现有 `src/core/subos/keeper.cppm` 同级），导出：

```cpp
export module xlings.core.subos.gpu;
import std;

export namespace xlings::subos::gpu {

// 返回需要追加到 bwrap argv 的参数序列。
// 实现内部按"存在即添加，不存在即跳过"逻辑组装。
// `exists_fn` 可注入用于测试（默认实现走 std::filesystem::exists）。
std::vector<std::string> passthrough_args();
std::vector<std::string> passthrough_args(
    std::function<bool(const std::string&)> exists_fn);

} // namespace
```

`subos.cppm` 的 `build_bwrap_argv_` 只多一行：

```cpp
if (gpu) {
    auto extra = xlings::subos::gpu::passthrough_args();
    argv.insert(argv.end(), extra.begin(), extra.end());
}
```

所有"哪些设备、用什么 flag、为什么挂 /sys"的细节全部封装在 gpu.cppm 内。后续要加 AMD ROCm（/dev/kfd）、Intel Habana 等只动 gpu.cppm。

### 2.3 透传清单（v1 — 仅 NVIDIA + DRM）

| 设备节点 | 用途 | 处理 |
|---|---|---|
| `/dev/nvidiactl` | NVIDIA 驱动控制接口 | `--dev-bind` if exists |
| `/dev/nvidia-uvm` | Unified Memory（CUDA 必需） | `--dev-bind` if exists |
| `/dev/nvidia-uvm-tools` | UVM 调试工具 | `--dev-bind` if exists |
| `/dev/nvidia-modeset` | 显示模式设置 | `--dev-bind` if exists |
| `/dev/nvidia0` … `/dev/nvidia15` | 每张 GPU 一个节点 | 循环探测，存在即 `--dev-bind` |
| `/dev/dri` | DRM 子系统（含 card*/renderD*） | `--dev-bind` if exists |
| `/sys` | libcuda/nvml PCI 枚举 | `--ro-bind /sys /sys` 无条件追加（开 --gpu 时） |

**为什么 /sys 是无条件加而非"存在即加"**：`/sys` 在任何运行 Linux 用户态的机器上必然存在；不必探测。开 `--gpu` 即代表用户希望 GPU 栈工作 → 必须挂 /sys。

**为什么 nvidia0..15 而非动态扫描**：避免在 gpu.cppm 里引入目录遍历 + 字符设备过滤逻辑。16 个上限覆盖现实 99.99% 多卡场景（A100 8 卡、H100 8 卡 typical），不存在的节点 `--dev-bind` 会被 `exists_fn` 短路掉，零开销。

**未来扩展**（不在 v1）：
- AMD: `/dev/kfd` + `/dev/dri/renderD*`（DRM 已涵盖）
- Habana: `/dev/accel/accel*`
- 用 `XLINGS_GPU_EXTRA_DEVS` env 接受额外白名单（debug 用）

### 2.4 与现有 sandbox 后端的交互矩阵

| 配置 | 行为 |
|---|---|
| `--sandbox bwrap --gpu` | gpu.cppm 追加 args |
| `--sandbox proot --gpu` | 静默忽略（proot 走 `--bind /dev /dev` + `--bind /sys /sys`，本来就全透传） |
| `--sandbox --gpu`（auto-detect） | bwrap 命中 → 走 bwrap 分支启用；proot fallback → 同上静默 |
| `--gpu` 无 `--sandbox` | 解析层直接报错并退出 1：`--gpu requires --sandbox` |

### 2.5 错误处理

GPU 透传是 best-effort：
- 节点不存在：跳过，不警告（设计意图）
- bwrap 因 setuid 限制不能 bind 字符设备：bwrap 本身会失败并打印自身错误，无需 xlings 拦截

不做"宿主无 GPU 则警告" — 因为可能用户故意在没 GPU 的机器上验证流程；不应假设意图。

## 3. 实现要点

### 3.1 函数签名传递

新增 bool 沿调用链：

```
CLI parser (subos.cppm:2061)
  → use_spawn_shell(name, stream, sandbox, sandbox_backend, gpu, cmd)
    → use_sandbox_mode_(name, stream, sandbox_backend, gpu, cmd)
      → build_bwrap_argv_(..., gpu, ..., cmd)
```

参数位置：放在 `cmd` 之前（因为 cmd 是已有的 last positional，保持向后兼容）。所有 default 值 `bool gpu = false`。

### 3.2 build_bwrap_argv_ 插入点

```cpp
// build_bwrap_argv_ — 紧接现有 sandbox_binds_ 循环之后、tmpfs/chdir 之前
if (gpu) {
    auto extra = xlings::subos::gpu::passthrough_args();
    argv.insert(argv.end(), extra.begin(), extra.end());
}
```

放在 bind 循环之后是因为 `--dev-bind` 必须在 `--dev /dev` 之后才能往新建的 /dev tmpfs 上挂节点；放在 chdir 之前是 bwrap argv 顺序约定（exec 段必须最后）。

### 3.3 gpu.cppm 实现骨架

```cpp
export module xlings.core.subos.gpu;
import std;

export namespace xlings::subos::gpu {

inline std::vector<std::string> passthrough_args(
    std::function<bool(const std::string&)> exists_fn)
{
    std::vector<std::string> out;
    auto add_dev = [&](const std::string& p) {
        if (exists_fn(p)) {
            out.push_back("--dev-bind");
            out.push_back(p);
            out.push_back(p);
        }
    };
    // NVIDIA control + UVM + modeset
    add_dev("/dev/nvidiactl");
    add_dev("/dev/nvidia-uvm");
    add_dev("/dev/nvidia-uvm-tools");
    add_dev("/dev/nvidia-modeset");
    // Per-GPU nodes
    for (int i = 0; i < 16; ++i)
        add_dev("/dev/nvidia" + std::to_string(i));
    // DRM (Vulkan / display)
    add_dev("/dev/dri");
    // sysfs (unconditional when GPU is requested)
    out.push_back("--ro-bind");
    out.push_back("/sys");
    out.push_back("/sys");
    return out;
}

inline std::vector<std::string> passthrough_args() {
    return passthrough_args([](const std::string& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    });
}

} // namespace
```

无平台 ifdef — 整个模块只在 Linux 编译进 sandbox 路径，gpu.cppm 自身平台中立（Windows/macOS 不调用即可）。

### 3.4 CLI 解析改动

`src/core/subos.cppm:2061` 起的 `for (int i = 3; i < argc; ++i)` 循环里加：

```cpp
bool gpu = false;
// ... 现有 if/else if 链 ...
else if (a == "--gpu") {
    gpu = true;
}
```

`--sandbox` / `--gpu` 一致性校验放在 use 路径调用前：

```cpp
if (gpu && !sandbox) {
    usageError("--gpu requires --sandbox");
    return 1;
}
```

### 3.5 xmake.lua

无改动 — `add_files("src/**.cppm")` 已经递归吸收新模块。

## 4. 测试

### 4.1 单元测试（C++ / gtest）

`tests/unit/test_main.cpp` 新增：

```cpp
import xlings.core.subos.gpu;

TEST(SubosGpu, EmptyWhenNoDevicesExist) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string&) { return false; });
    // 至少包含 /sys 的 --ro-bind 三元
    EXPECT_GE(args.size(), 3u);
    EXPECT_EQ(args[args.size() - 3], "--ro-bind");
    EXPECT_EQ(args[args.size() - 2], "/sys");
    EXPECT_EQ(args[args.size() - 1], "/sys");
}

TEST(SubosGpu, IncludesNvidiactlWhenPresent) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string& p) { return p == "/dev/nvidiactl"; });
    auto it = std::find(args.begin(), args.end(), "/dev/nvidiactl");
    ASSERT_NE(it, args.end());
    EXPECT_EQ(*std::prev(it), "/dev/nvidiactl");          // dst
    EXPECT_EQ(*std::prev(it, 2), "--dev-bind");            // flag
}

TEST(SubosGpu, EnumeratesPerGpuNodes) {
    int hits = 0;
    auto args = xlings::subos::gpu::passthrough_args(
        [&](const std::string& p) {
            if (p.starts_with("/dev/nvidia") && p.size() > 11
                && std::isdigit(p[11])) { ++hits; return true; }
            return false;
        });
    EXPECT_EQ(hits, 16);  // 0..15 探测
}
```

### 4.2 e2e 测试（bash）

`tests/e2e/subos_sandbox_test.sh` 扩展或新增 `subos_sandbox_gpu_test.sh`：

```bash
# 1. --gpu without --sandbox → exit 1
run_x subos use default --gpu && fail "expected error"

# 2. --sandbox --gpu on machine without /dev/nvidiactl → still works
#    （只验证 flag 不会破坏 sandbox 启动，不验证 GPU 实际可用）
run_x subos use default --sandbox --gpu --cmd 'echo ok' | grep -q ok
```

CI 跑在 ubuntu-24.04 无 GPU runner，所以测试只覆盖：
- flag 解析正确
- 无 GPU 时 sandbox 仍能正常启动（exists_fn 全 false → 仅追加 /sys 绑定）

GPU 实际可用性测试需要在有 GPU 的机器手动验证（写入 PR 描述的 manual test 段）。

## 5. 文档

`docs/design/subos-isolation.md` 末尾追加一节：

```markdown
## GPU 透传

bwrap sandbox 默认隔离 /dev 和 /sys。需 GPU 时显式追加 `--gpu`：

    xlings subos use mygpu --sandbox --gpu

`--gpu` 会把宿主存在的 NVIDIA 设备节点（/dev/nvidia*、/dev/nvidia-uvm、
/dev/nvidiactl、/dev/dri）以 --dev-bind 透传进 sandbox，并以只读方式
绑定 /sys 供 CUDA / nvml 枚举 GPU。

proot 后端默认即透传 /dev 和 /sys，--gpu 在 proot 模式下被忽略。
```

## 6. 风险与权衡

| 风险 | 缓解 |
|---|---|
| `/sys` 全量 RO 绑定泄露宿主硬件拓扑信息 | 仅在 `--gpu` 时启用；用户显式 opt-in |
| GPU 节点直通使容器逃逸面变大 | 安全模型本来就是"用户信任的本地 sandbox"，不是多租户 |
| 16 卡循环上限太低 | 数年内不会触及（H100 8 卡是当前主流密度上限）；触及时升 32 即可 |
| AMD/Intel GPU 不支持 | v1 明确仅 NVIDIA；后续 PR 在 gpu.cppm 内扩展 |
| nvidia driver 升级后路径变化 | 路径常量集中在 gpu.cppm 一处，变更面小 |

## 7. 验收清单

- [ ] `src/core/subos/gpu.cppm` 新模块，导出 `passthrough_args(exists_fn=default)`
- [ ] `subos.cppm` CLI 接受 `--gpu`，校验 `--gpu` 需 `--sandbox`
- [ ] `build_bwrap_argv_` 在 `gpu==true` 时追加 gpu.cppm 返回的 args
- [ ] 3 个 gtest 用例覆盖空、单设备、多 GPU 枚举
- [ ] e2e 验证 `--gpu --sandbox --cmd 'echo ok'` 在无 GPU runner 上通过
- [ ] `docs/design/subos-isolation.md` 同步
- [ ] CI 全绿
