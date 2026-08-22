export module xlings.i18n.en;

import std;
import xlings.i18n;

// The BASE catalogue. Must be complete: every key any other language may
// override has to exist here, because English is the last stop before a key
// renders as itself. `I18nCatalogue.EnglishIsComplete` pins that.
//
// WHAT IS AND IS NOT IN HERE
//
// Rule: anything you can type into a terminal stays as it is; the prose that
// explains it gets translated. So command names (`install`), flag names
// (`--yes`), placeholders (`<version>`), config keys, environment variable
// names (`XLINGS_HOME`) and format arguments (versions, paths, package names)
// never appear as values here -- only as parts of the sentences around them.
//
// `SubOS` is a coined product noun and keeps its spelling when it names the
// concept; a language may still translate it where it reads as an ordinary
// noun inside a sentence.
export namespace xlings::i18n::en {

inline constexpr Entry kEntries[] = {
    // ── Help ─────────────────────────────────────────────
    { "help.usage",              "USAGE" },
    { "help.subcommands",        "SUBCOMMANDS" },
    { "help.options",            "OPTIONS" },
    { "help.args",               "ARGS" },
    { "help.examples",           "EXAMPLES" },
    { "help.for_ai_agents",      "FOR AI AGENTS" },
    { "help.agent_hint",
      "If you are an LLM/AI agent, run `xlings agent` FIRST.\n"
      "It contains usage instructions designed specifically for you." },
    { "help.tagline",
      "A modern package manager and development environment tool" },

    // ── Interactive prompts ──────────────────────────────
    // The keys line and the picker titles. Prompts are user-facing prose and
    // are translated; what the keys DO (arrows, enter, esc) is not text the
    // user types, so the glyphs stay.
    { "ui.select_keys",        "up/down move   enter select   esc cancel" },
    { "ui.select_package",     "Select a package" },
    // ── Subcommand descriptions ──────────────────────────
    // The NAMES are literal in the source; only what we say about them is a
    // key. `install` must stay `install` in every language or the help stops
    // describing this program.
    { "cmd.install.desc",  "Install packages (e.g. xlings install gcc@15 node)" },
    { "cmd.remove.desc",   "Remove a package" },
    { "cmd.update.desc",   "Update package index or a specific package" },
    { "cmd.search.desc",   "Search for packages" },
    { "cmd.list.desc",     "List installed packages" },
    { "cmd.info.desc",     "Show package information" },
    { "cmd.use.desc",      "Switch tool version" },
    { "cmd.config.desc",   "Show or modify configuration" },
    { "cmd.subos.desc",    "Manage sub-OS environments" },
    { "cmd.self.desc",     "Manage xlings itself (install, update, clean)" },
    { "cmd.script.desc",   "Run xlings scripts" },
    { "cmd.agent.desc",    "Built-in skills and plain-text mode for LLM agents" },

    // ── Global option descriptions ───────────────────────
    { "opt.yes.desc",      "Skip confirmation prompts" },
    { "opt.verbose.desc",  "Enable verbose output" },
    { "opt.quiet.desc",    "Suppress non-essential output" },
    { "opt.agent.desc",    "Plain-text output for LLM agents (no TUI/ANSI)" },

    // ── `xlings config` field labels ─────────────────────
    // Left column only. The values are paths, names and versions, which are
    // data rather than prose.
    { "config.active_subos",     "active subos" },
    { "config.bin",              "bin" },
    { "config.mirror",           "mirror" },
    { "config.lang",             "lang" },
    { "config.ui_mode",          "ui mode" },
    { "config.theme",            "theme" },
    { "config.interactive",      "interactive" },
    { "config.index_repo",       "index-repo" },
    { "config.project_data",     "project data" },
    { "config.project_repo",     "project repo" },
    { "config.default_marker",   "default" },

    // ── Words that appear inside rendered values ─────────
    { "common.none",             "(none)" },
    { "common.builtin",          "built in" },
    { "common.more",             "more" },
    { "common.cancelled",        "cancelled" },
};

}  // namespace xlings::i18n::en
