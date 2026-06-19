> 编写日期: 2026-06-19 | 版本: 0.4.51

# 与其他工具对比

xlings 与常见工具的定位对比:

| | apt / brew | nix | docker | **xlings** |
|---|:---:|:---:|:---:|:---:|
| 多版本共存 | ❌ | ✅ | ✅ | ✅ |
| 无需 Root | ❌ | ⚠️ | ⚠️ | ✅(image 模式除外)|
| 无 daemon | ✅ | ✅ | ❌ | ✅ |
| 跨平台统一命令 | ❌ | ⚠️ | ✅ | ✅ Linux / macOS / Windows |
| 隔离粒度 | ❌ | FS | FS+ | 🔒 shell / FS / image 三级 |
| 存储复用 | — | ✅ store | ❌ 镜像膨胀 | ✅ 版本视图 + 引用计数 |
| 去中心化索引 | ❌ | ❌ | ❌ | ✅ 官方 + 第三方 + 自建 |
| Agent / JSON 接口 | ❌ | ❌ | ⚠️ API | ✅ `xlings interface`(NDJSON)|
| 可作 OS 级包管理器 | apt 本身是 | NixOS | ❌ | ✅(Luban Linux,即将推出)|
