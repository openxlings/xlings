// Skill: usage — Complete xlings usage guide for LLM agents.
// Written as a system-prompt-style instruction set, not documentation.

export module xlings.agent.skills.usage;

import std;
import xlings.agent.skill;
import xlings.cli.spec;

namespace xlings::agent::skills {

export class UsageSkill : public Skill {
public:
    auto name() const -> std::string_view override { return "usage"; }
    auto description() const -> std::string_view override {
        return "IMPORTANT: Read this FIRST. Complete usage instructions for AI agents.";
    }
    auto content() const -> std::string_view override {
        static const std::string generated = [] {
            std::string value{kContent};
            constexpr std::string_view marker = "@@COMMAND_REFERENCE@@";
            if (const auto at = value.find(marker); at != std::string::npos) {
                value.replace(at, marker.size(), cli::spec::agent_reference());
            }
            return value;
        }();
        return generated;
    }

private:
    static constexpr std::string_view kContent = R"SKILL([SYSTEM INSTRUCTION — xlings usage for AI agents]

You are now equipped with xlings, a developer tool version manager.
Follow these instructions precisely when using xlings commands.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
RULES — you MUST follow these, no exceptions
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. ALWAYS add --yes to install/remove/update commands.
   Without it, xlings will prompt for confirmation and HANG in
   non-interactive mode.

2. ALWAYS add --agent for clean output.
   Without it, output contains ANSI escape codes and TUI formatting
   that are difficult to parse.

3. NEVER guess package names. Run `xlings search` first.

4. Before installing anything, check if the project has .xlings.json.
   If it does, run `xlings install --yes --agent` with no package args
   to install everything the project declares.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
COMMAND REFERENCE
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
@@COMMAND_REFERENCE@@

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
DECISION TREES — follow these, do not improvise
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

WHEN you need to install a tool:
  → Know exact package name?
    YES → xlings install <name>@<ver> --yes --agent
    NO  → xlings search <keyword> --agent
          → read the results
          → xlings install <exact-name>@<ver> --yes --agent

WHEN a build fails with "command not found" or wrong version:
  1. xlings list --agent              → is the tool installed?
  2. NOT installed                    → search + install
  3. Installed but WRONG version      → xlings use <tool> <ver> --agent
  4. Too OLD (e.g. gcc 13 for C++23) → xlings install <tool>@<newer> --yes -u --agent
  5. Installed but still not found    → user must restart shell

WHEN the project has .xlings.json:
  → xlings install --yes --agent      (no args — installs all declared)
  → to add a new tool: install it, then update .xlings.json

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
SubOS — ISOLATED ENVIRONMENTS
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Use when: version conflicts between projects, sandbox needs, or
multiple toolchains on one machine.

  xlings subos new <name>                 # create
  xlings subos use <name>                 # switch to it
  xlings subos use <name> --cmd "<cmd>"   # run one command inside
  xlings subos use <name> --sandbox       # filesystem isolation
  xlings subos use <name> --sandbox --gpu # sandbox + NVIDIA/DRM passthrough

Security: Linux sandbox mode isolates the filesystem. macOS and Windows only
redirect HOME/USERPROFILE; use an OS sandbox or VM for untrusted code.
  xlings subos list --agent               # list all
  xlings subos remove <name>              # delete

Each SubOS has its own tool version mapping. Tool binaries are shared
globally. Creating a SubOS is instant. "default" cannot be removed.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
FLAGS REFERENCE
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  --yes / -y     Skip prompts. REQUIRED for non-interactive use.
  --agent        Clean plain-text output. RECOMMENDED always.
  -g / --global  Install to global scope.
  -u / --use     Activate installed version immediately.
  --all / -a     Show all subos (for list/use).

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
CONFIG & SELF-MANAGEMENT
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  xlings config --agent                Show config
  xlings config --lang <en|zh>         Set language
  xlings config --mirror <GLOBAL|CN>   Set mirror
  xlings self update                   Update xlings itself
  xlings self doctor                   Check workspace health
  xlings self clean                    Clean cache

[END OF INSTRUCTION])SKILL";
};

}  // namespace xlings::agent::skills
