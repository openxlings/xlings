export module xlings.core.home;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.home_identity;

// Which home this run uses, and what kind of home it is (design §3-§6).
//
// HomeContext is the one answer to "which home, in which deployment mode, at
// which layout". The home's path is still resolved by Config (anchored shim
// home, then XLINGS_HOME, then a self-contained layout, then ~/.xlings); this
// module answers what Config did not know: the MODE the home was deployed in
// and the LAYOUT version it carries, both DECLARED in `.xlings-home`.
//
//   mode    user      ~/.xlings of one user (the default install)
//           custom    any directory a user chose (XLINGS_HOME)
//           portable  self-contained: the binary's own directory
//           system    installed by a system package (read by many users)
//           multi     a system package manager home with per-user layers
//
// A marker written before modes existed has none; the mode is then inferred
// from where the home is, ONCE, and reported as inferred. Declaring is what
// installers and `self init` do from now on.
//
// layout  1  everything up to 2026.10.4
//         2  adds config/subos/, logs/subos/, run/subos/, state/ -- only
//            ADDS directories, which is why an older client is unaffected
//
// A home whose layout is higher than this client knows is read and never
// written (`writable() == false`): a newer client changed something this one
// cannot see, and writing would undo it without knowing.
export namespace xlings::home {

namespace fs = std::filesystem;

inline constexpr int kLayout = 2;

enum class Mode { User, Custom, Portable, System, Multi };
enum class Source { Anchored, Env, SelfContained, Default };

std::string_view to_string(Mode m);
std::optional<Mode> mode_from_string(std::string_view s);
std::string_view to_string(Source s);

struct HomeContext {
    fs::path home;
    Source source { Source::Default };
    Mode mode { Mode::User };
    bool modeDeclared { false };     // from .xlings-home, not inferred
    int layout { 1 };
    std::string id;                  // the marker's id, when there is one

    bool writable() const { return layout <= kLayout; }
};

// Read `.xlings-home` and complete the context for a home Config resolved.
HomeContext describe(const fs::path& home, Source source);

// Declare the mode and/or raise the layout in `.xlings-home`, keeping every
// key this client does not know. The layout only ever moves up. A home with
// no marker is left alone (home_identity writes the marker).
std::expected<void, std::string> declare(const fs::path& home,
                                         std::optional<Mode> mode,
                                         std::optional<int> layout);

// The mode a home without a declaration is taken to have.
Mode infer_mode(const fs::path& home, Source source);

// ── What a system install provides (design §4, deployment S and M) ──
//
// S: the entry is a system package's (/usr/bin/xlings, root-owned). Each user
//    still has a home of their own; the system adds root-owned components and
//    a configuration file, and updates the entry through its package manager.
// M: S plus a root-owned system LAYER, a home a package manager maintains for
//    every user (/opt/xlings), declared `"mode": "multi"` in its .xlings-home.
//    Read here; resolving packages from it is not implemented yet.

struct Entry {
    fs::path path;               // the binary this process is
    bool system { false };       // root-owned, outside the home, not writable here
};

// The running binary as a deployment: is it the home's own, or a system
// package's that `self update` must not try to replace?
Entry describe_entry(const fs::path& exe, const fs::path& home);

// /etc/xlings/config.json (%ProgramData%\xlings\config.json): defaults a
// home's own .xlings.json overrides. XLINGS_SYSTEM_CONFIG names another file.
fs::path system_config_path();
// The file as an object; missing or unreadable is empty, never an error.
nlohmann::json read_system_config();

// The system layer when one is installed: /opt/xlings (%ProgramData%\xlings\home)
// or XLINGS_SYSTEM_LAYER, and only if its marker declares mode "multi".
std::optional<fs::path> system_layer();

// For read-only commands only: the commands an unknown-layout home allows.
bool is_read_only_command(std::span<const std::string_view> argv);

// The writer rule (design §25 "schema"): read a JSON document in order to
// update it. A missing file is a new, empty document. A file that exists and
// does not parse as an object is an ERROR -- not an empty document -- and the
// caller must not write: replacing it would turn "could not be read" into
// "is now empty", which no repair can walk back. Writers update the object
// they read, so every key they do not know survives.
std::expected<nlohmann::json, std::string> read_json_for_update(const fs::path& path);

}  // namespace xlings::home
