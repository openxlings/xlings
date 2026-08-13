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

export module xlings.agent.skill;

import std;

namespace xlings::agent {

// ─── Skill interface ───────────────────────────────────────────

export struct Skill {
    virtual ~Skill() = default;
    virtual auto name() const -> std::string_view = 0;
    virtual auto description() const -> std::string_view = 0;
    virtual auto content() const -> std::string_view = 0;
};

// ─── Registry ──────────────────────────────────────────────────

export class SkillRegistry {
public:
    void add(std::unique_ptr<Skill> skill);

    auto list() const -> const std::vector<std::unique_ptr<Skill>>&;

    auto find(std::string_view name) const -> const Skill*;

    auto count() const -> std::size_t;

private:
    std::vector<std::unique_ptr<Skill>> skills_;
};

}  // namespace xlings::agent
