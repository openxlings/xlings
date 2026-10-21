export module xlings.subos.situation;

import std;

// Where this xlings runs, who runs it, which store it reads -- and so where a
// change it is asked for goes (design 2026-10-21 §3.0). The one answer every
// part asks: the command line (run here, send it to the broker, say it needs
// the administrator), the home's paths (a user's view over the system's
// store), the installer (what it may borrow), what a refusal says.
//
//   place   host       a person's computer: their home is theirs
//           sandbox    a Luban system in a sandbox on this computer
//           machine    a Luban system with its own kernel on this computer
//                      (qemu, a Mac's vz, WSL2's)
//           system     a Luban system booted as itself (hardware, an image)
//   who     owner      the home's owner (a host)
//           admin      the system's administrator (root, sudo; a sandbox's
//                      session acts for the system)
//           user       a user of the system: a view of their own over its store
//   store   personal   the owner's home
//           shared     this computer's store, which the system reads and the
//                      host writes (read-only here)
//           own        the system's own, written here
//
// What a command touches comes from the broker's classification (the one
// table of xlings's commands, xlings.subos.broker); route() is the table
// below and nothing else decides it.
export namespace xlings::subos::situation {

namespace fs = std::filesystem;

enum class Place { Host, Sandbox, Machine, System };
enum class Who { Owner, Admin, User };
enum class Store { Personal, Shared, Own };

// What a command changes.
enum class Touch {
    Read,       // nothing
    Fetch,      // payloads into a store (install, a view's store addition)
    Index,      // the index every user of the store reads (update)
    Instance,   // this system's own state (remove, use)
    Home,       // the store's owner's settings, other systems, the policy
};

enum class Way {
    Here,       // run it here
    Broker,     // the host's broker runs it for this system
    Admin,      // the system's administrator's: say `sudo xlings ...`
    Owner,      // the store's owner's, on the host: say so
    NoBroker,   // the host's, and the host offers no broker: one line, what works
};

struct Situation {
    Place place { Place::Host };
    Who who { Who::Owner };
    Store store { Store::Personal };
    fs::path system_home;     // the store a system reads (its root record's home); empty on a host
    std::string system;       // the system's name, when it says
    bool broker { false };    // a broker answers here
};

// What a Luban system's root says it is (/etc/xlings/usr names its
// generation: <home>/subos/<name>/root/usr): the store it reads and its name.
struct RootRecord {
    fs::path home;
    std::string name;
};
std::optional<RootRecord> root_record(const fs::path& etc = "/etc");
// The record's line, read: nullopt for anything else.
std::optional<RootRecord> parse_root_record(std::string_view line);

// A Luban machine's guest: its kernel was started with a luban.home.
bool in_machine();

// The facts measure() reads, for a test to give.
struct Facts {
    std::string mode;          // XLINGS_SUBOS_MODE
    bool root_instance { false };   // XLINGS_ROOT_INSTANCE set
    std::optional<RootRecord> record;
    bool machine { false };
    bool root { false };       // uid 0
    bool writable { false };   // this process can write the store's home
    bool read_only { false };  // the store's home is on a read-only mount (whoever asks)
    bool broker { false };
};
Situation from(const Facts& f, const fs::path& home);

// This process's, for the store at `home` (the system's, before a user's view
// replaces it): measured each call.
Situation measure(const fs::path& home);

Way route(const Situation& s, Touch t);

std::string_view to_string(Place p);
std::string_view to_string(Who w);
std::string_view to_string(Store s);

}  // namespace xlings::subos::situation
