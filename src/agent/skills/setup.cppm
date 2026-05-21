// Skill: setup — Set up a development environment from scratch.

export module xlings.agent.skills.setup;

import std;
import xlings.agent.skill;

namespace xlings::agent::skills {

export class SetupSkill : public Skill {
public:
    auto name() const -> std::string_view override { return "setup"; }
    auto description() const -> std::string_view override {
        return "Set up a development environment from scratch";
    }
    auto content() const -> std::string_view override { return kContent; }

private:
    static constexpr std::string_view kContent = R"SKILL(# Setup Skill — Set up a development environment

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
};

}  // namespace xlings::agent::skills
