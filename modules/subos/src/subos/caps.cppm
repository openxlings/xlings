export module xlings.subos.caps;

import std;
import xlings.subos.home_view;
import xlings.subos.ports;

// What this host can do for isolation (design §18), measured, never assumed.
//
// The compiler decides what a sandbox gets from Policy + Caps; the platform
// matrix `subos status` prints is these probes (xlings.subos.gates). A
// capability is a FACT with its evidence: a backend that was found but does
// not work carries the probe's raw output, because "bwrap failed" without
// the reason is how #640's misleading sysctl advice happened.
export namespace xlings::subos::caps {

namespace fs = std::filesystem;

struct Backend {
    std::string name;            // "bwrap" | "proot"
    fs::path bin;
    // payload | runtimedir | host -- where it was found; a host binary is
    // used only when it is DECLARED here and reported (design §20).
    std::string source;
    bool usable { false };       // the probe passed
    std::string probe_output;    // raw stdout+stderr of a failed probe
};

struct Caps {
    std::string platform;                 // linux | macos | windows
    std::optional<Backend> bwrap;
    std::optional<Backend> proot;
    bool userns { false };                // unprivileged user namespaces work
    int landlock_abi { 0 };               // 0 = no Landlock
    std::optional<fs::path> pasta;        // for net=nat (only when /dev/net/tun exists)
    std::string pasta_missing;            // why not, when it is not there
    bool seccomp { false };
    std::string kernel;                   // uname release, for the cache key
};

// Where the backends are looked for, in order (design §20, #640 F10):
//   bwrap  1. /usr/lib/xlings/bwrap, root-owned and not writable by others --
//             what `self doctor --isolation --fix` installs, with a narrow
//             AppArmor profile that grants it user namespaces;
//          2. the system's /usr/bin/bwrap, /usr/local/bin/bwrap (not a shim),
//             when its probe passes -- used, and reported as the host's;
//          3. <home>/data/xpkgs/xim-x-bwrap/*/bin/bwrap.
//          No setuid: xlings neither makes one nor relies on one.
//   proot  <home>/data/xpkgs/xim-x-proot/*/bin/proot, <home>/runtimedir/proot,
//          then /usr/bin/proot, /usr/local/bin/proot when not a shim
inline constexpr std::string_view kRootOwnedBwrap = "/usr/lib/xlings/bwrap";

// Every bwrap found, in that order, each probed. The doctor reports them all.
// `fresh` probes every one again (the doctor's view) and refreshes the cache.
std::vector<Backend> bwrap_candidates(const HomeView& home, const Ports& ports, bool fresh = false);
// The first usable one; otherwise the first found (with its probe output).
std::optional<Backend> locate_bwrap(const HomeView& home, const Ports& ports);
// The payload only (what `xlings install bwrap` put there).
std::optional<Backend> payload_bwrap(const HomeView& home);
std::optional<Backend> locate_proot(const HomeView& home, const Ports& ports);

// pasta (from passt) for net=nat: the payload, then the host's at the two
// usual paths when it is not a shim. Usable only with /dev/net/tun.
std::optional<fs::path> locate_pasta(const HomeView& home, const Ports& ports, std::string& why_not);

// `bwrap --ro-bind / / -- /bin/true`; fills usable / probe_output.
void probe_bwrap(Backend& b);

// Locate and probe everything. Cheap enough to run per entry today; the
// cache (state/isolation-caps.json) keys on kernel + boot + backend + version.
Caps probe(const HomeView& home, const Ports& ports);

// The current platform's name.
std::string_view platform_name();

}  // namespace xlings::subos::caps
