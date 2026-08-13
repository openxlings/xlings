// xlings.agent.resources — overview text + skill registry builder.
//
// The overview is the static text printed by `xlings agent`.
// The registry is built by importing each skill module and registering
// it.  To add a new skill, add its module import and a register call
// in build_registry().

module xlings.agent.resources;

import std;
import xlings.agent.skill;
import xlings.agent.skills.usage;
import xlings.agent.skills.contributing;

namespace xlings::agent::resources {

SkillRegistry build_registry() {
    SkillRegistry reg;
    reg.add(std::make_unique<skills::UsageSkill>());
    reg.add(std::make_unique<skills::ContributingSkill>());
    return reg;
}

}
