# 配色主题

> 使用说明。规约与设计理由见 [`spec/themes.md`](spec/themes.md)。

xlings 的终端配色由九个**角色槽**决定。主题是一份为这些槽指定颜色的 JSON 文件。

## 1. 设置

```bash
xlings config --theme list              # 列出可用主题与当前生效的那个
xlings config --theme default           # 回到内置默认
xlings config --theme mono              # 按名字选择
xlings config --theme ./my-theme.json   # 按路径选择
```

取值有三种形式:

| 形式 | 含义 |
|---|---|
| `default` | 编译进二进制的内置主题。不读磁盘,任何情况下都可用 |
| 裸名(如 `mono`) | `$XLINGS_HOME/config/themes/<名字>.json` |
| 路径(含 `/` 或以 `.json` 结尾) | 该文件 |

裸名**只**在 `$XLINGS_HOME/config/themes/` 下查找,不搜索当前工作目录。一个随工作目录变化的配置值不可复现。

路径相对于**声明它的那份配置**所在的目录:写在项目 `.xlings.json` 里的 `./themes/x.json` 相对项目根,写在 home 配置里的相对 home。

不存在的取值会被拒绝,并列出可用项 —— 设置动作不会报告成功却存下一个用不了的值。

## 2. 随发行版附带的主题

| 名字 | 用途 |
|---|---|
| `default` | 内置,不在磁盘上 |
| `mono` | 单色。适用于色觉障碍、单色终端,以及不希望终端花哨的场合 |
| `high-contrast` | 高对比。适用于阳光下、投影仪,以及调色板不佳的终端 |

`mono` 与 `high-contrast` 以文件形式随发行包交付到 `$XLINGS_HOME/config/themes/`。它们是**格式样例**,供复制修改。

这两个文件归 xlings 所有,升级时会被覆盖。要定制就复制一份改副本。所有权是**按文件**的:xlings 只覆盖它自己交付的那两个名字,你放进同一目录的其他文件不受影响。

本地从源码构建的 xlings 没有发行包,因此 `config/themes/` 可能是空的,`--theme list` 只会列出 `default`。这不影响使用。

## 3. 九个角色槽

主题指定的是**角色**而不是具体用途,因此新增的输出会自动获得协调的配色。

| 槽 | 用于 |
|---|---|
| `accent` | 主要强调:标题、当前选中项、命令名 |
| `alt` | 次要强调,与 `accent` 区分开的另一类高亮 |
| `success` | 成功、已完成、已就绪 |
| `warn` | 警告级诊断 |
| `error` | 错误级诊断 |
| `text` | 正文 |
| `muted` | 次要信息:路径、注释、说明性文字 |
| `border` | 分隔线与面板边框 |
| `surface` | 面板背景 |

## 4. 文件格式

```json
{
  "name": "my-theme",
  "dark": {
    "accent":  "#7AA2F7",
    "error":   "#F7768E"
  },
  "light": {
    "accent":  "#2E5AAC"
  }
}
```

- 颜色为 `#RRGGBB` 或 `#RGB`
- `dark` 与 `light` 分别对应深色与浅色终端背景
- **未写出的槽继承内置默认**,所以一份主题只需写它真正要改的部分

`dark` 与 `light` **各自独立**回退。只写 `dark` 不会把这些值镜像到 `light` —— 那等于把一套作者从未看过的配色交给浅色终端的用户。

## 5. 出问题时的行为

主题永远不会让 xlings 无法输出。每一种失效都会被报告,并回退到仍能工作的状态。

| 情形 | 行为 |
|---|---|
| 未设置,或设为 `default` | 使用内置默认,不读磁盘 |
| 文件不存在 | 警告并说明去向,回退内置默认 |
| 文件不可读 | 警告,回退内置默认 |
| JSON 非法 | 警告,回退内置默认 |
| 槽位名拼错 | 警告,并在拼写接近时给出建议;其余槽照常生效 |
| 颜色值非法 | 警告,**只有该槽**保持默认 |
| 部分槽未写 | 继承内置默认(这是设计,不是失效) |

关键性质:**局部错误只影响出错的那一槽**。整份丢弃会把"一行写错"放大成"整个主题不生效",而用户很难分辨后者与"这个设置根本没用"。

## 6. 自定义

```bash
cp "$XLINGS_HOME/config/themes/mono.json" ~/my-theme.json
$EDITOR ~/my-theme.json
xlings config --theme ~/my-theme.json
```

也可以直接放进 `$XLINGS_HOME/config/themes/`,之后用裸名选择:

```bash
cp mono.json "$XLINGS_HOME/config/themes/ocean.json"
xlings config --theme ocean
```

只要不与随发行版附带的名字(`mono`、`high-contrast`)相同,升级不会动它。
