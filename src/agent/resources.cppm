// xlings.agent.resources — compile-time embedded skill content.
//
// Each SkillEntry bundles a name, one-line description, and the full
// prompt text that `xlings agent skills <name>` prints to stdout.
// Content is authored as C++ raw-string literals so it ships inside
// the binary — no external files required at runtime.
//
// Maintenance: when a skill's source markdown changes, update the
// corresponding literal here.  A future sync script
// (tools/sync-agent-resources.sh) can automate this.

export module xlings.agent.resources;

import std;

namespace xlings::agent::resources {

export struct SkillEntry {
    std::string_view name;
    std::string_view description;
    std::string_view content;
};

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

// ─── Skills ────────────────────────────────────────────────────

constexpr std::string_view kSkillUsage = R"SKILL(# xlings Usage Skill

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

constexpr std::string_view kSkillSetup = R"SKILL(# Setup Skill — Set up a development environment

Follow this workflow to set up a development environment from scratch.

## Steps

1. Check what tools are already installed:
     xlings list

2. Search for needed tools:
     xlings search <keyword>

3. Install tools with specific versions:
     xlings install <pkg>@<ver> --yes
   Install multiple at once:
     xlings install gcc@15 cmake node --yes

4. Verify installation:
     <tool> --version
   If "command not found", restart shell or run: source ~/.bashrc

5. (Optional) Create project config for reproducibility:
   Create .xlings.json in project root:
     {"workspace": {"gcc": "15.1.0", "cmake": "3.31.6"}}
   Others can then run `xlings install` to get the same environment.

## Common tool names

  gcc, clang, node, python, cmake, rust, go, java, zig, deno, bun
  Use `xlings search <keyword>` to find more.

## Example: C++ project setup

  xlings search gcc
  xlings install gcc@15 cmake --yes
  gcc --version
  cmake --version)SKILL";

constexpr std::string_view kSkillDebugBuild = R"SKILL(# Debug Build Skill — Fix build failures from missing or wrong tools

## Diagnosis

1. Read the error message. Common patterns:
   - "command not found: <tool>"  →  tool not installed
   - "version X required"        →  wrong version active
   - "unsupported option"        →  tool too old

2. Check current state:
     xlings list
     <tool> --version

3. Identify the fix:
   - Not installed     → xlings install <tool> --yes
   - Wrong version     → xlings info <tool>   (see available versions)
                        → xlings use <tool> <correct-version>
   - Too old           → xlings install <tool>@<newer> --yes -u

4. Retry the build.

## Example: gcc too old for C++23

  Error: '__cpp_lib_format' was not declared
  Diagnosis: gcc --version → 13.3.0 (incomplete C++23 support)
  Fix: xlings install gcc@15 --yes -u
  Verify: gcc --version → 15.1.0
  Retry: make)SKILL";

constexpr std::string_view kSkillSubos = R"SKILL(# SubOS Skill — Isolated environment management

SubOS provides isolated tool environments, similar to Python venvs
but for the entire development toolchain.

## When to use SubOS

- Multiple projects need conflicting tool versions
- You want to experiment without affecting your global environment
- Agent needs a sandboxed workspace

## Lifecycle

### Create
  xlings subos new <name>
  xlings subos new <name> --storage tmpfs     # ephemeral (deleted on reboot)

### Enter
  xlings subos use <name>                     # interactive shell
  xlings subos use <name> --cmd "<command>"   # single command
  xlings subos use <name> --sandbox           # with filesystem isolation

### List and inspect
  xlings subos list
  xlings subos info <name>

### Remove
  xlings subos remove <name>

## How it works

- Each SubOS has its own workspace: which tools and which versions are active
- Tool binaries (xpkgs) are shared globally — only the version mapping differs
- Creating a SubOS is instant (no copying of tool binaries)
- "default" SubOS always exists and cannot be removed

## Workflow example

  xlings subos new project-a           # create isolated env
  xlings subos use project-a           # switch to it
  xlings install gcc@14 --yes          # install gcc 14 in project-a
  xlings subos use default             # switch back — gcc 14 not active here
  xlings install gcc@15 --yes          # install gcc 15 in default)SKILL";

// ─── Skill registry ────────────────────────────────────────────

export constexpr SkillEntry kSkills[] = {
    {"usage",       "Complete usage guide: install, search, remove, version switching, SubOS, project mode",
                    kSkillUsage},
    {"setup",       "Set up a development environment from scratch",
                    kSkillSetup},
    {"debug-build", "Diagnose build failures caused by missing or wrong tool versions",
                    kSkillDebugBuild},
    {"subos",       "Create and manage isolated SubOS environments",
                    kSkillSubos},
};

export constexpr std::size_t kSkillCount = std::size(kSkills);

}  // namespace xlings::agent::resources
