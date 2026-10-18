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

export namespace luban::service {

// Luban is released with the xlings it drives: the same date version
// (YYYY.M.D.N), bumped with mcpp.toml and src/core/config.cppm.
// tests/unit/test_luban_cli.cpp holds them equal.
inline constexpr std::string_view kVersion = "2026.10.17.1";

namespace fs = std::filesystem;

struct Context {
    fs::path home;
    Port port;
    Sink sink;
    bool yes { false };       // the user already confirmed what this asks
};

// `core` -> `subos:luban-core`; a full reference (`ns:name[@v]`) as given.
std::string edition_ref(std::string_view edition);
std::span<const std::string_view> official_editions();

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
};

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

struct NewOptions {
    std::string name;
    std::string edition { "core" };
    std::string proxy;
    std::string carrier;
    // Its login user (part 3 §6.1), made after it is: a name, and the
    // password's crypt hash ("" = logs in without one). Empty: root.
    std::string user;
    std::string password_hash;
};
// An edition that cannot be used as root: a desktop (its browsers refuse it).
bool needs_user(std::string_view edition);
inline constexpr std::string_view kDefaultUser = "lubanoser";
Outcome create(const Context& c, const NewOptions& o);

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
