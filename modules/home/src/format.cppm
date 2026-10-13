// A home's format: its layout version, which clients understand it, and the
// steps that bring an older home to this client's (design §4).
//
// Readers are tolerant, writers move forward. A step brings a home from what
// an older client wrote to what this one writes, once, idempotently, and is
// recorded; nothing dispatches an operation to "the code of the version that
// wrote the home" -- that would keep every old format's code alive forever.
// The reading of an older shape lives in `COMPAT(... drop in YYYY.M)` code for
// at least six months.
export module xlings.home.format;

import std;

export namespace xlings::home::format {

namespace fs = std::filesystem;

// The layout this client writes. The marker's integer `layout`: released
// clients from 2026.10.9.1 on read a home whose layout is above theirs and
// never write it.
inline constexpr int kLayout = 2;

// What each layout brought, for the note a switch to an older client prints.
struct Epoch {
    int layout;
    std::string_view since;     // the first client that writes it
    std::string_view brings;    // what a client that does not know it cannot see
};
std::span<const Epoch> epochs();

// What a client knows of the format.
struct ClientFormat {
    bool gates { false };       // reads the layout and refuses to write a newer one
    int knows { 1 };            // the highest layout it understands
};
ClientFormat client_format(std::string_view version);

// The note for switching this home (at `layout`) to the client `version`:
// nothing when that client understands the home; otherwise what it will not
// see, and whether it can still write the home. `read_only` is set when it
// cannot -- a switch the caller confirms first.
struct SwitchNote {
    std::string text;
    bool read_only { false };
};
std::optional<SwitchNote> switch_note(int layout, std::string_view version);

// Date versions (2026.10.12.3) and the older semantic ones (0.4.40), compared
// part by part as numbers; a missing part is 0.
int compare_versions(std::string_view a, std::string_view b);

// ── Steps ───────────────────────────────────────────────────────────

// One step from what an older client wrote to what this one writes. `needed`
// only reads; `apply` is idempotent and returns what it did, in one line.
struct Step {
    std::string_view name;
    std::function<bool(const fs::path& home)> needed;
    std::function<std::expected<std::string, std::string>(const fs::path& home)> apply;
};

struct Report {
    std::vector<std::pair<std::string, std::string>> done;      // step, what it did
    std::vector<std::pair<std::string, std::string>> failed;    // step, why
};

// Each step is its own failure domain: one that throws or fails is reported
// and the others still run.
Report run(const fs::path& home, std::span<const Step> steps);

// ── Problem classes ─────────────────────────────────────────────────

// What a problem found in a home is, which decides who handles it (§4.4).
enum class Problem {
    Format,         // an older client's shape: a step migrates it
    Derived,        // routes, generations, caches: rebuilt
    Dangling,       // a registration whose payload is gone and cannot come back: dropped
    Reinstallable,  // a payload that is gone and the index still has: one command
    Recipe,         // the detector or a recipe is wrong: not the user's to fix
    Decision,       // the user's data or administrator rights: only the user
};
// Whether a problem of this class is said to the user (the others are handled
// and recorded).
bool told(Problem p);
std::string_view to_string(Problem p);

}  // namespace xlings::home::format
