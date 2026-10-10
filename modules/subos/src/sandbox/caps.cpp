module xlings.subos.caps;

import std;
import xlings.platform;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.subos.tools;
import xlings.libs.json;
import xlings.observe;

namespace xlings::subos::caps {

std::string_view platform_name() {
    if constexpr (platform::is_windows) return "windows";
    else if constexpr (platform::is_macos) return "macos";
    else return "linux";
}

Backend backend_of(const tools::Found& f, bool usable = false) {
    return Backend{ .name = f.tool, .bin = f.bin, .source = std::string(tools::to_string(f.source)),
                    .usable = usable };
}

std::optional<Backend> payload_bwrap(const HomeView& home) {
    for (const auto& f : tools::candidates("bwrap", home, Ports{}))
        if (f.source == tools::Source::Payload) return backend_of(f);
    return std::nullopt;
}

namespace {

std::string read_line(const char* path) {
    std::ifstream in(path);
    std::string v;
    std::getline(in, v);
    return v;
}

// What a probe result depends on: this boot of this kernel (a sysctl or an
// AppArmor profile can change, a reboot resets both), and the binary itself
// -- its bytes, and its owner and mode: dropping a setuid bit or handing the
// file to root changes what it can do and not its mtime.
std::optional<std::string> probe_context() {
    std::error_code ec;
    const auto userns = platform::read_symlink("/proc/self/ns/user", ec);
    if (ec) return std::nullopt;
    const auto mountns = platform::read_symlink("/proc/self/ns/mnt", ec);
    if (ec) return std::nullopt;
    std::ifstream status("/proc/self/status");
    if (!status) return std::nullopt;
    std::string credentials, line;
    while (std::getline(status, line))
        if (line.starts_with("Uid:") || line.starts_with("Gid:") || line.starts_with("CapEff:") ||
            line.starts_with("NoNewPrivs:") || line.starts_with("Seccomp:") ||
            line.starts_with("Seccomp_filters:")) credentials += line + '|';
    if (status.bad() || credentials.empty()) return std::nullopt;
    return userns.string() + '|' + mountns.string() + '|' + credentials;
}

std::string cache_key(const Backend& b, std::string_view context) {
    std::error_code ec;
    auto size = fs::file_size(b.bin, ec);
    auto mtime = fs::last_write_time(b.bin, ec).time_since_epoch().count();
    const auto own = platform::file_ownership(b.bin).value_or(platform::FileOwnership{});
    const unsigned mode = own.mode, uid = own.uid;
    return std::format("userns-v3|{}|{}|{}|{:o}|{}|{}|{}|{}|{}", b.bin.string(), size, mtime, mode, uid,
                       read_line("/proc/sys/kernel/osrelease"),
                       read_line("/proc/sys/kernel/random/boot_id"),
                       read_line("/proc/sys/kernel/apparmor_restrict_unprivileged_userns"), context);
}

nlohmann::json entry_of(const Backend& b) {
    return {{"usable", b.usable}, {"output", b.probe_output}, {"disable_userns_fails", b.disable_userns_fails}};
}

// state/isolation-caps.json (design §18): probe results per key. A probe
// is a process start per candidate per entry; on the hot path it is not.
void probe_cached(std::vector<Backend>& all, const HomeView& home, bool fresh) {
    // Host and producer have different namespace/credential authority, even
    // when their backend bytes match. An unobserved context is never cached.
    const auto context = probe_context();
    if (!context) {
        for (auto& backend : all) probe_bwrap(backend);
        return;
    }
    const auto file = home.caps_cache();
    std::error_code ec;
    nlohmann::json cache = nlohmann::json::object();
    if (!fresh && fs::exists(file, ec)) {
        std::ifstream in(file);
        cache = nlohmann::json::parse(in, nullptr, false);
        if (!cache.is_object()) cache = nlohmann::json::object();
    }
    bool dirty = false;
    for (auto& b : all) {
        const auto key = cache_key(b, *context);
        if (auto it = cache.find(key); it != cache.end() && it->is_object()) {
            b.usable = it->value("usable", false);
            b.probe_output = it->value("output", "");
            b.disable_userns_fails = it->value("disable_userns_fails", "");
            observe::trace("caps", std::format("{}: cached ({})", b.bin.string(), b.usable ? "usable" : "fails"));
            continue;
        }
        probe_bwrap(b);
        observe::trace("caps", std::format("{}: probed ({})", b.bin.string(), b.usable ? "usable" : "fails"));
        cache[key] = entry_of(b);
        dirty = true;
    }
    if (!dirty) return;
    fs::create_directories(file.parent_path(), ec);
    // Bounded: entries for binaries and boots long gone are not worth keeping.
    if (cache.size() > 32) cache = nlohmann::json::object();
    for (auto& b : all) cache[cache_key(b, *context)] = entry_of(b);
    auto tmp = fs::path(file.string() + ".tmp");
    std::ofstream(tmp) << cache.dump();
    fs::rename(tmp, file, ec);
}

}  // namespace

std::vector<Backend> bwrap_candidates(const HomeView& home, const Ports& ports, bool fresh) {
    std::vector<Backend> out;
    if constexpr (!platform::is_linux) return out;
    for (const auto& f : tools::candidates("bwrap", home, ports)) out.push_back(backend_of(f));
    probe_cached(out, home, fresh);
    return out;
}

std::optional<Backend> locate_bwrap(const HomeView& home, const Ports& ports) {
    auto all = bwrap_candidates(home, ports);
    for (auto& b : all) if (b.usable && b.disable_userns_fails.empty()) return b;
    for (auto& b : all) if (b.usable) return b;
    if (!all.empty()) return all.front();
    return std::nullopt;
}

std::optional<Backend> locate_proot(const HomeView& home, const Ports& ports) {
    // PATH is not searched: a `proot` on PATH may be an xlings shim, and
    // running it would move the whole session to whichever home owns it.
    if (auto f = tools::first("proot", home, ports)) return backend_of(*f, true);
    return std::nullopt;
}

std::optional<fs::path> locate_pasta(const HomeView& home, const Ports& ports, std::string& why_not) {
    auto found = tools::first("pasta", home, ports);
    if (!found) {
        why_not = "pasta (passt) is not installed";
        return std::nullopt;
    }
    std::error_code ec;
    if (!fs::exists("/dev/net/tun", ec)) {
        why_not = "/dev/net/tun is missing on this host";
        return std::nullopt;
    }
    return found->bin;
}

void probe_bwrap(Backend& b) {
    auto cmd = platform::shell_quote(b.bin.string()) + " --unshare-user --ro-bind / / -- /bin/true";
    auto [status, output] = platform::run_command_capture(cmd);
    b.usable = status == 0;
    b.probe_output = b.usable ? std::string{} : std::move(output);
    b.disable_userns_fails.clear();
    if (!b.usable) return;
    auto [locked, why] = platform::run_command_capture(
        platform::shell_quote(b.bin.string()) + " --unshare-user --disable-userns --ro-bind / / -- /bin/true");
    if (locked != 0) {
        b.disable_userns_fails = why.substr(0, why.find('\n'));
        if (b.disable_userns_fails.empty()) b.disable_userns_fails = std::format("exit {}", locked);
    }
}

Caps probe(const HomeView& home, const Ports& ports) {
    Caps c;
    c.platform = std::string(platform_name());
    if constexpr (platform::is_linux) {
        c.bwrap = locate_bwrap(home, ports);
        c.proot = locate_proot(home, ports);
        c.userns = c.bwrap && c.bwrap->usable;
        c.pasta = locate_pasta(home, ports, c.pasta_missing);
        c.seccomp = true;
        c.landlock_abi = platform::landlock::abi();
        c.kernel = platform::kernel_release();
    }
    return c;
}

}  // namespace xlings::subos::caps
