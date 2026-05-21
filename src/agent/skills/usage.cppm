// Skill: usage — Complete xlings usage guide for LLM agents.

export module xlings.agent.skills.usage;

import std;
import xlings.agent.skill;

namespace xlings::agent::skills {

export class UsageSkill : public Skill {
public:
    auto name() const -> std::string_view override { return "usage"; }
    auto description() const -> std::string_view override {
        return "Complete usage guide: install, search, remove, version switching, SubOS, project mode";
    }
    auto content() const -> std::string_view override { return kContent; }

private:
    static constexpr std::string_view kContent = R"SKILL(# xlings Usage Skill

You are using xlings, a developer tool version manager. This skill
teaches you how to use it correctly and efficiently.

## Rules

1. Add --yes to all install/remove commands to skip interactive prompts
2. Add --agent for clean output without TUI formatting
3. Use `xlings search` first if you are unsure about the exact package name
4. Check for .xlings.json in the project root before installing tools manually

## DO NOT

- Run xlings without --yes in non-interactive contexts (it will hang)
- Guess package names — always search first if unsure
- Install globally when a project .xlings.json exists (use project mode instead)

## Package Management

### Search (always do this first if unsure about package name)
  xlings search <keyword> [--agent]
  Example: xlings search gcc

### Install
  xlings install <pkg>@<ver> --yes [--agent]
  xlings install <pkg1> <pkg2> --yes [--agent]    # multiple at once
  Example: xlings install gcc@15 --yes
  Flags:
    -g, --global   Install to global scope (not project-local subos)
    -u, --use      Activate the installed version even if another is active

### Remove
  xlings remove <pkg>[@<ver>] [--agent]

### List installed
  xlings list [--agent]
  xlings list --all [--agent]         # across all subos environments

### Package info
  xlings info <pkg> [--agent]

### Update index
  xlings update [--agent]
  xlings update <pkg> [--agent]       # upgrade specific package

### Switch version
  xlings use <pkg> <ver> [--agent]
  xlings use <pkg>                     # list available versions
  Example: xlings use gcc 15

## SubOS (Isolated Environments)

Use SubOS when:
- Project needs tool versions that conflict with global versions
- Agent needs a sandboxed execution environment
- Multiple projects need different toolchains on the same machine

Commands:
  xlings subos new <name>                    # create
  xlings subos use <name>                    # enter (interactive shell)
  xlings subos use <name> --cmd "<command>"  # run single command inside
  xlings subos use <name> --sandbox          # filesystem-level isolation
  xlings subos list [--agent]               # list all
  xlings subos info [name]                   # show details
  xlings subos remove <name>                 # delete

## Project Mode

If the current directory (or a parent) has .xlings.json:
  {"workspace": {"gcc": "15.1.0", "node": "22.17.1"}}

Run `xlings install` with no arguments to install all declared tools.

## Decision Trees

### "I need to install a tool"
  1. Know exact name?   → xlings install <name>@<ver> --yes
  2. Unsure about name? → xlings search <keyword> → pick → install
  3. Need version list? → xlings info <name> → pick version → install

### "Build fails with 'command not found'"
  1. xlings list → is the tool installed?
  2. Not installed → xlings search + install
  3. Installed, wrong version → xlings use <tool> <version>
  4. Installed, still not found → restart shell (source profile)

### "Project has .xlings.json"
  1. Run xlings install (no args) → installs everything declared
  2. Need to add a new tool → xlings install <pkg>@<ver> --yes
     then add to .xlings.json workspace

## Key Flags Reference

  --yes / -y    Skip confirmation prompts (required for non-interactive use)
  --agent       Clean plain-text output, no TUI/ANSI formatting
  -g / --global Install to global scope (not project-local subos)
  -u / --use    Activate installed version even if another is active
  --all / -a    Show across all subos (for list/use commands)

## Configuration

  xlings config                        Show current config
  xlings config --lang <en|zh>         Set language
  xlings config --mirror <GLOBAL|CN>   Set download mirror

## Self-management

  xlings self update                   Update xlings itself
  xlings self doctor                   Verify workspace consistency
  xlings self clean                    Remove cache + orphaned packages)SKILL";
};

}  // namespace xlings::agent::skills
