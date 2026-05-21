// xlings.agent.resources — overview text + skill registry builder.
//
// The overview is the static text printed by `xlings agent`.
// The registry is built by importing each skill module and registering
// it.  To add a new skill, add its module import and a register call
// in build_registry().

export module xlings.agent.resources;

import std;

import xlings.agent.skill;
import xlings.agent.skills.usage;

namespace xlings::agent::resources {

// ─── Overview (printed by `xlings agent`) ──────────────────────

export constexpr std::string_view kOverview = R"PROMPT(xlings — Developer tool version manager

A CLI tool for installing, managing, and switching between multiple
versions of development tools (compilers, runtimes, build systems)
with isolated environments (SubOS).

Quick start (if you just need to run a command now):
  xlings search <keyword>              Search packages
  xlings install <pkg>[@<ver>] --yes   Install (--yes skips confirmation)
  xlings list                          List installed
  xlings use <pkg> <ver>               Switch version
  xlings info <pkg>                    Show package details
  xlings update                        Update package index

Tip: Add --agent flag to any command for clean plain-text output
     without TUI formatting (no ANSI codes, no progress bars).

Run `xlings agent skills` to see all available skill guides.
Run `xlings agent skills <name>` for a specific skill.)PROMPT";

// ─── Registry builder ──────────────────────────────────────────
//
// Adding a new skill:
//   1. Create src/agent/skills/<name>.cppm  (implement Skill interface)
//   2. Import its module above
//   3. Add a register call below

export SkillRegistry build_registry() {
    SkillRegistry reg;
    reg.add(std::make_unique<skills::UsageSkill>());
    return reg;
}

}  // namespace xlings::agent::resources
