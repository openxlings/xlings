export module xlings.confine.common;

import std;
import xlings.subos.home_view;
import xlings.subos.intent;
import xlings.subos.spec;

// The pieces of a POSIX view more than one implementation shares.
export namespace xlings::confine::common {

namespace fs = std::filesystem;
namespace sp = xlings::subos::spec;
namespace in = xlings::subos::intent;
using xlings::subos::HomeView;

// A POSIX sandbox's paths are POSIX paths, whichever host compiled the spec.
std::string posix(const fs::path& p);

// The host userland every POSIX sandbox needs read-only, and nothing else of
// /etc: the loader cache, DNS, certificates, the zone file.
void host_userland(std::vector<sp::MountOp>& m, const in::Intent& i, sp::Storage storage, bool host_time);

// The instance's own files at the places a POSIX userland looks for them.
void instance_files(std::vector<sp::MountOp>& m, const HomeView& home, const in::Intent& i,
                    sp::Storage storage, bool read_only_home);

// The GPU device nodes and driver files this host has.
std::vector<sp::MountOp> gpu_mounts(const in::Intent& i);

// The proxy variables of net=proxy: everything through the in-sandbox
// SOCKS5h gateway, the host's own proxies emptied.
void proxy_env(sp::SandboxSpec& s);

// `--env K=V` the policy admits, the rest degraded with the command that
// admits it. `config_name` is how the hint names the instance.
void explicit_env(const in::Intent& i, sp::SandboxSpec& s, std::string_view config_name);

// The environment, directory and command of a view of the host (bwrap,
// fake, proot): the instance's home, the home's shims first on PATH.
void view_finish(const in::Intent& i, const HomeView& home, const std::map<std::string, std::string>& grant_env,
                 sp::SandboxSpec& s);

}  // namespace xlings::confine::common
