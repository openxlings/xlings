// xlings.agent.skill — Skill base class and registry.
//
// Every built-in skill implements the Skill interface and registers
// itself with the SkillRegistry.  The registry drives `xlings agent`
// (list) and `xlings agent skills <name>` (print).
//
// Adding a new skill:
//   1. Create src/agent/skills/<name>.cppm
//   2. Implement a class deriving from Skill
//   3. Register it in SkillRegistry::build() below

module xlings.agent.skill;

import std;


// ── out-of-line class members ─────────────────────────────────

namespace xlings::agent {

void SkillRegistry::add(std::unique_ptr<Skill> skill){
        skills_.push_back(std::move(skill));
    }

auto SkillRegistry::list() const -> const std::vector<std::unique_ptr<Skill>>&{
        return skills_;
    }

auto SkillRegistry::find(std::string_view name) const -> const Skill*{
        for (auto& s : skills_) {
            if (s->name() == name) return s.get();
        }
        return nullptr;
    }

auto SkillRegistry::count() const -> std::size_t{
        return skills_.size();
    }

} // namespace xlings::agent
