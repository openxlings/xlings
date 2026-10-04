export module xlings.subos.spec;

import std;
import xlings.libs.json;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.caps;

// Policy + HomeView + Caps -> SandboxSpec (design §16).
//
// The spec is pure data: every mount in order, every namespace, the whole
// environment, the identity, the command. A provider translates it and
// nothing else (bwrap argv, proot argv, an environment for home-redirect);
// a provider never decides what a sandbox gets. That is what makes isolation
// testable without a sandbox -- the unit tests compare compiled specs -- and
// it is the only place a security default lives.
export namespace xlings::subos::spec {

namespace fs = std::filesystem;

enum class Backend { Bwrap, Proot, HomeRedirect, Fake };
enum class Storage { Shared, Image, Tmpfs };

std::string_view to_string(Backend b);
std::string_view to_string(Storage s);
std::optional<Storage> storage_from_string(std::string_view s);

enum class MountKind {
    RoBind,     // host path, read-only
    Bind,       // host path, read-write
    DevBind,    // device node
    Tmpfs,      // empty, private, gone at exit
    Proc,       // a /proc for the sandbox's pid namespace
    Dev,        // a minimal /dev
    Dir,        // an empty directory
};

struct MountOp {
    MountKind kind;
    std::string src;   // host side (binds)
    std::string dst;   // inside
    bool operator==(const MountOp&) const = default;
};

// A requirement the host could not meet.
struct Unmet {
    std::string dimension;   // "fs", "pid", "net", "identity", ...
    std::string reason;
    std::string fix;         // least-privilege remedy, a command when there is one
    policy::Need need { policy::Need::Should };
};

struct Request {
    std::string instance;                     // its name
    fs::path instance_dir;                    // <home>/subos/<name>
    std::string user;                         // login name
    std::vector<std::string> argv;            // empty = the user's shell
    std::string shell { "/bin/sh" };          // $SHELL
    bool interactive { false };               // stdin is a terminal
    Storage storage { Storage::Shared };
    fs::path image_mountpoint;                // Storage::Image
    std::optional<Backend> preferred;         // `--sandbox bwrap|proot`
    std::set<std::string, std::less<>> grants;  // per-call --allow (and legacy --gpu)
    std::map<std::string, std::string> host_env;
    // Host facts the compiler must not read itself, so a test can pin them.
    std::function<bool(std::string_view)> host_exists;
};

struct SandboxSpec {
    Backend backend { Backend::Fake };
    fs::path backend_bin;
    std::vector<MountOp> mounts;              // in application order
    bool unshare_user { false };
    bool unshare_pid { false };
    bool unshare_ipc { false };
    bool unshare_uts { false };
    bool unshare_net { false };
    bool disable_userns { false };
    bool die_with_parent { false };
    bool new_session { false };
    bool block_tiocsti { false };
    std::string hostname;
    // clear_env: the sandbox sees exactly `env`. Otherwise the caller's
    // environment is inherited and `env` is applied on top (Legacy).
    bool clear_env { false };
    std::map<std::string, std::string> env;
    fs::path cwd;                             // inside
    std::vector<std::string> argv;            // the command inside
    fs::path proot_root;                      // Backend::Proot only
    std::vector<Unmet> degraded;              // Should-items not met

    nlohmann::json describe() const;          // for status, audit, golden tests
};

struct Refusal {
    std::vector<Unmet> missing;               // Must-items not met
};

std::expected<SandboxSpec, Refusal> compile(const policy::Policy& policy,
                                            const HomeView& home,
                                            const caps::Caps& caps,
                                            const Request& request);

}  // namespace xlings::subos::spec
