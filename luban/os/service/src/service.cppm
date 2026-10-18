// Luban's service layer (Luban OS architecture design §5.1): environments as
// operations. Every operation reports through the context's sink and returns an
// Outcome; it never prints and never asks -- a question is the caller's (the
// luban command asks at a terminal, the Luban OS app in a dialog, an agent
// passes -y), and an operation that needs an answer it was not given returns
// code 2 with the command that gives it.
export module luban.service;

export import luban.service.events;
export import luban.service.port;
export import luban.service.settings;
export import luban.service.capabilities;
import std;
import xlings.libs.json;
import opencaps;

export namespace luban::service {

// Luban is released with the xlings it drives: the same date version
// (YYYY.M.D.N), bumped with mcpp.toml and src/core/config.cppm.
// tests/unit/test_luban_cli.cpp holds them equal.
inline constexpr std::string_view kVersion = "2026.10.18.1";

namespace fs = std::filesystem;

struct Context {
    fs::path home;
    Port port;
    Sink sink;
    bool yes { false };       // the user already confirmed what this asks
};


// What the index says of an edition: whether it is there, its status, the
// architectures it is built for, its description and latest version.
struct EditionAvailability {
    bool listed { false };
    std::string status;
    std::vector<std::string> archs;
    std::string description;
    std::string latest;
};
// An error only when the index could not be asked; not listed is a value.
std::expected<EditionAvailability, std::string> edition_availability(const Context& c, std::string_view edition);

struct Environment {
    std::string name;
    std::string kind;              // root | view
    std::string edition;           // ns:name@version it was made from, or empty
    std::size_t packages { 0 };
    std::optional<int> generation;
    bool running { false };
    std::string shell;             // a root's login shell
    Settings settings;
    std::string carrier { "local" };
    // The desktop sessions its root declares (share/wayland-sessions/<id>.desktop),
    // read where the root's links lead -- or from the luban of its carrier's machine.
    std::vector<std::string> desktops;
    // Its root's layout (xlings.subos.rootfs::layout_of; 0: not a root here),
    // and what its manifest's `luban.requires` names (opencaps needs).
    int layout { 0 };
    std::vector<std::string> needs;
    // Old names its sessions still answer to (X-Luban-Aliases; design
    // 2026-10-18 §15.3): labwc-desktop -> labwc.
    std::map<std::string, std::string> aliases;
    // Where it comes from, as its record says (design §4.4): the layers below
    // its edition and the index of each; its manifest as its template wrote it.
    std::vector<std::string> chain;
    std::string edition_index;
    std::vector<std::string> chain_index;
    nlohmann::json manifest = nlohmann::json::object();
};

// What this luban provides (design 2026-10-18 §9; luban/specs/opencaps): the
// root layout it reads, the manifest fields it knows.
opencaps::Provides luban_provides();
// What `e` needs of whoever reads or runs it -- its root's layout, its
// manifest's requires -- against `reader`: the unmet ones, each with why.
std::vector<opencaps::Verdict> unmet_needs(const Environment& e, const opencaps::Provides& reader);

// What going into a system shows first (design 2026-10-18 §8): its desktop or
// its console, and when a desktop was wanted and cannot be, why -- said, never
// silent (E20). The Luban OS app, `luban enter` and `luban status` read it.
struct EnterPlan {
    std::string target { "console" };   // console | desktop
    std::string session;                // the desktop's session, when target is desktop
    std::string why;                    // a desktop wanted and not there; empty otherwise
};
EnterPlan enter_plan(const Environment& e);

// What a console says first, before its prompt (part 3 §7.3): two columns,
// no frame (a wide character misaligns a right border), at most 72 columns,
// ASCII when the locale is not UTF-8, colour only where it is known to work
// and never needed to read it. The same lines in a terminal, a machine's
// console and the Luban OS window.
struct WelcomeStyle {
    bool zh { false };
    bool color { false };
    bool utf8 { true };
};
// This terminal's: NO_COLOR / LUBAN_COLOR=0, TERM, the locale.
WelcomeStyle terminal_style();
// What only the caller knows: the kernel ("Linux 6.8.0"), a machine's size
// ("KVM · 2 CPU · 2 GiB"), who logs in, where its network goes.
struct WelcomeFacts {
    std::string kernel;
    std::string machine;
    std::string user { "root" };
    std::string user_home { "/root" };
    std::string network;
};
// `mode` is how this session runs (sandbox or machine).
std::vector<std::string> welcome(const Environment& e, std::string_view mode, bool temp,
                                 const WelcomeStyle& style, const WelcomeFacts& facts = {});
// The prompt's mark of a system (part 3 §7.2): [luban] shares this host's
// kernel, <luban> has its own; :temp / :home when it keeps less than all.
std::string kind_mark(std::string_view mode, std::string_view persist);
// The user's language (LC_ALL, LC_MESSAGES, LANG) is Chinese.
bool chinese_locale();

std::expected<std::vector<Environment>, std::string> list(const Context& c);
std::expected<Environment, std::string> describe(const Context& c, std::string_view name);

}  // namespace luban::service

