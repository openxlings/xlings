export module xlings.subos.spec;

import std;
import xlings.libs.json;
import xlings.subos.home_view;
import xlings.subos.policy;

// What a sandbox is, as data (design §16). xlings.confine compiles a policy
// and the host's capabilities into one (part 3 §6.2).
//
// The spec is pure data: every mount in order, every namespace, the whole
// environment, the identity, the command. A provider translates it and
// nothing else (bwrap argv, proot argv, an environment for home-redirect);
// a provider never decides what a sandbox gets. That is what makes isolation
// testable without a sandbox -- the unit tests compare compiled specs -- and
// it is the only place a security default lives.
export namespace xlings::subos::spec {

namespace fs = std::filesystem;

enum class Backend { Bwrap, Proot, HomeRedirect, Landlock, Fake };
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
    TmpOverlay, // `src` seen at `dst` with writes kept in memory, gone at exit
};

// What a root's own state keeps after a session (Luban OS architecture §10.1):
// everything (All), its /home only (Home), nothing (None). Home and None put a
// memory overlay over the rest, so every start is the same system.
enum class Persist { All, Home, None };
std::string_view to_string(Persist p);
std::optional<Persist> persist_from_string(std::string_view s);

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
    // The instance's persona host name (neutral identity); empty = its name.
    std::string hostname;
    std::vector<std::string> argv;            // empty = the user's shell
    std::string shell { "/bin/sh" };          // $SHELL
    bool interactive { false };               // stdin is a terminal
    Storage storage { Storage::Shared };
    fs::path image_mountpoint;                // Storage::Image
    std::optional<Backend> preferred;         // `--sandbox bwrap|proot`
    std::set<std::string, std::less<>> grants;  // per-call --allow (and legacy --gpu)
    std::map<std::string, std::string> host_env;
    // `--env K=V`: set explicitly by the caller, not inherited. Bounded by
    // the policy (Policy::env_explicit_any).
    std::map<std::string, std::string> explicit_env;
    // What xlings itself sets for a system's session (its prompt's facts:
    // LUBAN_KIND, LUBAN_PERSIST, PS1): nothing of the host, so whatever the
    // policy passes; a caller's own --env of the same name wins where allowed.
    std::map<std::string, std::string> system_env;
    std::string cwd;                          // inside; empty = the user's home
    std::vector<std::string> publish;         // --publish HOST:SANDBOX (net=nat)
    // A rootfs instance (design part 2 §3.1): the tree that is `/` inside,
    // <subos>/rootfs. Empty for a view of the host.
    fs::path root;
    // Host facts the compiler must not read itself, so a test can pin them.
    std::function<bool(std::string_view)> host_exists;
    // Where a host path resolves (its symlinks followed), likewise.
    std::function<std::string(std::string_view)> host_resolve;
    // Whether a host path is a device node, likewise.
    std::function<bool(std::string_view)> host_device;
    std::vector<MountOp> root_mounts; // Owner-checked, private skeleton and exact RO leaves.
    Persist persist { Persist::All };     // a root's state after the session
    // Who logs in to a root (part 3 §6.1): its passwd's answer. root unless
    // the root names another (etc/luban/login) or the call does (--user).
    struct RootLogin {
        std::string name { "root" };
        unsigned uid { 0 };
        unsigned gid { 0 };
        std::string home { "/root" };
    } root_login;
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
    bool net_nat { false };                   // a private network, egress through pasta
    bool net_proxy { false };                 // loopback-only netns, declared SOCKS5h exit
    std::string proxy_url;
    fs::path pasta_bin;
    bool host_loopback { false };             // grant: reach the host's own services
    std::vector<std::string> publish;         // "8080:80" host:sandbox TCP ports
    bool disable_userns { false };
    bool die_with_parent { false };
    bool new_session { false };
    bool block_tiocsti { false };
    std::string hostname;
    // The ids inside the user namespace (bwrap --uid/--gid): a rootfs
    // instance runs as its own root, as a container does.
    std::optional<unsigned> uid;
    std::optional<unsigned> gid;
    // clear_env: the sandbox sees exactly `env`. Otherwise the caller's
    // environment is inherited and `env` is applied on top (Legacy).
    bool clear_env { false };
    std::map<std::string, std::string> env;
    fs::path cwd;                             // inside
    std::vector<std::string> argv;            // the command inside
    fs::path proot_root;                      // Backend::Proot only
    // Backend::Landlock: the only places the command may write. Everything
    // else on the host stays readable -- it is a write fence, not a view.
    std::vector<fs::path> landlock_rw;
    std::vector<Unmet> degraded;              // Should-items not met

    nlohmann::json describe() const;          // for status, audit, golden tests
};

struct Refusal {
    std::vector<Unmet> missing;               // Must-items not met
};

}  // namespace xlings::subos::spec
