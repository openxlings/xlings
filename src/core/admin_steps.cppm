export module xlings.core.admin_steps;

import std;
import xlings.runtime;

// Steps only an administrator may take, declared by recipes and settled by
// xlings (Luban OS part 3 §8.2). A recipe never runs `sudo` itself: it says
// what and why (libxpkg `system.admin_step`); xlings asks once, after the
// install, through the one door (`subos::elevation`, recorded in
// logs/elevation.ndjson), or -- when nobody can answer -- keeps them in
// state/admin-steps.json and says how to finish, which `self doctor --fix`
// does at a terminal.
export namespace xlings::admin_steps {

struct Step {
    std::string why;
    std::vector<std::string> argv;
    bool operator==(const Step&) const = default;
};

// A deferred request's target (op "admin"): why, then each word, joined by
// the ASCII unit separator. nullopt: not one.
std::optional<Step> parse(std::string_view target);

// What is still waiting, and replacing that list.
std::vector<Step> pending(const std::filesystem::path& home);
void record(const std::filesystem::path& home, const std::vector<Step>& steps);

// Asks once and runs `steps` when somebody can be asked (`yes` answers the
// question, not the password); otherwise adds them to what is waiting. A
// step that fails stays waiting. Returns how many still wait.
std::size_t settle(const std::filesystem::path& home, std::vector<Step> steps, bool yes, EventStream& stream);

// The command line a person runs as root for `s`.
std::string command(const Step& s);

}  // namespace xlings::admin_steps
