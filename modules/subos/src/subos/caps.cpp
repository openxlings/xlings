module;

#if defined(__linux__)
#include <sys/stat.h>
#include <sys/utsname.h>
#endif

module xlings.subos.caps;

import std;
import xlings.platform;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.libs.json;
import xlings.observe;

namespace xlings::subos::caps {

std::string_view platform_name() {
    if constexpr (platform::is_windows) return "windows";
    else if constexpr (platform::is_macos) return "macos";
    else return "linux";
}

namespace {

// Sentinel iteration, not a range-for over a directory_iterator: a copy of
// the iterator through a BMI fails to link on the musl cross toolchain, and
// libc++ only offers the sentinel comparison (see the history of the sandbox
// code this replaces).
std::optional<fs::path> first_payload_bin(const fs::path& root, std::string_view bin) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    std::error_code it_ec;
    for (auto it = fs::directory_iterator(root, it_ec);
         !it_ec && it != std::default_sentinel; it.increment(it_ec)) {
        auto candidate = it->path() / "bin" / std::string(bin);
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

}  // namespace

std::optional<Backend> payload_bwrap(const HomeView& home) {
    if (auto bin = first_payload_bin(home.home / "data" / "xpkgs" / "xim-x-bwrap", "bwrap"))
        return Backend{ .name = "bwrap", .bin = *bin, .source = "payload" };
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
// AppArmor profile can change, a reboot resets both), and the binary itself.
std::string cache_key(const Backend& b) {
    std::error_code ec;
    auto size = fs::file_size(b.bin, ec);
    auto mtime = fs::last_write_time(b.bin, ec).time_since_epoch().count();
    return std::format("{}|{}|{}|{}|{}|{}", b.bin.string(), size, mtime,
                       read_line("/proc/sys/kernel/osrelease"),
                       read_line("/proc/sys/kernel/random/boot_id"),
                       read_line("/proc/sys/kernel/apparmor_restrict_unprivileged_userns"));
}

// state/isolation-caps.json (design §18): probe results per key. A probe
// is a process start per candidate per entry; on the hot path it is not.
void probe_cached(std::vector<Backend>& all, const HomeView& home, bool fresh) {
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
        const auto key = cache_key(b);
        if (auto it = cache.find(key); it != cache.end() && it->is_object()) {
            b.usable = it->value("usable", false);
            b.probe_output = it->value("output", "");
            observe::trace("caps", std::format("{}: cached ({})", b.bin.string(), b.usable ? "usable" : "fails"));
            continue;
        }
        probe_bwrap(b);
        observe::trace("caps", std::format("{}: probed ({})", b.bin.string(), b.usable ? "usable" : "fails"));
        cache[key] = {{"usable", b.usable}, {"output", b.probe_output}};
        dirty = true;
    }
    if (!dirty) return;
    fs::create_directories(file.parent_path(), ec);
    // Bounded: entries for binaries and boots long gone are not worth keeping.
    if (cache.size() > 32) cache = nlohmann::json::object();
    for (auto& b : all) cache[cache_key(b)] = {{"usable", b.usable}, {"output", b.probe_output}};
    auto tmp = fs::path(file.string() + ".tmp");
    std::ofstream(tmp) << cache.dump();
    fs::rename(tmp, file, ec);
}

}  // namespace

std::vector<Backend> bwrap_candidates(const HomeView& home, const Ports& ports, bool fresh) {
    std::vector<Backend> out;
    std::error_code ec;
#if defined(__linux__)
    {
        const fs::path p(kRootOwnedBwrap);
        struct stat st{};
        if (::stat(p.c_str(), &st) == 0 && st.st_uid == 0 && !(st.st_mode & (S_IWGRP | S_IWOTH)))
            out.push_back(Backend{ .name = "bwrap", .bin = p, .source = "root-owned" });
    }
#endif
    for (const auto* p : {"/usr/bin/bwrap", "/usr/local/bin/bwrap"}) {
        fs::path candidate(p);
        if (!fs::is_regular_file(candidate, ec)) continue;
        if (ports.shim_owner && ports.shim_owner(candidate)) continue;
        out.push_back(Backend{ .name = "bwrap", .bin = candidate, .source = "system" });
    }
    if (auto b = payload_bwrap(home)) out.push_back(*b);
    probe_cached(out, home, fresh);
    return out;
}

std::optional<Backend> locate_bwrap(const HomeView& home, const Ports& ports) {
    auto all = bwrap_candidates(home, ports);
    for (auto& b : all) if (b.usable) return b;
    if (!all.empty()) return all.front();
    return std::nullopt;
}

std::optional<Backend> locate_proot(const HomeView& home, const Ports& ports) {
    if (auto bin = first_payload_bin(home.home / "data" / "xpkgs" / "xim-x-proot", "proot"))
        return Backend{ .name = "proot", .bin = *bin, .source = "payload", .usable = true };
    std::error_code ec;
    auto runtime = home.home / "runtimedir" / "proot";
    if (fs::is_regular_file(runtime, ec))
        return Backend{ .name = "proot", .bin = runtime, .source = "runtimedir", .usable = true };
    // The host's proot, at the two paths a distribution puts it. PATH is not
    // searched: a `proot` on PATH may be an xlings shim, and running it would
    // move the whole session to whichever home owns it.
    for (const auto* p : {"/usr/bin/proot", "/usr/local/bin/proot"}) {
        fs::path candidate(p);
        if (!fs::is_regular_file(candidate, ec)) continue;
        if (ports.shim_owner && ports.shim_owner(candidate)) continue;
        return Backend{ .name = "proot", .bin = candidate, .source = "host", .usable = true };
    }
    return std::nullopt;
}

std::optional<fs::path> locate_pasta(const HomeView& home, const Ports& ports, std::string& why_not) {
    std::error_code ec;
    std::optional<fs::path> found = first_payload_bin(home.home / "data" / "xpkgs" / "xim-x-passt", "pasta");
    if (!found) {
        for (const auto* p : {"/usr/bin/pasta", "/usr/local/bin/pasta"}) {
            fs::path candidate(p);
            if (!fs::exists(candidate, ec)) continue;
            if (ports.shim_owner && ports.shim_owner(candidate)) continue;
            found = candidate;
            break;
        }
    }
    if (!found) {
        why_not = "pasta (passt) is not installed";
        return std::nullopt;
    }
    if (!fs::exists("/dev/net/tun", ec)) {
        why_not = "/dev/net/tun is missing on this host";
        return std::nullopt;
    }
    return found;
}

void probe_bwrap(Backend& b) {
    auto cmd = platform::shell_quote(b.bin.string()) + " --ro-bind / / -- /bin/true";
    auto [status, output] = platform::run_command_capture(cmd);
    b.usable = status == 0;
    b.probe_output = b.usable ? std::string{} : std::move(output);
}

Caps probe(const HomeView& home, const Ports& ports) {
    Caps c;
    c.platform = std::string(platform_name());
#if defined(__linux__)
    c.bwrap = locate_bwrap(home, ports);
    c.proot = locate_proot(home, ports);
    c.userns = c.bwrap && c.bwrap->usable;
    c.pasta = locate_pasta(home, ports, c.pasta_missing);
    c.seccomp = true;
    struct utsname u{};
    if (::uname(&u) == 0) c.kernel = u.release;
#else
    (void)home;
    (void)ports;
#endif
    return c;
}

}  // namespace xlings::subos::caps
