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

// ── Links: what a view and a root name ───────────────────────────

// A link: where it is, and the absolute path it names (a relative target is
// read against the link's directory). A link is a symbolic link, or on
// Windows a symbolic link or a junction (platform::dir_link_target).
struct Link {
    fs::path at;
    fs::path target;
};

struct LinkWalk {
    std::vector<Link> links;
    // Each directory under the walk that could not be read. The walk goes on
    // past it; a reader that must see everything treats any entry here as
    // "could not read", never as "holds no link".
    std::vector<std::string> unreadable;
};

// Every link under `dir`. A linked directory is one link, never descended
// into, and the walk stops at depth 10 (a payload that links a directory back
// to an ancestor). `dir` absent is empty; `dir` itself a link is that one link.
LinkWalk walk_links(const fs::path& dir);

// The store key of a payload root, `<package>/<version>`.
std::string payload_key(const fs::path& payloadRoot);

// ── Roots: <data>/roots/<owner>/<store-name>@<version> ─────────────
//
// A program other than xlings that uses payloads directly (mcpp wires the
// payload paths it resolved into its builds) states that it holds one by a
// root: a link named `<store-name>@<version>` in a directory named for the
// owner, naming the payload directory `<data>/xpkgs/<store-name>/<version>`.
// Its modification time is the owner's last use; the owner sets it, and the
// owner's policy decides when a root goes. Creating, touching and deleting a
// root needs no lock and no xlings process. The store's collection holds every
// payload a root names (docs/specs/store-roots.md).

fs::path roots_dir(const fs::path& data_dir);

struct Root {
    std::string owner;
    fs::path at;         // the link
    fs::path payload;    // the payload root it names
};

struct RootScan {
    std::vector<Root> live;
    std::vector<fs::path> dangling;  // a root whose payload does not exist
    std::vector<fs::path> ignored;   // an entry that is not a link, or names no payload
};

// Every root under `<data>/roots`. Absent is none. A directory or a link that
// cannot be read is an error: what it would have named is not known.
std::expected<RootScan, std::string> read_roots(const fs::path& data_dir);

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
