export module xlings.confine.implementation;

import std;
import xlings.subos.home_view;
import xlings.subos.caps;
import xlings.subos.intent;
import xlings.subos.spec;

// One way to make a sandbox (SubOS design part 3 §6.2).
//
// An implementation answers four questions about itself and nothing about
// the policy: can it run on this host (and why not), what view does it give
// an Intent (mounts, a write fence, a ptrace root), what does it do about
// processes, and what environment and command does the sandboxed program
// start with. The compiler (xlings.confine) asks them in a fixed order and
// keeps the policy's semantics -- what is Must, what degrades -- for itself.
export namespace xlings::confine {

namespace fs = std::filesystem;
namespace sp = xlings::subos::spec;
namespace in = xlings::subos::intent;
using xlings::subos::HomeView;
using xlings::subos::caps::Caps;

// What an implementation provides for one interface of the platform matrix
// (FsGate, ProcessScope, ...) on this host (design part 1 §17).
enum class Enforced { Kernel, Advisory, None };
struct GateClaim {
    std::string gate;
    bool supported { true };
    Enforced enforced { Enforced::Kernel };
    std::string reason;
    std::string route;
};

struct Implementation {
    sp::Backend backend { sp::Backend::Fake };
    std::string_view name;
    // The boundary is the kernel's (namespaces): network, identity, nested
    // user namespaces and read-only mounts can be enforced.
    bool kernel { false };
    // A rootfs instance can be entered (its tree as `/`).
    bool presents_root { false };
    // How a granted socket reaches the sandbox: bound into its view, or --
    // no view to bind into -- passed by the variables that name it.
    enum class Sockets { Bind, PassVariables } sockets { Sockets::Bind };
    // How a `--mount` is honoured: a bind into the view, or added to the
    // paths a write fence allows (no mapping elsewhere).
    enum class Mounts { Bind, Fence } mounts { Mounts::Bind };

    // Can it run here for this Intent? A refusal names the dimension, why,
    // and the least-privilege fix. `chosen` is set to the binary.
    std::function<std::optional<sp::Unmet>(const Caps&, const in::Intent&, fs::path& chosen)> available;
    // The filesystem view, appended to `s.mounts` / `s.landlock_rw` /
    // `s.proot_root`; a requirement of the view it cannot meet refuses.
    std::function<std::optional<sp::Unmet>(const in::Intent&, const HomeView&, sp::SandboxSpec&)> view;
    // Process, IPC and terminal isolation; what it cannot do goes to `s.degraded`.
    std::function<void(const in::Intent&, sp::SandboxSpec&)> processes;
    // The program's environment, directory and command, given the base
    // environment and the grants' variables. Returns when it fully decided.
    std::function<void(const in::Intent&, const HomeView&, const Caps&,
                       const std::map<std::string, std::string>& grant_env, sp::SandboxSpec&)> finish;
    // An advisory boundary says so as a `fs` requirement it cannot meet.
    std::optional<std::string> advisory_fs;
    // Its rows of the platform matrix on this host (none when it cannot run).
    std::function<std::vector<GateClaim>(const Caps&)> gates;
};

}  // namespace xlings::confine
