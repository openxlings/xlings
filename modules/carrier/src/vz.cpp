module xlings.carrier.vz;

import std;
import xlings.carrier;
import xlings.libs.sha256;
import xlings.platform;
import xlings.platform.stream;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.subos.tools;

namespace xlings::carrier::vz {

namespace {

constexpr int kStopped = 3;
constexpr int kAbsent = 4;

// The helper (part 4 §6.6): luban beside this xlings, as `luban __vm` --
// Virtualization.framework in a binary signed with its entitlement.
// XLINGS_VZ_HELPER names a stand-in that answers the verbs directly (the
// tests' seam).
struct Helper {
    std::vector<std::string> argv;   // what a verb is appended to
};

std::optional<Helper> helper(const subos::HomeView&) {
    if (const char* seam = std::getenv("XLINGS_VZ_HELPER"); seam && *seam) return Helper{{seam}};
    std::error_code ec;
    const auto luban = platform::get_executable_path().parent_path() / "luban";
    if (fs::is_regular_file(luban, ec)) return Helper{{luban.string(), "__vm"}};
    return std::nullopt;
}

struct Ran {
    int rc { 126 };
    std::string out;
};

Ran run(const Helper& h, std::vector<std::string> args) {
    std::vector<std::string> argv = h.argv;
    argv.insert(argv.end(), args.begin(), args.end());
    Ran r;
    r.rc = platform::stream::run(argv, [&](std::string_view, std::string_view bytes) { r.out.append(bytes); });
    return r;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

Probe probe(const subos::HomeView& home) {
    const bool seam = std::getenv("XLINGS_VZ_HELPER") != nullptr;
    if (!seam && !platform::is_macos)
        return {.supported = false, .reason = "the vz carrier exists on macOS", .route = "--carrier local"};
    const auto h = helper(home);
    if (!h)
        return {.supported = false,
                .reason = "the Luban host machine is luban's, and there is no luban beside this xlings",
                .route = "xlings self update  (the release carries both)"};
    auto r = run(*h, {"probe"});
    if (r.rc != 0)
        return {.supported = false, .reason = trimmed(r.out),
                .route = "macOS 13 or later on Apple silicon, not inside another virtual machine",
                .evidence = trimmed(r.out)};
    return {.supported = true, .reason = "Virtualization.framework", .evidence = trimmed(r.out)};
}

std::expected<Endpoint, std::string> ensure(const subos::HomeView& home) {
    const auto h = helper(home);
    if (!h) return std::unexpected("there is no luban beside this xlings to run the Luban host machine");
    const auto name = vm_name(home.home);
    Endpoint at{.carrier = "vz", .instance = name, .launcher = h->argv, .xlings = "/xlings/bin/xlings"};
    at.launcher.insert(at.launcher.end(), {"exec", "--name", name, "--"});
    auto status = run(*h, {"status", "--name", name});
    if (status.rc == kAbsent) {
        if (!image_source()) return std::unexpected("this build cannot make a carrier image");
        auto image = image_source()(home, image_files());
        if (!image) return std::unexpected("cannot make the carrier image: " + image.error());
        const auto dir = home.home / "carriers" / "vz";
        std::error_code ec;
        fs::create_directories(dir, ec);
        auto created = run(*h, {"create", "--name", name, "--dir", dir.string(), "--image", image->string()});
        if (created.rc != 0)
            return std::unexpected(std::format("the host machine {} was not made (exit {}): {}", name, created.rc,
                                               trimmed(created.out)));
        status.rc = kStopped;
    }
    if (status.rc == kStopped) {
        if (auto started = run(*h, {"start", "--name", name}); started.rc != 0)
            return std::unexpected(std::format("the host machine {} did not start (exit {}): {}", name, started.rc,
                                               trimmed(started.out)));
    } else if (status.rc != 0) {
        return std::unexpected(std::format("the host machine {} answered {}: {}", name, status.rc, trimmed(status.out)));
    }
    refresh_guest(at, home.home / "carriers" / "vz" / "probe.json");
    return at;
}

}  // namespace

std::string vm_name(const fs::path& home) {
    return "xlings-" + sha256::hex(home.lexically_normal().generic_string()).substr(0, 12);
}

std::vector<ImageFile> image_files() {
    return {
        {.path = "etc", .directory = true},
        {.path = "etc/passwd", .content = "root:x:0:0:root:/root:/xlings/bin/xlings\n"},
        {.path = "etc/group", .content = "root:x:0:\n"},
        {.path = "etc/hostname", .content = "xlings-carrier\n"},
        // The kernel's own DHCP (ip=dhcp) writes the name servers it was
        // given to /proc/net/pnp, in resolv.conf's form.
        {.path = "etc/resolv.conf", .link = "/proc/net/pnp"},
        {.path = "etc/xlings", .directory = true},
        {.path = "etc/xlings/root.json", .content = "{ \"home\": \"/xlings\" }\n"},
        {.path = "xlings", .directory = true},
        {.path = "xlings/bin", .directory = true},
        {.path = "xlings/.xlings.json", .content = "{}\n"},
        {.path = "root", .mode = 0700, .directory = true},
        {.path = "tmp", .mode = 01777, .directory = true},
        {.path = "proc", .directory = true},
        {.path = "sys", .directory = true},
        {.path = "dev", .directory = true},
        {.path = "run", .directory = true},
        {.path = "grant", .mode = 0755, .directory = true},
        {.path = "usr", .directory = true},
        {.path = "usr/bin", .directory = true},
        {.path = "usr/bin/xlings", .link = "/xlings/bin/xlings"},
        // What Virtualization.framework boots: the machine kernel, beside
        // the tree it shares (the Linux luban in it is the initramfs).
        {.path = "boot", .directory = true},
        {.path = "boot/vmlinuz", .mode = 0644, .kernel = true},
        {.path = "newroot", .directory = true},
    };
}

Carrier make() {
    Carrier c;
    c.name = "vz";
    c.probe = probe;
    c.ensure = ensure;
    c.stop = [](const Endpoint& at) -> std::expected<void, std::string> {
        const auto h = helper(subos::HomeView{});
        if (!h) return std::unexpected("there is no luban beside this xlings");
        if (auto r = run(*h, {"stop", "--name", at.instance}); r.rc != 0)
            return std::unexpected(std::format("the host machine {} did not stop: {}", at.instance, trimmed(r.out)));
        return {};
    };
    c.grant = [](const Endpoint& at, const fs::path& host, bool writable,
                 std::string_view name) -> std::expected<std::string, std::string> {
        const auto h = helper(subos::HomeView{});
        if (!h) return std::unexpected("there is no luban beside this xlings");
        auto r = run(*h, {"share", "--name", at.instance, "--host", host.string(), "--tag",
                                           std::string(name), "--mode", writable ? "rw" : "ro"});
        if (r.rc != 0) return std::unexpected(std::format("cannot share {} with the carrier: {}", host.string(), trimmed(r.out)));
        return trimmed(r.out);
    };
    return c;
}

}  // namespace xlings::carrier::vz
