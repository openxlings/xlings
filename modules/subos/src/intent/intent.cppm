export module xlings.subos.intent;

import std;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.spec;

// What a sandbox is asked to be, before anything decides how
// (SubOS design part 3 §6.1).
//
// The policy and the call (spec::Request) lowered into the decisions that do
// not depend on the backend: who the user is, what the network should be,
// which host paths are mapped in -- in order, each with its refusal when it
// may not be, the shape of an openkal preopen: a name inside, a source, a
// mode -- which devices and sockets are granted, which variables may pass.
// An implementation (xlings.confine) turns an Intent into its own plan; it
// never reads the policy again.
export namespace xlings::subos::intent {

namespace fs = std::filesystem;

// A host path mapped in (`--mount`), or the reason it may not be.
struct Grant {
    std::string source;          // host path, POSIX form
    std::string inside;          // where it appears
    bool writable { true };
    std::optional<std::string> refused;   // a Must: the sandbox is not entered
};

struct Identity {
    bool neutral { false };      // private/locked: `user`, the instance's host name, UTC
    std::string user;            // login name inside
    std::string login;           // the caller's login (a host-side directory name)
    std::string home_inside;     // /home/<user>
    fs::path etc_dir;            // the instance's passwd/group/hosts
    std::string tz;              // neutral only; empty = UTC
    std::string hostname;        // neutral only: the persona's; empty = the instance's name
};

struct Network {
    policy::Net mode { policy::Net::Host };
    std::string proxy;           // Net::Proxy
    bool host_loopback { false };
    std::vector<std::string> publish;
};

struct Environment {
    bool clear { true };                          // !env_inherit
    std::vector<std::string> pass;                // base + policy env_pass
    bool explicit_any { true };                   // --env may name any variable
};

struct Intent {
    std::string instance;
    fs::path instance_dir;
    Identity id;
    spec::Storage storage { spec::Storage::Shared };
    fs::path image_mountpoint;
    fs::path root;                                // a rootfs instance's tree; empty = a view
    std::vector<spec::MountOp> root_mounts;
    std::vector<Grant> mounts;                    // in the policy's order
    std::set<std::string, std::less<>> grants;    // named grants: policy's and this call's
    bool gpu { false };
    Network net;
    Environment env;
    bool disable_userns { false };
    bool interactive { false };
    std::string shell;
    std::vector<std::string> argv;
    std::string cwd;
    std::map<std::string, std::string> host_env;
    std::map<std::string, std::string> explicit_env;
    std::function<bool(std::string_view)> host_exists;

    // How hard a dimension is required: Must under --no-degrade, else the
    // policy's word, else Should.
    std::map<std::string, policy::Need, std::less<>> needs;
    bool no_degrade { false };
    [[nodiscard]] policy::Need need(std::string_view dimension) const;

    // A host path exists (the request's seam, so goldens pin host facts).
    [[nodiscard]] bool exists(std::string_view path) const;
};

Intent lower(const policy::Policy& policy, const HomeView& home, const spec::Request& request);

}  // namespace xlings::subos::intent
