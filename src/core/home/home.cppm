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

// For read-only commands only: the commands an unknown-layout home allows.
bool is_read_only_command(std::span<const std::string_view> argv);

}  // namespace xlings::home
