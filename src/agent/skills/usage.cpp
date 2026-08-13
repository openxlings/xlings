// Skill: usage — Complete xlings usage guide for LLM agents.
// Written as a system-prompt-style instruction set, not documentation.

module xlings.agent.skills.usage;

import std;
import xlings.agent.skill;
import xlings.cli.spec;


// ── out-of-line class members ─────────────────────────────────

namespace xlings::agent::skills {

auto UsageSkill::name() const -> std::string_view{ return "usage"; }

auto UsageSkill::description() const -> std::string_view{
        return "IMPORTANT: Read this FIRST. Complete usage instructions for AI agents.";
    }

auto UsageSkill::content() const -> std::string_view{
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

} // namespace xlings::agent::skills
