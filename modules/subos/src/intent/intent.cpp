module xlings.subos.intent;

import std;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.spec;

namespace xlings::subos::intent {

namespace {

std::string posix(const fs::path& p) { return p.generic_string(); }

// A host path a mount may not use (design §11): the system's own trees, the
// sandbox's machinery, and the xlings home or anything above it.
std::optional<std::string> mount_refusal(const std::string& src, const std::string& dst,
                                         const HomeView& home) {
    auto norm = [](const std::string& p) { return fs::path(p).lexically_normal().generic_string(); };
    const auto s = norm(src), d = norm(dst);
    const auto h = posix(home.home.lexically_normal());
    if (s.empty() || s.front() != '/') return "the host path must be absolute";
    if (d.empty() || d.front() != '/') return "the path inside must be absolute";
    if (h == s || h.starts_with(s.ends_with('/') ? s : s + "/"))
        return "the xlings home, or a directory above it, cannot be mapped";
    for (std::string_view sys : {"/", "/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/proc",
                                 "/dev", "/sys", "/run", "/run/xlings", "/boot"}) {
        if (d == sys) return std::format("{} is the sandbox's own; map below it or elsewhere", sys);
    }
    if (d.starts_with("/run/xlings/") || d.starts_with("/proc/") || d.starts_with("/sys/"))
        return "that path inside is the sandbox's own";
    return std::nullopt;
}

}  // namespace

policy::Need Intent::need(std::string_view dimension) const {
    if (no_degrade) return policy::Need::Must;
    auto it = needs.find(dimension);
    return it == needs.end() ? policy::Need::Should : it->second;
}

bool Intent::exists(std::string_view path) const {
    if (host_exists) return host_exists(path);
    std::error_code ec;
    return fs::exists(fs::path(path), ec);
}

Intent lower(const policy::Policy& policy, const HomeView& home, const spec::Request& r) {
    Intent in;
    in.instance = r.instance;
    in.instance_dir = r.instance_dir;
    in.id.neutral = policy.identity == policy::Identity::Neutral;
    in.id.user = in.id.neutral ? std::string("user") : r.user;
    in.id.login = r.user;
    in.id.home_inside = "/home/" + in.id.user;
    in.id.etc_dir = r.instance_dir / (in.id.neutral ? "etc-neutral" : "etc");
    in.id.tz = policy.tz;
    in.storage = r.storage;
    in.image_mountpoint = r.image_mountpoint;
    in.root = r.root;
    in.root_mounts = r.root_mounts;
    in.gpu = r.grants.contains("gpu") || policy.grants.contains("gpu");
    in.grants = policy.grants;
    in.grants.insert(r.grants.begin(), r.grants.end());
    in.net.mode = policy.net;
    in.net.proxy = policy.proxy;
    in.net.host_loopback = policy.grants.contains("host-loopback") || r.grants.contains("host-loopback");
    in.net.publish = r.publish;
    in.env.clear = !policy.env_inherit;
    in.env.pass.assign(policy::kBaseEnvPass.begin(), policy::kBaseEnvPass.end());
    in.env.pass.insert(in.env.pass.end(), policy.env_pass.begin(), policy.env_pass.end());
    in.env.explicit_any = policy.env_explicit_any;
    in.disable_userns = policy.disable_userns;
    in.interactive = r.interactive;
    in.shell = r.shell;
    in.argv = r.argv;
    in.cwd = r.cwd;
    in.host_env = r.host_env;
    in.explicit_env = r.explicit_env;
    in.host_exists = r.host_exists;
    in.needs = policy.needs;
    in.no_degrade = policy.no_degrade;
    for (const auto& m : policy.mounts) {
        Grant g{m.src, m.dst.empty() ? m.src : m.dst, m.rw, std::nullopt};
        if (auto why = mount_refusal(g.source, g.inside, home)) g.refused = g.source + ": " + *why;
        else if (!in.exists(g.source)) g.refused = g.source + ": does not exist on the host";
        in.mounts.push_back(std::move(g));
    }
    return in;
}

}  // namespace xlings::subos::intent
