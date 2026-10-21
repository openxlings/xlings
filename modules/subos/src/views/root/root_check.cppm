export module xlings.subos.root_check;

import std;
import xlings.libs.json;

// A Luban system's self-check (design 2026-10-16 L §11): what a system needs
// to be usable, asked of its root as its own processes see it. One engine for
// every moment that asks -- made, upgraded, booted, entered, and on demand
// (`luban status --check`) -- and for CI, which runs the same check.
//
// Fast is a handful of reads (booting and entering ask it every time). Full
// adds the runtime closure of /usr/bin and, inside the system, the programs:
// the checks editions and packages declare, and every package's programs
// starting at all.
export namespace xlings::subos::root_check {

namespace fs = std::filesystem;

enum class Depth { Fast, Full };

struct Item {
    std::string id;
    std::string what;           // what holds when it passes, for a person
    bool critical { false };    // a failure makes the system not usable
    bool ok { true };
    std::string detail;         // why not, or what was seen
};

struct Report {
    std::vector<Item> items;
    // No critical item failed.
    bool ok() const;
    // Items that did not pass, critical or not.
    std::size_t failed() const;
};

struct Options {
    // $TERM of who will use it: its terminfo entry is checked.
    std::string term;
    // Run programs (declared checks, packages starting). Only meaningful for
    // the root this process runs in ("/").
    bool run { true };
    std::chrono::seconds program_timeout { 10 };
    // A desktop session runs the gui and sound checks (not this check).
    bool session { false };
};

// Where editions and packages declare their checks: files of
// {"checks": [{"id", "what", "run", "critical", "timeout"}]}, `run` a
// /bin/sh command. A payload's share/luban/checks.d reaches here through the
// share projection (a recipe's extensions.luban.checks are written there).
// Besides `run` (design 2026-10-21 §6.1): `launcher` -- an application id the
// launcher lists (its desktop entry is there, shown); `gui` and `sound` -- a
// window that maps, a sound that is heard -- need a desktop session and are
// run where there is one (luban-os-ci L3), said as such here.
inline constexpr std::string_view kDeclaredDir = "usr/share/luban/checks.d";

Report check(const fs::path& root, Depth depth, const Options& options = {});

// Whether a program asked for its version started at all (its loader, its
// libraries), from its exit and what it said -- not whether it answered 0.
bool started(int code, bool timed_out, std::string_view output);

nlohmann::json to_json(const Report& report);

}  // namespace xlings::subos::root_check
