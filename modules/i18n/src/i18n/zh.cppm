export module xlings.i18n.zh;

import std;
import xlings.i18n;

// Chinese. An OVERLAY: state only what differs from English, and anything
// omitted falls back per key rather than per file. That is what lets a
// translation land incrementally instead of all at once, and it is the same
// rule the shipped themes follow against the built-in default.
//
// Command names, flags, placeholders and environment variable names are absent
// on purpose -- see en.cppm for the rule.
export namespace xlings::i18n::zh {

inline constexpr Entry kEntries[] = {
    // ── 帮助 ─────────────────────────────────────────────
    { "help.usage",              "用法" },
    { "help.subcommands",        "子命令" },
    { "help.options",            "选项" },
    { "help.args",               "参数" },
    { "help.examples",           "示例" },
    { "help.for_ai_agents",      "面向 AI 助手" },
    { "help.agent_hint",
      "如果你是 LLM / AI 助手,请先运行 `xlings agent`。\n"
      "那里有专门为你准备的使用说明。" },
    { "help.tagline",
      "现代化的包管理与开发环境工具" },

    // ── 子命令描述 ───────────────────────────────────────
    { "cmd.install.desc",  "安装软件包(例:xlings install gcc@15 node)" },
    { "cmd.remove.desc",   "移除一个软件包" },
    { "cmd.update.desc",   "更新包索引,或更新指定的包" },
    { "cmd.search.desc",   "搜索软件包" },
    { "cmd.list.desc",     "列出已安装的软件包" },
    { "cmd.info.desc",     "查看软件包信息" },
    { "cmd.use.desc",      "切换工具版本" },
    { "cmd.config.desc",   "查看或修改配置" },
    { "cmd.subos.desc",    "管理 SubOS 子系统环境" },
    { "cmd.self.desc",     "管理 xlings 自身(安装、更新、清理)" },
    { "cmd.script.desc",   "运行 xlings 脚本" },
    { "cmd.agent.desc",    "内置技能与面向 LLM 助手的纯文本模式" },

    // ── 全局选项描述 ─────────────────────────────────────
    { "opt.yes.desc",      "跳过确认提示" },
    { "opt.verbose.desc",  "输出详细信息" },
    { "opt.quiet.desc",    "抑制非必要输出" },
    { "opt.agent.desc",    "面向 LLM 助手的纯文本输出(不使用 TUI / ANSI)" },

    // ── `xlings config` 字段标签 ─────────────────────────
    { "config.active_subos",     "当前子系统" },
    { "config.bin",              "可执行目录" },
    { "config.mirror",           "镜像" },
    { "config.lang",             "语言" },
    { "config.ui_mode",          "界面模式" },
    { "config.theme",            "主题" },
    { "config.interactive",      "交互模式" },
    { "config.index_repo",       "索引仓库" },
    { "config.project_data",     "项目数据" },
    { "config.project_repo",     "项目仓库" },
    { "config.default_marker",   "默认" },

    // ── 渲染值里出现的词 ─────────────────────────────────
    { "common.none",             "(无)" },
    { "common.builtin",          "内置" },
    { "common.more",             "更多" },
    { "common.cancelled",        "已取消" },
};

}  // namespace xlings::i18n::zh
