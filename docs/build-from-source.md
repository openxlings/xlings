> 编写日期: 2026-06-19 | 版本: 0.4.51

# 从源码构建

xlings 使用 [mcpp](https://github.com/mcpp-community/mcpp) 作为构建工具,构建依赖通过 xlings 自身声明在 `.xlings.json` 中。

```bash
# 1. 先安装 xlings(见 README 的「快速开始」)
# 2. 在仓库根目录安装构建依赖:
xlings install           # 读取 .xlings.json → 安装 mcpp 构建工具链

# 3. 构建与测试:
mcpp build
mcpp test
```

同一份 `.xlings.json` 同时驱动 CI 和 release 流水线,保证本地、CI 与发布环境一致。
