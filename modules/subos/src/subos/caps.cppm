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
    std::optional<fs::path> pasta;        // for net=nat
    bool seccomp { false };
    std::string kernel;                   // uname release, for the cache key
};

// Where the backends are looked for, in order (unchanged from the sandbox
// code this replaces; design §20 reorders it in the doctor checkpoint):
//   bwrap  <home>/data/xpkgs/xim-x-bwrap/*/bin/bwrap
//   proot  <home>/data/xpkgs/xim-x-proot/*/bin/proot, <home>/runtimedir/proot,
//          then /usr/bin/proot, /usr/local/bin/proot when not a shim
std::optional<Backend> locate_bwrap(const HomeView& home);
std::optional<Backend> locate_proot(const HomeView& home, const Ports& ports);

// `bwrap --ro-bind / / -- /bin/true`; fills usable / probe_output.
void probe_bwrap(Backend& b);

// Locate and probe everything. Cheap enough to run per entry today; the
// cache (state/isolation-caps.json) keys on kernel + boot + backend + version.
Caps probe(const HomeView& home, const Ports& ports);

// The current platform's name.
std::string_view platform_name();

}  // namespace xlings::subos::caps
