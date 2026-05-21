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
import xlings.agent.skills.contributing;

namespace xlings::agent::resources {

// ─── Overview (printed by `xlings agent`) ──────────────────────

export constexpr std::string_view kOverview = R"PROMPT([INSTRUCTION] You are interacting with xlings, a developer tool version
manager. READ THIS FULLY before running any xlings command.

[WHAT IS XLINGS]
xlings installs, manages, and switches between multiple versions of
development tools (compilers, runtimes, build systems) with isolated
environments (SubOS).

[CRITICAL — READ THE SKILL FIRST]
Before using xlings, you MUST run:
  xlings agent skills usage
This outputs the complete usage guide with rules, command formats,
flags, decision trees, and error handling — everything you need to
use xlings correctly. Do NOT guess command syntax.

[QUICK REFERENCE — use only AFTER reading the skill]
  xlings search <keyword>              Search packages
  xlings install <pkg>[@<ver>] --yes   Install (--yes skips prompts)
  xlings list                          List installed
  xlings use <pkg> <ver>               Switch version
  xlings info <pkg>                    Show package details
  xlings update                        Update package index

[FLAGS YOU MUST USE]
  --yes     REQUIRED for non-interactive use (skips confirmation prompts)
  --agent   Outputs clean plain text without TUI formatting (recommended)

[NEXT STEP]
Run `xlings agent skills usage` now to learn the full usage guide.)PROMPT";

// ─── Registry builder ──────────────────────────────────────────
//
// Adding a new skill:
//   1. Create src/agent/skills/<name>.cppm  (implement Skill interface)
//   2. Import its module above
//   3. Add a register call below

export SkillRegistry build_registry() {
    SkillRegistry reg;
    reg.add(std::make_unique<skills::UsageSkill>());
    reg.add(std::make_unique<skills::ContributingSkill>());
    return reg;
}

}  // namespace xlings::agent::resources
