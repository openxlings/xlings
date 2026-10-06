export module xlings.subos.boot;

import std;
import xlings.libs.json;

// Which SubOS a machine boots (design part 2 §8): `<home>/boot.json`.
//
// The maintainer's model: the boot entries are configured at run time and
// take effect on the next boot. A package change inside the running host is
// live (a new generation); changing WHICH SubOS is the host happens when
// stage-0 runs. Pure: the file's content in, the choice and the content to
// write back out. Stage-0 and `subos boot` share it, and the tests drive it
// without a machine.
export namespace xlings::subos::boot {

namespace fs = std::filesystem;

// A boot that is not marked good this many times in a row stops being chosen.
inline constexpr int kTries = 3;

struct Config {
    std::string default_entry { "default" };
    std::string fallback { "default" };
    std::optional<std::string> once;          // the next boot only (a trial)
    std::map<std::string, int> tries;         // boots left before falling back
    // The last boot, as stage-0 recorded it.
    std::string booted;
    std::string via;                          // "once" | "default" | "fallback"
    bool good { false };
};

Config from_json(const nlohmann::json& j);
nlohmann::json to_json(const Config& c);

// Absent is the default configuration; unreadable is an error (a machine is
// never booted from a guess about a file it could not read).
std::expected<Config, std::string> load(const fs::path& file);
// Written beside the file and renamed over it.
std::expected<void, std::string> save(const fs::path& file, const Config& c);

struct Candidate {
    std::string subos;
    std::string via;
};

// In order: the trial, the default (while it has tries left), the fallback.
// `bootable` answers whether a SubOS can be booted at all (it exists and has
// a root); one that cannot is skipped, never chosen.
std::vector<Candidate> candidates(const Config& c,
                                  const std::function<bool(std::string_view)>& bootable);

// The configuration to write before exec'ing `chosen`'s init: a trial is
// consumed, the chosen entry spends a try, the boot is recorded as not yet good.
Config record_boot(Config c, const Candidate& chosen);

// The running boot worked: its tries are restored.
Config mark_good(Config c);

}  // namespace xlings::subos::boot
