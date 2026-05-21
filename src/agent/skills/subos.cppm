// Skill: subos — Isolated environment management.

export module xlings.agent.skills.subos;

import std;
import xlings.agent.skill;

namespace xlings::agent::skills {

export class SubosSkill : public Skill {
public:
    auto name() const -> std::string_view override { return "subos"; }
    auto description() const -> std::string_view override {
        return "Create and manage isolated SubOS environments";
    }
    auto content() const -> std::string_view override { return kContent; }

private:
    static constexpr std::string_view kContent = R"SKILL(# SubOS Skill — Isolated environment management

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
};

}  // namespace xlings::agent::skills
