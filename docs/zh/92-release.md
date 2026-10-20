# 发布

[English](../92-release.md) | **简体中文**

xlings 的一个版本怎样到达用户。release 工作流变绿并不代表发布完成：还有两步在它之外。

## 版本号

`YYYY.M.D.N`，`N` 从 1 开始；`.0` 保留给正式版本。版本号写在三处且必须一致——`mcpp.toml`、`src/core/config.cppm`
和 `luban/os/service/src/service.cppm`（Luban OS 应用的）：luban、luban-os 与它们旁边的 xlings 一起发布。

## 步骤

1. 变更 squash 合入 `main`。
2. release 工作流构建 Linux（x86_64、aarch64）、macOS 和 Windows，发布 GitHub release，每个资产附 `.sha256`；luban 另有自己的资产（`luban-<version>-<platform>`，取自各平台 xlings 包的 `bin/luban`）。
3. **中国大陆镜像**：从中国大陆的机器把资产推到 GitCode（`tools/mirror-latest.sh xlings`，再 `luban`、`luban-os`、`xdesk`、`luban-cursors`），每个都下载（GET，不是 HEAD）并核对 sha256。
4. **索引**：xim-pkgindex 的 `pkgs/x/xlings.lua` 为每个平台写入新版本和 sha256，`latest` 指向它；`pkgs/luban/luban.lua`、`luban-os.lua`、`xdesk.lua` 同样，指针主题变了时还有 `cursors.lua`。
5. `main` 上重新运行 CI，重新生成发布的索引 artifact。
   测试已发布体验的作业等待它：release 触发的 `xlings-ci-luban-fresh` 先等待已发布索引中 `xlings` 与 `luban-os` 的 `latest`
   等于本版本。
6. 真实验证发布：全新安装、host matrix、索引的验收。

## CI 从不做的事

CI 从不在 fresh-install 套件里固定一个已发布的 xlings 版本：它安装 `latest` 解析到的版本，也就是首次安装的用户得到的版本
（`tests/fresh-install/no_xlings_version_pin_check.sh` 会强制这一点）。
