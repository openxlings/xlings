# Luban OS 安卓版：初步分析（预研记录，未来再做）

日期：2026-10-20　状态：预研结论，未排期　范围：在手机上不经 Termux 直接使用 Luban OS（aarch64）

## 1. 已确定的方向

- 自己发布 APK（GitHub release 与 CN 镜像），先不上 Google Play（其"下载可执行代码"政策，Termux 因此离开 Play）。
- 有 root 用 root（`su` 下 chroot + mount namespace，接近原生）；无 root 用 proot（ptrace 转换系统调用）。两者是一个新的 `android` carrier，与 WSL2、vz 并列。
- 复用 luban-os 的界面（SDL3 + imgui）。它本来就经接口（NDJSON）驱动 xlings；系统里的 xlings 用现成的 aarch64 静态构建。
- `import std` 不退化到头文件；缺什么就做最小的中间层。

## 2. 本地实测（2026-10-20，x86_64 主机，无 KVM）

| 项目 | 结果 |
|---|---|
| `import std` + `std::println`，`mcpp build --target aarch64-linux-android` | 通过。mcpp 自动选 `android-ndk@30.0.16248370`（clang 21，NDK 自带 `share/libc++/v1/std.cppm`）。产物在 qemu-user + Android arm64 系统镜像（`xim:android-system-image` 24-default-arm64-v8a 的 `qemu-user-root`）上运行，输出正确。API 24 的 linker 对 `DT_FLAGS_1` 警告，无害 |
| compat.imgui 1.92.8-docking | 通过：两帧后 1 个 draw list、80 个顶点 |
| compat.sdl3 3.4.2 | 失败：没有 Android 分支，Android 落到 Linux 分支，去编 X11（bionic 上 `mblen` 未声明） |
| `modules/platform` | 3 个文件 9 个错误：`memfd_create`（API 30）、`process_vm_readv`、`sethostname`（API 23），以及 `isolation.cpp` 里一处 `for (auto& c : path)` 绑定临时对象（libc++ 下报错，改 `const auto&` 即可，对所有平台都更正确） |
| 提高 API level | mcpp 固定按 `aarch64-unknown-linux-android21` 编译，忽略 triple 后缀。只给用户代码加 `--target=…android30` 会与按 android21 预编译的 `std.pcm` 冲突（`-Wmodule-file-config-mismatch`），所以 API level 必须是 mcpp 的 target 属性，参与指纹，std 模块按同一 API 编 |

## 3. 最小中间层（三处）

1. **mcpp**：`[target.aarch64-linux-android] api = 29`（或接受 `aarch64-linux-android29`），std 模块和用户代码按同一 API level 编，计入指纹。
2. **compat.sdl3（mcpplibs 索引）**：Android 分支。用 SDL 自带的 `include/build_config/SDL_build_config_android.h`，编 `src/core/android`、`src/video/android` 等，链接 `android log EGL GLESv2`；`android-project/app/src/main/java/org/libsdl/app/*.java`（SDLActivity 等）用 d8 编成 dex。这些 SDL 3.4.2 源码里都有。
3. **`modules/platform`**：`platform::is_android` 分支，只补 bionic 缺的接口（API 29 下约剩 `memfd_create` 走 `syscall()`）。

打包：index 已有 `android-build-tools`（aapt2、d8、zipalign、apksigner）、`android-platform`（android.jar）、`android-debug-keystore`、`android-emulator`、`android-platform-tools`（adb）。还缺 APK 打包一步（脚本或 `mcpp pack` 的 apk 形态）。luban-os 编成 `libmain.so` 导出 `SDL_main`。

## 4. 风险

- **执行限制**：Android 10 起应用不能执行数据目录里的文件；可执行的二进制（proot、xlings）放在 APK 的 nativeLibraryDir，命名为 `lib*.so`。系统里的程序由 proot 经它们自己的 ld.so 加载。
- **phantom process killer**（Android 12 起）：应用派生的后台进程超过 32 个会被杀。运行系统时要前台服务加常驻通知；Android 14 起可引导用户在开发者选项里关掉。另需电池优化白名单。
- **性能**：proot 下系统调用多的工作（编译、解包）慢约 2–5 倍；root 模式接近原生。
- **存储**：系统在应用私有目录，卸载即删；界面要事先说明，提供 `luban export`。
- **验证环境**：本机无 KVM，模拟器只能 TCG，很慢；APK 的界面验证需要真机（有 root、无 root 各一台）。

## 5. 分阶段

| 阶段 | 内容 | 判断标准 |
|---|---|---|
| S0 | mcpp 的 API level；compat.sdl3 的 Android 分支；最小 SDL3 + imgui + `import std` APK；真机上 proot/chroot + 静态 xlings 跑 `luban new box --edition tiny`、进入、跑 xdesk | 能编、能跑、性能可接受 |
| S1 | `android` carrier（root/proot）、界面移植、触摸映射（单击、长按 = 右键、双指滚动、软键盘）、前台服务 | 手机上建系统、进控制台 |
| S2 | 桌面（xdesk 软件渲染，opensal 走应用数据目录里的 unix socket）、外接键鼠与三星 DeX、APK 发布流程与 CI（模拟器测界面，真机手动验证） | 手机上进入 xdesk 桌面 |

最低 Android 10（API 29），只做 aarch64。
