export module xlings.store;

import std;

// The payload store's roots (SubOS design part 3 §7.1).
//
// A payload directory is <store>/<package>/<version>, under a directory named
// `xpkgs`. A workspace that has it installed holds it; so does every retained
// root generation that links into it (part 2 §6.3: "a payload an older
// generation references is not collected before that generation is"). The
// second kind is this module's: which generations hold a payload, and the
// ledger of payloads removed from every workspace that a generation still
// holds -- deleted when the last of those generations is pruned.
export namespace xlings::store {

namespace fs = std::filesystem;

// The payload `p` lives in: <...>/xpkgs/<package>/<version>, or nullopt for a
// path outside a store (a sysroot link, the client itself).
std::optional<fs::path> payload_root(const fs::path& p);

// One retained generation's links, as its owner read them.
struct RootSet {
    std::string subos;
    int generation { 0 };
    std::vector<fs::path> targets;   // absolute link targets
};

// A generation that keeps a payload alive.
struct Holder {
    std::string subos;
    int generation { 0 };
    bool operator==(const Holder&) const = default;
};

// What holds a payload. `unreadable` names roots that could not be read: each
// is a holder for this decision (it might link into the payload), never an
// absence.
struct Pins {
    std::vector<Holder> holders;
    std::vector<std::string> unreadable;
    [[nodiscard]] bool held() const { return !holders.empty() || !unreadable.empty(); }
};

// Which of `roots` link into `payload` (a payload root, compared lexically).
Pins pins(const fs::path& payload, std::span<const RootSet> roots,
          std::span<const std::string> unreadable);

// ── The retained ledger: <data>/retained.json ───────────────────────

struct Retained {
    fs::path payload;        // the payload root kept on disk
    std::string target;      // the xvm target it was registered under
    std::string version;
    std::string since;       // UTC, ISO 8601: when the last workspace let go
    bool operator==(const Retained&) const = default;
};

fs::path ledger_path(const fs::path& data_dir);

// Absent is empty (no payload was ever retained). A present file that cannot
// be parsed is an error: what it would have listed is not known, so nothing
// it might name may be released.
std::expected<std::vector<Retained>, std::string> read_ledger(const fs::path& ledger);

// Adds `entry` (replacing one for the same payload); atomic.
std::expected<void, std::string> retain(const fs::path& ledger, Retained entry);

// Drops the entry for `payload`, if any; atomic. The payload is not touched.
std::expected<void, std::string> forget(const fs::path& ledger, const fs::path& payload);

}  // namespace xlings::store
