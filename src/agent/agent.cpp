// xlings.agent — `xlings agent` subcommand.
//
// Provides built-in skill content that LLM agents can retrieve at
// runtime to learn how to use xlings.  The skill texts are compiled
// into the binary (see agent/skills/*.cppm) so they are available
// regardless of working directory or repo checkout.
//
// Command tree:
//   xlings agent              → overview + skill list
//   xlings agent skills       → skill list (same as above)
//   xlings agent skills <n>   → print full content of skill <n>
//   xlings agent <n>          → shorthand for skills <n>

module xlings.agent;

import std;
import xlings.agent.skill;
import xlings.agent.resources;

namespace xlings::agent {

void print_overview(const SkillRegistry& reg) {
    std::cout << resources::kOverview << "\n\n";
    std::cout << "Available skills (run `xlings agent skills <name>`):\n\n";
    for (auto& s : reg.list()) {
        std::string name{s->name()};
        if (name.size() < 14) name.resize(14, ' ');
        std::cout << "  " << name << s->description() << "\n";
    }
    std::cout << "\n";
}

bool print_skill(const SkillRegistry& reg, std::string_view name) {
    auto* skill = reg.find(name);
    if (skill) {
        std::cout << skill->content() << "\n";
        return true;
    }
    std::cerr << "Unknown skill: " << name << "\n";
    std::cerr << "Available skills: ";
    bool first = true;
    for (auto& s : reg.list()) {
        if (!first) std::cerr << ", ";
        std::cerr << s->name();
        first = false;
    }
    std::cerr << "\n";
    std::cerr << "Run `xlings agent skills <name>` to view a skill.\n";
    return false;
}

int run(int argc, char* argv[]) {
    auto reg = resources::build_registry();

    // No sub-argument: show overview
    if (argc <= 2) {
        print_overview(reg);
        return 0;
    }

    std::string_view sub{argv[2]};

    // Handle -h/--help as plain text (no TUI)
    if (sub == "-h" || sub == "--help") {
        print_overview(reg);
        return 0;
    }

    if (sub == "skills") {
        // `-h`/`--help` here is a help request, not a skill named "--help".
        // Without this, `xlings agent skills --help` looks up a skill by that
        // name, fails, and exits 1 -- the one shape a documented command path
        // must never have.
        if (argc <= 3 || std::string_view{argv[3]} == "-h"
            || std::string_view{argv[3]} == "--help") {
            print_overview(reg);
            return 0;
        }
        // `xlings agent skills <name>`
        return print_skill(reg, argv[3]) ? 0 : 1;
    }

    // Shorthand: `xlings agent <name>` → `xlings agent skills <name>`
    if (reg.find(sub)) {
        std::cout << reg.find(sub)->content() << "\n";
        return 0;
    }

    std::cerr << "Unknown: xlings agent " << sub << "\n";
    std::cerr << "Usage:\n";
    std::cerr << "  xlings agent                 Show overview and skill list\n";
    std::cerr << "  xlings agent skills          List available skills\n";
    std::cerr << "  xlings agent skills <name>   Show a specific skill\n";
    return 1;
}

}
