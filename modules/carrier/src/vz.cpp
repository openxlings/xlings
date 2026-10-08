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

// The helper: XLINGS_VZ_HELPER names a stand-in (the tests' seam); otherwise
// the tool table's xlings-vm payload.
std::optional<fs::path> helper(const subos::HomeView& home) {
    if (const char* seam = std::getenv("XLINGS_VZ_HELPER"); seam && *seam) return fs::path(seam);
    if (auto found = subos::tools::first("xlings-vm", home, subos::Ports{})) return found->bin;
    return std::nullopt;
}

struct Ran {
    int rc { 126 };
    std::string out;
};

Ran run(const fs::path& h, std::vector<std::string> args) {
    std::vector<std::string> argv{h.string()};
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
        return {.supported = false, .reason = "the VM helper (xlings-vm) is not installed",
                .route = "xlings install xlings-vm"};
    auto r = run(*h, {"probe"});
    if (r.rc != 0)
        return {.supported = false, .reason = "this Mac cannot run the carrier's VM: " + trimmed(r.out),
                .route = "macOS 13 or later on Apple silicon or a Mac with Virtualization.framework",
                .evidence = trimmed(r.out)};
    return {.supported = true, .reason = "Virtualization.framework", .evidence = trimmed(r.out)};
}

std::expected<Endpoint, std::string> ensure(const subos::HomeView& home) {
    const auto h = helper(home);
    if (!h) return std::unexpected("the VM helper (xlings-vm) is not installed");
    const auto name = vm_name(home.home);
    Endpoint at{.carrier = "vz", .instance = name,
                .launcher = {h->string(), "exec", "--name", name, "--"}, .xlings = "/xlings/bin/xlings"};
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
            return std::unexpected(std::format("xlings-vm create {} failed (exit {}): {}", name, created.rc,
                                               trimmed(created.out)));
        status.rc = kStopped;
    }
    if (status.rc == kStopped) {
        if (auto started = run(*h, {"start", "--name", name}); started.rc != 0)
            return std::unexpected(std::format("xlings-vm start {} failed (exit {}): {}", name, started.rc,
                                               trimmed(started.out)));
    } else if (status.rc != 0) {
        return std::unexpected(std::format("xlings-vm status {} answered {}: {}", name, status.rc, trimmed(status.out)));
    }
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
    };
}

Carrier make() {
    Carrier c;
    c.name = "vz";
    c.probe = probe;
    c.ensure = ensure;
    c.stop = [](const Endpoint& at) -> std::expected<void, std::string> {
        if (at.launcher.empty()) return std::unexpected("not a vz endpoint");
        if (auto r = run(at.launcher.front(), {"stop", "--name", at.instance}); r.rc != 0)
            return std::unexpected(std::format("xlings-vm stop {} failed: {}", at.instance, trimmed(r.out)));
        return {};
    };
    c.grant = [](const Endpoint& at, const fs::path& host, bool writable,
                 std::string_view name) -> std::expected<std::string, std::string> {
        if (at.launcher.empty()) return std::unexpected("not a vz endpoint");
        auto r = run(at.launcher.front(), {"share", "--name", at.instance, "--host", host.string(), "--tag",
                                           std::string(name), "--mode", writable ? "rw" : "ro"});
        if (r.rc != 0) return std::unexpected(std::format("cannot share {} with the carrier: {}", host.string(), trimmed(r.out)));
        return trimmed(r.out);
    };
    return c;
}

}  // namespace xlings::carrier::vz
