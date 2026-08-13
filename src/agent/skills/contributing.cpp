// Skill: contributing — xlings project development workflow for AI agents.
// Written as system-prompt-style instructions to enforce correct dev flow.

module xlings.agent.skills.contributing;

import std;
import xlings.agent.skill;


// ── out-of-line class members ──────────────────────────────────

namespace xlings::agent::skills {

auto ContributingSkill::name() const -> std::string_view { return "contributing"; }

auto ContributingSkill::description() const -> std::string_view {
    return "REQUIRED before any code change. Development workflow, PR rules, release process.";
}

auto ContributingSkill::content() const -> std::string_view { return kContent; }

} // namespace xlings::agent::skills
