module xlings.carrier.wsl2;

import std;
import xlings.carrier;
import xlings.libs.sha256;
import xlings.platform;
import xlings.platform.stream;
import xlings.subos.home_view;

namespace xlings::carrier::wsl2 {

namespace {

// wsl.exe: Windows' own, under %SystemRoot%. XLINGS_WSL_EXE names another --
// the seam the tests use to run this carrier's lifecycle against a stand-in.
std::optional<fs::path> wsl_exe() {
    if (const char* seam = std::getenv("XLINGS_WSL_EXE"); seam && *seam) return fs::path(seam);
    if constexpr (!platform::is_windows) return std::nullopt;
    const char* root = std::getenv("SystemRoot");
    const fs::path exe = fs::path(root && *root ? root : "C:\\Windows") / "System32" / "wsl.exe";
    std::error_code ec;
    if (!fs::exists(exe, ec)) return std::nullopt;
    return exe;
}

struct Ran {
    int rc { 126 };
    std::string out;
};

// wsl.exe answers its own commands in UTF-16 unless told otherwise.
Ran run(const fs::path& wsl, std::vector<std::string> args) {
    platform::set_env_variable("WSL_UTF8", "1");
    std::vector<std::string> argv{wsl.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    Ran r;
    r.rc = platform::stream::run(argv, [&](std::string_view, std::string_view bytes) { r.out.append(bytes); });
    return r;
}

std::string first_lines(const std::string& text, int n) {
    std::string out;
    std::istringstream in(text);
    for (std::string line; n-- > 0 && std::getline(in, line);) {
        std::erase(line, '\0');
        std::erase(line, '\r');
        if (!line.empty()) out += (out.empty() ? "" : " | ") + line;
    }
    return out;
}

constexpr std::string_view kInstall = "wsl --install --no-distribution   (once, as administrator; then restart)";

Probe probe(const subos::HomeView&) {
    const auto wsl = wsl_exe();
    if (!wsl) {
        if constexpr (!platform::is_windows)
            return {.supported = false, .reason = "the wsl2 carrier exists on Windows", .route = "--carrier local"};
        return {.supported = false, .reason = "WSL is not installed", .route = std::string(kInstall)};
    }
    auto status = run(*wsl, {"--status"});
    if (status.rc != 0)
        return {.supported = false, .reason = "WSL did not answer: " + first_lines(status.out, 3),
                .route = std::string(kInstall), .evidence = first_lines(status.out, 6)};
    // WSL1 translates system calls: no namespaces, so no sandbox and no root.
    std::string flat = status.out;
    std::erase(flat, '\0');
    if (flat.find("WSL 2 is not supported") != std::string::npos ||
        flat.find("requires an update to its kernel component") != std::string::npos)
        return {.supported = false, .reason = "only WSL1 is available here; a SubOS needs WSL2's kernel",
                .route = "enable the Virtual Machine Platform, then: wsl --update", .evidence = first_lines(flat, 6)};
    return {.supported = true, .reason = "WSL2", .evidence = first_lines(flat, 6)};
}

fs::path carrier_dir(const subos::HomeView& home) { return home.home / "carriers" / "wsl2"; }

std::expected<Endpoint, std::string> ensure(const subos::HomeView& home) {
    const auto wsl = wsl_exe();
    if (!wsl) return std::unexpected("WSL is not installed");
    const auto name = distro_name(home.home);
    Endpoint at{.carrier = "wsl2", .instance = name, .launcher = launcher(*wsl, name), .xlings = "/xlings/bin/xlings"};
    auto listed = run(*wsl, {"--list", "--quiet"});
    if (listed.rc == 0 && std::ranges::contains(parse_list(listed.out), name)) return at;

    // First use for this home: its machine, imported once.
    if (!image_source()) return std::unexpected("this build cannot make a carrier image");
    auto image = image_source()(home, image_files());
    if (!image) return std::unexpected("cannot make the carrier image: " + image.error());
    const auto disk = carrier_dir(home) / "disk";
    std::error_code ec;
    fs::create_directories(disk, ec);
    auto imported = run(*wsl, {"--import", name, disk.string(), image->string(), "--version", "2"});
    if (imported.rc != 0)
        return std::unexpected(std::format("wsl --import {} failed (exit {}): {}", name, imported.rc,
                                           first_lines(imported.out, 4)));
    // It answers as an xlings home before it is handed out.
    std::vector<std::string> check = at.launcher;
    check.insert(check.end(), {at.xlings, "--version"});
    if (const int rc = platform::run_argv(check); rc != 0)
        return std::unexpected(std::format("the carrier {} was imported but its xlings does not run (exit {})", name, rc));
    return at;
}

}  // namespace

std::string distro_name(const fs::path& home) {
    const auto digest = sha256::hex(home.lexically_normal().generic_string());
    return "xlings-" + digest.substr(0, 12);
}

std::vector<std::string> parse_list(std::string_view output) {
    std::string text(output);
    std::erase(text, '\0');                                   // UTF-16LE of ASCII names
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);   // UTF-8 BOM
    if (text.starts_with("\xFF\xFE")) text.erase(0, 2);       // UTF-16 BOM, NULs gone
    std::vector<std::string> names;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::erase(line, '\r');
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        if (!line.empty()) names.push_back(line);
    }
    return names;
}

