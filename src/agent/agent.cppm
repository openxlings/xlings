// xlings.agent — `xlings agent` subcommand.
//
// Provides built-in skill content that LLM agents can retrieve at
// runtime to learn how to use xlings.  The skill texts are compiled
// into the binary (see agent/resources.cppm) so they are available
// regardless of working directory or repo checkout.
//
// Command tree:
//   xlings agent              → overview + skill list
//   xlings agent skills       → skill list (same as above)
//   xlings agent skills <n>   → print full content of skill <n>

export module xlings.agent;

import std;

import xlings.agent.resources;

namespace xlings::agent {

using resources::kOverview;
using resources::kSkills;
using resources::kSkillCount;

// Print the overview text followed by the skill index table.
export void print_overview() {
    std::cout << kOverview << "\n\n";
    std::cout << "Available skills (run `xlings agent skills <name>`):\n\n";
    for (std::size_t i = 0; i < kSkillCount; ++i) {
        auto& s = kSkills[i];
        // Pad name to 14 chars for alignment
        std::string name{s.name};
        if (name.size() < 14) name.resize(14, ' ');
        std::cout << "  " << name << s.description << "\n";
    }
    std::cout << "\n";
}

// Print the full content of a named skill.
// Returns true if found, false if not (with an error message to stderr).
export bool print_skill(std::string_view name) {
    for (std::size_t i = 0; i < kSkillCount; ++i) {
        if (kSkills[i].name == name) {
            std::cout << kSkills[i].content << "\n";
            return true;
        }
    }
    std::cerr << "Unknown skill: " << name << "\n";
    std::cerr << "Available skills: ";
    for (std::size_t i = 0; i < kSkillCount; ++i) {
        if (i > 0) std::cerr << ", ";
        std::cerr << kSkills[i].name;
    }
    std::cerr << "\n";
    std::cerr << "Run `xlings agent skills <name>` to view a skill.\n";
    return false;
}

// Top-level handler for `xlings agent [args...]`.
// Called from cli.cppm's subcommand action.
export int run(int argc, char* argv[]) {
    // argc/argv are the filtered args starting from the first positional
    // after "agent". Example: `xlings agent skills usage` gives:
    //   argv[0] = "xlings", argv[1] = "agent", argv[2] = "skills", argv[3] = "usage"
    // We receive the full argv but offset starts at index 2 (after "agent").

    // No sub-argument: show overview
    if (argc <= 2) {
        print_overview();
        return 0;
    }

    std::string_view sub{argv[2]};

    // Handle -h/--help as plain text (no TUI)
    if (sub == "-h" || sub == "--help") {
        print_overview();
        return 0;
    }

    if (sub == "skills") {
        if (argc <= 3) {
            // `xlings agent skills` — list all skills
            print_overview();
            return 0;
        }
        // `xlings agent skills <name>`
        return print_skill(argv[3]) ? 0 : 1;
    }

    // Unknown sub: check if it's a skill name directly
    // (allow `xlings agent usage` as shorthand for `xlings agent skills usage`)
    for (std::size_t i = 0; i < kSkillCount; ++i) {
        if (kSkills[i].name == sub) {
            std::cout << kSkills[i].content << "\n";
            return 0;
        }
    }

    std::cerr << "Unknown: xlings agent " << sub << "\n";
    std::cerr << "Usage:\n";
    std::cerr << "  xlings agent                 Show overview and skill list\n";
    std::cerr << "  xlings agent skills          List available skills\n";
    std::cerr << "  xlings agent skills <name>   Show a specific skill\n";
    return 1;
}

}  // namespace xlings::agent
