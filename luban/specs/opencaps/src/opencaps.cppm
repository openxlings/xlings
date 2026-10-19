// opencaps: what the parts of a system provide and need, by name and level
// (SPEC.md).
//
// Parts released apart -- a command, a window, a machine's agent, a system, a
// component -- say what they rely on as a capability and a least level, and
// what they have as a capability at a level. A need is met by the same name
// at that level or higher; a name the checking side does not know is not met,
// and that is said: the other side is newer.
export module opencaps;

import std;

export namespace opencaps {

inline constexpr std::string_view kVersion = "0.1";

struct Need {
    std::string name;
    int level { 1 };
    bool operator==(const Need&) const = default;
};

// What a part has: each name at the highest level it provides.
using Provides = std::map<std::string, int, std::less<>>;

// "root-layout>=2", "agent-display" (level 1). Malformed: why.
std::expected<Need, std::string> parse_need(std::string_view text);
// "root-layout=2", "host-gpu" (level 1), into `into`. Malformed: why.
std::expected<void, std::string> parse_provision(std::string_view text, Provides& into);
std::string format(const Need& n);

// One registered level of a capability (SPEC.md §5).
struct Level {
    std::string_view name;
    int level;
    std::string_view adds;
    std::string_view provider;
    // The package that brings it to a system that lacks it (a system's
    // capabilities): what to install instead of upgrading the whole system.
    // Empty when it is not a package's to bring.
    std::string_view package {};
};
std::span<const Level> registry();
// The highest registered level of `name`; 0 when the name is not registered.
int known_level(std::string_view name);

// The answer for one need.
struct Verdict {
    Need need;
    int have { 0 };          // the level provided (0: none)
    bool known { true };     // the name is registered here
    bool met() const { return known && have >= need.level; }
    // Why not met, in a reader's words (empty when met).
    std::string why() const;
};
std::vector<Verdict> check(std::span<const Need> needs, const Provides& provides);
// The unmet ones only.
std::vector<Verdict> unmet(std::span<const Need> needs, const Provides& provides);

}  // namespace opencaps
