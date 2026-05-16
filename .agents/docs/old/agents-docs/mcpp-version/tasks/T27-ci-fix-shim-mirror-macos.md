# T27 — PR #151 CI 修复：shim 初始化 + 镜像默认值 + macOS 兼容

> **Wave**: 依赖 T24/T25/T26（PR #151 合入后的修复）
> **PR**: [#151](https://github.com/d2learn/xlings/pull/151)
> **涉及文件**: CI YAML、E2E 脚本、C++ 平台模块、config 模板

---

## 1. 背景

PR #151 将 shim 创建从打包时改为安装时，引入 `config/xvm` 模板和统一发布脚本。
合入后 CI 三平台失败，原因各不相同。

---

## 2. 问题清单与修复分类

| # | 问题 | 根因 | 修复类型 | 方案 |
|---|------|------|---------|------|
| 1 | Phase 3 无 shim，xvm 无法创建新 shim | Phase 3 直接解压包、未运行 install/init | ✅ 正确修复 | Phase 3 setup 中调用 `xlings self init` |
| 2 | CI/E2E 索引同步超时（Gitee 不可达） | `config/xlings.json` 默认 `mirror: "CN"` | ✅ 根源修复 | 改 `config/xlings.json` 默认值为 GLOBAL |
| 3 | macOS xmake bundle SIGABRT 崩溃 | 下载的二进制缺少 ad-hoc 签名 | ✅ 正确修复 | 下载后 `codesign -s - -f` 重签名 |
| 4 | macOS 复制二进制后签名失效 | Apple Silicon 要求 ad-hoc codesign | ✅ 正确修复 | `make_files_executable` 对 Mach-O 文件调用 codesign |
| 5 | `platform::exec` 返回原始 waitpid 值 | `std::system()` 返回编码后的状态 | ✅ Bug 修复 | 用 `WEXITSTATUS`/`WIFSIGNALED` 提取正确退出码 |

---

## 3. 修复方案详解

### 3.1 Phase 3 添加 `xlings self init`（问题 #1）

**改动文件**：`.github/workflows/xlings-ci-linux.yml`、`.github/workflows/xlings-ci-macos.yml`

Phase 3 解压包后调用 `self init` 创建 shim：

```yaml
export PATH="$XLINGS_HOME/subos/current/bin:$XLINGS_HOME/bin:$PATH"
export XLINGS_HOME XLINGS_DATA="$XLINGS_HOME/data" XLINGS_SUBOS="$XLINGS_HOME/subos/current"
"$XLINGS_HOME/bin/xlings" self init
```

### 3.2 默认镜像改为 GLOBAL（问题 #2）

**改动文件**：`config/xlings.json`

**优化要点**：直接修改源头，而非在 4 处消费端用 jq 打补丁。

| 字段 | 修改前 | 修改后 |
|------|--------|--------|
| `mirror` | `"CN"` | `"GLOBAL"` |
| `xim.index-repo` | `gitee.com/...` | `github.com/d2learn/xim-pkgindex.git` |
| `xim.res-server` | `gitcode.com/...` | `github.com/xlings-res` |
| `repo` | `gitee.com/...` | `github.com/d2learn/xlings.git` |

CN 镜像保留在 `mirrors` 字段中，中国用户可通过编辑 `~/.xlings/.xlings.json` 手动切换。

### 3.3 macOS xmake bundle codesign（问题 #3）

**改动文件**：`tools/macos_release.sh`

下载的 xmake bundle 二进制缺少 Apple Silicon 要求的 ad-hoc 代码签名，导致 SIGABRT。
在下载后立即执行 `codesign -s - -f` 签名：

```bash
curl -fSsL -o "$XMAKE_BIN" "$XMAKE_URL"
chmod +x "$XMAKE_BIN"
codesign -s - -f "$XMAKE_BIN" 2>/dev/null || true
```

### 3.4 macOS codesign 优化（问题 #4）

**改动文件**：`core/platform/macos.cppm`

只对无扩展名或 `.dylib`/`.so` 文件签名，跳过 `.yaml`、`.json` 等非二进制文件：

```cpp
export void make_files_executable(const std::filesystem::path& dir) {
    if (!std::filesystem::exists(dir)) return;
    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        ::chmod(entry.path().c_str(), 0755);
        auto ext = entry.path().extension().string();
        if (ext.empty() || ext == ".dylib" || ext == ".so") {
            std::string cmd = "codesign -s - -f \"" + entry.path().string() + "\" 2>/dev/null";
            std::system(cmd.c_str());
        }
    }
}
```

### 3.5 修复 `platform::exec` 退出码（问题 #5）

**改动文件**：`core/platform.cppm`

```cpp
export int exec(const std::string& cmd) {
    int status = std::system(cmd.c_str());
#if !defined(_WIN32)
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return status;
#else
    return status;
#endif
}
```

---

## 4. 完整改动列表

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `config/xlings.json` | 修改 | 默认镜像从 CN → GLOBAL |
| `core/platform.cppm` | 修改 | `exec()` 正确提取退出码 |
| `core/platform/macos.cppm` | 修改 | `make_files_executable()` 优化 codesign |
| `.github/workflows/xlings-ci-linux.yml` | 修改 | Phase 3 添加 `self init` |
| `.github/workflows/xlings-ci-macos.yml` | 修改 | Phase 3 添加 `self init` + 镜像切换 |
| `tools/macos_release.sh` | 修改 | xmake bundle 下载后 codesign 签名 |
| `tests/e2e/linux_usability_test.sh` | 修改 | `prepare_runtime()` 添加镜像切换 |
| `tests/e2e/macos_usability_test.sh` | 修改 | `setup_env()` 添加镜像切换 |

---

## 5. 遗留问题

| 问题 | 建议 |
|------|------|
| CI 镜像切换依赖 jq | 可考虑给 xim 增加环境变量覆盖镜像的功能，替代 jq 补丁 |
