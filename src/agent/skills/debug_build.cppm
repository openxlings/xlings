// Skill: debug-build — Diagnose build failures from missing or wrong tools.

export module xlings.agent.skills.debug_build;

import std;
import xlings.agent.skill;

namespace xlings::agent::skills {

export class DebugBuildSkill : public Skill {
public:
    auto name() const -> std::string_view override { return "debug-build"; }
    auto description() const -> std::string_view override {
        return "Diagnose build failures caused by missing or wrong tool versions";
    }
    auto content() const -> std::string_view override { return kContent; }

private:
    static constexpr std::string_view kContent = R"SKILL(# Debug Build Skill — Fix build failures from missing or wrong tools

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
};

}  // namespace xlings::agent::skills