// What a Luban system is made of and where it comes from (design 2026-10-18
// §4, §5): everything is an xpkg. A coordinate is a system (`type = "subos"`),
// a desktop (`categories` lists luban-desktop; its session file is the
// convention), or a package; a system's lineage is its `from` chain, each
// layer read from the index it resolves in; the official short names are the
// names in the `luban:` namespace of the official index. What a third party's
// system or component does beyond Luban is shown once and its answer kept.
export namespace luban::service {



// The index Luban's official packages come from: xlings's default one.
inline constexpr std::string_view kOfficialIndex = "xim";

// A coordinate as the index describes it (xlings interface package_info).
struct Coordinate {
    std::string given;                  // as typed
    std::string package;                // ns:name
    std::string ns, name, version;
    std::string index;                  // the repository it resolves in
    std::string type;                   // subos | package | script | ...
    std::string description;
    std::vector<std::string> categories;
    std::string passed_over;            // why `version` is not the newest
    // What it is to Luban (design §4.1): system | desktop | package.
    std::string role() const;
    bool official() const { return index == kOfficialIndex; }
    std::string ref() const { return version.empty() ? package : package + "@" + version; }
};
// Not in any index: the index's answer, as an error.
std::expected<Coordinate, std::string> describe_coordinate(const Port& port, std::string_view coord);

// An official system's short name (`desktop`): luban:<name> when the index
// has it, else the published subos:luban-<name> (frozen, still resolved);
// nullopt when neither is a system.
std::optional<std::string> official_system(const Port& port, std::string_view short_name);
// The official desktop of that name (luban:<name>, categories luban-desktop).
std::optional<std::string> official_desktop(const Port& port, std::string_view name);
// Every official system's short name and the ref it resolves to, in one search.
std::map<std::string, std::string> official_system_refs(const Port& port);
// The official systems the index offers (their short names), for a message.
std::vector<std::string> official_systems(const Port& port);

// What a quick start offers, read from the indexes (design 2026-10-18 §6.4):
// the systems (category distribution) and the desktops (luban-desktop), the
// official first -- an official system by its short name -- and every other
// with the index it comes from.
struct Offer {
    std::string ref;            // ns:name
    std::string name;           // the short name of an official one
    std::string description;
    std::string index;
    bool official { false };
};
struct QuickStartOffers {
    std::vector<Offer> systems, desktops;
};
QuickStartOffers quick_start_offers(const Port& port);

// One layer of a system's lineage: a template and its manifest as written.
struct Layer {
    std::string ref;                    // ns:name@version
    std::string index;
    nlohmann::json manifest = nlohmann::json::object();
    // An official Luban edition: the official index, the luban: namespace
    // (or a published subos:luban-* one).
    bool official() const;
    // How it calls itself: the manifest's luban.os.name, else its package name.
    std::string name() const;
    std::string version() const;
};

// A system's lineage (design §4.4): its layers, top first, as
// `xlings subos new --dry-run` read them.
struct Lineage {
    std::vector<Layer> layers;
    std::vector<std::string> packages;  // merged, as declared
    std::string policy;
    // The Luban OS base: the first official edition from the top.
    const Layer* base() const;
    // The layer it is made from directly (the second), if any.
    const Layer* parent() const;
    // Its manifest's luban object merged down the chain, the upper layer winning per key.
    nlohmann::json luban(std::string_view key) const;
    // What every layer's luban.requires names.
    std::vector<std::string> needs() const;
};
std::expected<Lineage, std::string> plan_system(const Port& port, std::string_view name, std::string_view ref);

// A made system's lineage, from its record (no index asked): its own manifest
// for its top layer; a record from before the indexes were kept takes a
// published Luban name as official.
Lineage lineage_of(const Environment& e);
// "myos 2026.11.1 (Luban desktop)"; an official edition is "Luban desktop 2026.10.19.1".
std::string display_name(const Lineage& l);
// The os-release of a system that names itself (luban.os), written over the
// edition's: NAME, ID, ID_LIKE=luban, VERSION_ID, LUBAN_BASE. Empty for an
// official edition (its own stays).
std::string os_release(const Lineage& l);

// What a third party's system or components do beyond Luban, as a
// confirmation shows it (design §5.4): one line each, the layer or
// coordinate that brings it named.
struct Beyond {
    std::string what;                   // "policy", "settings", "admin", ...
    std::string text;
    std::string from;                   // the layer or coordinate
    bool attention { false };           // shown first, marked
};
std::vector<Beyond> beyond_luban(const Lineage& l);
// The identity of what is confirmed: the refs of the third-party layers and
// coordinates, and what they do beyond Luban. A change asks again.
std::string confirmation_key(const Lineage& l, std::span<const Coordinate> extra);

// <home>/config/luban/confirmed.json. Absent: nothing confirmed; unreadable:
// an error naming the file (never "not confirmed").
std::expected<bool, std::string> is_confirmed(const fs::path& home, std::string_view key);
std::expected<void, std::string> record_confirmed(const fs::path& home, std::string_view key, std::string_view label);


struct NewOptions {
    std::string name;
    // The system (design 2026-10-18 §5.1): an official short name (`edition`:
    // a name in the luban: namespace), or a system coordinate among `xpkgs`
    // (any index); neither is core. `xpkgs` also takes desktops and packages,
    // installed into it once it is made.
    std::string edition;
    std::vector<std::string> xpkgs;
    // What `luban config` takes, applied once it is made (desktop, mode,
    // persist, shell, login, on-close, policy, tz, allow, ...).
    std::vector<std::pair<std::string, std::string>> settings;
    std::string proxy;
    std::string carrier;
    // Its login user (part 3 §6.1), made after it is: a name, and the
    // password's crypt hash ("" = logs in without one). Empty: root.
    std::string user;
    std::string password_hash;
    // A third party's part was shown and agreed to (the caller asked).
    bool agreed { false };
};

// What `luban new` would make, before anything is (design 2026-10-18 §5.3):
// the system's lineage read from its templates, what was given besides it,
// and whether a third party's part is to be confirmed.
struct NewPreview {
    std::string system;                     // the ref xlings makes it from
    Lineage lineage;
    std::vector<Coordinate> extras;         // desktops and packages given with --xpkg
    std::vector<Beyond> beyond;             // what its third-party layers do beyond Luban
    std::string confirm_key;                // empty: everything is official
    bool confirmed { false };               // confirmed before (config/luban/confirmed.json)
    bool needs_confirmation() const { return !confirm_key.empty() && !confirmed; }
    // What it is as data: distribution, luban_base, parent, lineage[], extras[].
    nlohmann::json value() const;
};
// Checks, in order: what each coordinate is; one system; the chain reaches an
// official edition; what its manifests need of this luban. A failure is said
// through the sink with its next step, and is the error's exit code.
std::expected<NewPreview, int> preview_new(const Context& c, const NewOptions& o);
// A system that cannot be used as root: a desktop (its browsers refuse it).
bool needs_user(const NewPreview& p);
inline constexpr std::string_view kDefaultUser = "lubanoser";
// Makes it. A third party's part not confirmed before is made only with
// Context::yes (the caller asked, or was told to); otherwise code 2, with how.
Outcome create(const Context& c, const NewOptions& o, const NewPreview* preview = nullptr);

// Changes, in the order given: machine settings (mode, persist, desktop,
// on-close), the login shell, and xlings's isolation settings (policy, proxy,
// tz, allow, disallow, mount, unmount, fetch, observe). Nothing: the current
// settings as data.
Outcome configure(const Context& c, std::string_view name,
                  const std::vector<std::pair<std::string, std::string>>& changes);

Outcome upgrade(const Context& c, std::string_view name, std::string_view to, bool dry_run);
Outcome rollback(const Context& c, std::string_view name, std::string_view to);
Outcome history(const Context& c, std::string_view name);
Outcome remove(const Context& c, std::string_view name);
Outcome stop(const Context& c, std::string_view name);
Outcome boot(const Context& c, std::span<const std::string> args);

// What `export` writes, from the file name: iso, img, qcow2, tar, dir, luban;
// empty when the name says nothing it knows.
std::string export_format(std::string_view file);
struct ExportOptions {
    std::string name;
    fs::path file;
    std::string format;            // empty: from the file name
    std::string size;
    std::string boot;
};
Outcome export_environment(const Context& c, const ExportOptions& o);
Outcome import_environment(const Context& c, const fs::path& file, std::string name);

// On this terminal: the environment's shell (`temp`: nothing it writes stays),
// or one command.
int enter_sandbox(const Context& c, std::string_view name, bool temp, std::string_view user = {});
int run_command(const Context& c, std::string_view name, std::span<const std::string> argv, std::string_view user = {});

}  // namespace luban::service
