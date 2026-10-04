module;

#if defined(__linux__)
#include <sys/utsname.h>
#endif

module xlings.subos.caps;

import std;
import xlings.platform;
import xlings.subos.home_view;
import xlings.subos.ports;

namespace xlings::subos::caps {

std::string_view platform_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
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

std::optional<Backend> locate_bwrap(const HomeView& home) {
    if (auto bin = first_payload_bin(home.home / "data" / "xpkgs" / "xim-x-bwrap", "bwrap"))
        return Backend{ .name = "bwrap", .bin = *bin, .source = "payload" };
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
    if (auto b = locate_bwrap(home)) {
        probe_bwrap(*b);
        c.bwrap = std::move(b);
    }
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