std::vector<ImageFile> image_files() {
    return {
        {.path = "etc", .directory = true},
        // No Windows drives, no Windows programs: the host reaches a SubOS
        // only through a grant (CARRIER-WSL-INTEROP-OFF).
        {.path = "etc/wsl.conf",
         .content = "# xlings carrier (SubOS design part 3 §5.3)\n"
                    "[automount]\nenabled = false\nmountFsTab = false\n\n"
                    "[interop]\nenabled = false\nappendWindowsPath = false\n\n"
                    "[user]\ndefault = root\n\n"
                    "[boot]\nsystemd = false\n"},
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
        {.path = "sbin", .directory = true},
        // drvfs is mounted by WSL's /init when it runs as mount.drvfs.
        {.path = "sbin/mount.drvfs", .link = "/init"},
        {.path = "usr", .directory = true},
        {.path = "usr/bin", .directory = true},
        {.path = "usr/bin/xlings", .link = "/xlings/bin/xlings"},
    };
}

std::vector<std::string> launcher(const fs::path& wsl, std::string_view name) {
    return {wsl.string(), "-d", std::string(name), "-u", "root", "--exec"};
}

int guest_grant(std::span<const std::string> args) {
    if (args.size() != 3 || (args[2] != "rw" && args[2] != "ro")) {
        std::println(std::cerr, "usage: xlings __carrier-grant <windows-path> <target> <rw|ro>");
        return 2;
    }
    const fs::path target(args[1]);
    if (!target.is_absolute() || !target.lexically_normal().generic_string().starts_with("/grant/")) {
        std::println(std::cerr, "a grant is mounted under /grant/, never elsewhere");
        return 2;
    }
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        std::println(std::cerr, "cannot create {}: {}", target.string(), ec.message());
        return 1;
    }
    return platform::run_argv({"/sbin/mount.drvfs", args[0], target.string(), "-o", args[2] == "rw" ? "rw" : "ro"});
}

Carrier make() {
    Carrier c;
    c.name = "wsl2";
    c.probe = probe;
    c.ensure = ensure;
    c.stop = [](const Endpoint& at) -> std::expected<void, std::string> {
        const auto wsl = wsl_exe();
        if (!wsl) return std::unexpected("WSL is not installed");
        if (auto r = run(*wsl, {"--terminate", at.instance}); r.rc != 0)
            return std::unexpected(std::format("wsl --terminate {} failed: {}", at.instance, first_lines(r.out, 2)));
        return {};
    };
    c.grant = [](const Endpoint& at, const fs::path& host, bool writable,
                 std::string_view name) -> std::expected<std::string, std::string> {
        const auto target = "/grant/" + std::string(name);
        std::vector<std::string> argv = at.launcher;
        argv.insert(argv.end(), {at.xlings, "__carrier-grant", host.string(), target, writable ? "rw" : "ro"});
        if (const int rc = platform::run_argv(argv); rc != 0)
            return std::unexpected(std::format("cannot grant {} to the carrier (exit {})", host.string(), rc));
        return target;
    };
    return c;
}

}  // namespace xlings::carrier::wsl2
