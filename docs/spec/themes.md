# 配色主题 (Themes)

> 状态: 生效 (2026.8.22.1 起)
> 实现: `modules/theme/`(独立 mcpp 成员包)· `config/themes/*.json`(随发行包交付的样例)
> 使用说明: [`../theme.md`](../theme.md)

## 1. 选一个

```bash
xlings config --theme list                 # 内置 + config/themes/ 下的 + 当前生效的
xlings config --theme mono                 # 自带的
xlings config --theme ./my-theme.json      # 自己的,路径可以在任何地方
xlings config --theme default              # 回到编译进二进制的默认
```

`theme` 存的是一个**路径引用**。裸名字是 `config/themes/<name>.json` 的糖;带 `/`、`\`、`.json` 或以 `.` 开头的当作路径。

相对路径的基准和本地索引源同一条规则:来自项目配置就相对项目根,来自全局配置就相对 home。**所以一个项目可以自带配色**,和它自带索引源是同一个机制:

```json
// <project>/.xlings.json
{ "theme": "./tools/brand-theme.json" }
```

## 2. 写一个

主题是**部分覆盖**:只写想改的槽,其余回落到内置 `default`。没有 `extends` —— "没写"已经表示"继承"。

```json
{
  "name": "my-theme",
  "dark":  { "accent": "#4FC1FF", "muted": "#6A737D" },
  "light": { "accent": "#0969DA" }
}
```

`dark` 和 `light` **各自独立回落**。只写 `dark` 的主题在浅色终端下完全等于默认 —— 没写就是没意见,替作者猜一个他没看过的配色更糟。

颜色格式 `#RRGGBB` 或 `#RGB`。

## 3. 九个槽,封闭

| 槽 | 用于 | 从前叫 |
|---|---|---|
| `accent` | 强调、当前项、进度 | `cyan` |
| `alt` | 命令名、标识符 | `magenta` |
| `success` | 成功、已安装 | `green` |
| `warn` | 警告 | `amber` |
| `error` | 错误 | `red` |
| `text` | 正文 | `text` |
| `muted` | 次要信息、路径、说明 | `dim` |
| `border` | 分隔线、面板边 | `border` |
| `surface` | 背景 | `surface` |

**不接受其它键**,也**不支持按屏幕/命令覆盖**。后者会让主题文件和"有哪些屏幕"这张表耦合 —— 以后加一个屏幕就要同步改所有主题。

槽位名写错会报一条带 did-you-mean 的诊断,不会被静默忽略:

```
[warn] 'dark.acent' is not a colour slot
         from           @xlings/config/themes/broken.json
         did you mean   accent
```

## 4. 为什么是角色名而不是颜色名

改名之前,语义层(`theme::title()` 等)有 **7** 个调用点,颜色名访问器(`theme::cyan()` / `dim_color()` 等)有 **119** 个。也就是说 94% 的配色决策在点名"什么颜色",而不是"什么角色" —— 一个重映射角色的主题文件当时改不动任何东西。

按用途把那 119 处归类,和调色板已有的九个颜色**一一对应**:基数一直是对的,词汇是错的。

所以内置 `default` 与改名前的取值**逐字节相同**(`tests/unit/test_theme.cpp` 直接对着 `palette` 断言),这次改名在像素上是零变化。

**规约**:`ui/` 里不得出现按颜色命名的取色函数。出现即是回归。

## 5. 自带哪些

```
default          编译进二进制    好看
mono             config/themes/  无色差:色觉障碍、黑白终端、"别花哨"
high-contrast    config/themes/  极端环境:强光、投影、劣质终端
```

**就这三个。** 每个自带主题都要有人在 9 个槽 × 2 种背景下逐一看过,第四个是维护成本而不是选择自由。

`default` 不落盘 —— 它编译在二进制里,这正是让那两个文件成为**可选**的原因:配置目录被删、只读、还没初始化,xlings 依然有颜色可用。

## 6. 所有权

`~/.xlings/config/themes/` **属于 xlings**,升级时按版本标记重写(和 `config/shell/` 同款)。文件头写着:

```json
"_comment": "shipped by xlings; copy this file and point `xlings config --theme <path>` at the copy. Edits here are overwritten on upgrade."
```

要改就复制一份放到自己的路径,再 `xlings config --theme <那个路径>`。**所有权跟着"你指向哪里"走**,不靠目录约定 —— 所以没有 `official/` 这一层要解释。

## 7. 背景深浅是另一件事

`theme`(用哪套配色)和终端背景是深是浅(`XLINGS_THEME=dark|light|auto` / OSC-11 探测)是两个正交的问题。一套主题**同时**给出 dark 和 light 两组取值,由背景探测挑其中一组。

把两者合成一个键会让"我在浅色终端下想用 mono"变得无法表达。

## 8. 模块边界

`modules/theme` 是 workspace 的独立成员,**零依赖**(只用 `std` 和 JSON 读取器)。它拥有 `Rgb` 和 `Background`,而不是从 `core/palette` 借:

- `theme` 回答"用哪个颜色";
- `palette` 回答"这个终端能不能上色、背景是深是浅" —— 这个需要 `xlings.platform`。

保持这条分界,是让每个前端(含 `xlings-gui`)和迁移期仍在写 SGR 的 `core/log` 都能依赖它而不产生环。
