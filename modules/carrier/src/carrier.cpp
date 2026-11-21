module xlings.carrier;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.platform.stream;
import xlings.platform.target;
import xlings.subos.home_view;
import xlings.subos.std_loader;
import xlings.runtime.cancellation;
import xlings.carrier.wsl2;
import xlings.carrier.vz;

namespace xlings::carrier {

namespace {

Carrier local() {
    Carrier c;
    c.name = "local";
    c.probe = [](const HomeView&) { return Probe{.supported = true, .reason = "this machine's kernel"}; };
    c.ensure = [](const HomeView&) -> std::expected<Endpoint, std::string> {
        return Endpoint{.carrier = "local", .xlings = platform::get_executable_path().string()};
    };
    c.stop = [](const Endpoint&) -> std::expected<void, std::string> { return {}; };
    c.grant = [](const Endpoint&, const fs::path& host, bool, std::string_view) -> std::expected<std::string, std::string> {
        return host.string();   // it is already here
    };
    return c;
}

std::vector<std::string> command(const Endpoint& at, std::span<const std::string> args,
                                 const std::map<std::string, std::string>& env) {
    std::vector<std::string> argv = at.launcher;
    argv.push_back(at.xlings);
    if (!at.launcher.empty() && !env.empty()) {
        // A launcher may not carry variables across (wsl.exe does not), and
        // a guest has no userland of its own to do it (no env(1)): the xlings
        // there sets them and goes on as `xlings <args>`.
        argv.push_back("__carrier-env");
        for (const auto& [k, v] : env) argv.push_back(k + "=" + v);
        argv.push_back("--");
    }
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
}

}  // namespace

namespace {
std::string& version_slot_() {
    static std::string v;
    return v;
}

std::optional<nlohmann::json> ask_probe_(const Endpoint& at) {
    std::vector<std::string> argv = at.launcher;
    argv.insert(argv.end(), {at.xlings, "__carrier-probe"});
    std::string out;
    if (platform::stream::run(argv, [&](std::string_view stream, std::string_view bytes) {
            if (stream == "stdout") out.append(bytes);
        }) != 0)
        return std::nullopt;   // an older guest without the probe
    auto j = nlohmann::json::parse(out, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    return j;
}
}  // namespace

void set_client_version(std::string version) { version_slot_() = std::move(version); }
const std::string& client_version() { return version_slot_(); }

namespace {
GuestBuildSource& guest_build_slot_() {
    static GuestBuildSource source;
    return source;
}

nlohmann::json read_probe_(const fs::path& file) {
    if (std::ifstream in(file); in) {
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object()) return j;
    }
    return nlohmann::json::object();
}

void keep_probe_(const fs::path& file, const nlohmann::json& j) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    try {
        platform::write_file_atomic(file.string(), j.dump() + "\n");
    } catch (const std::exception&) {
        // luban status reads it; the call does not depend on it.
    }
}
}  // namespace

void set_guest_build_source(GuestBuildSource source) { guest_build_slot_() = std::move(source); }

bool older_release(std::string_view a, std::string_view b) {
    auto parts = [](std::string_view v) {
        std::vector<long> out;
        for (const auto part : v | std::views::split('.')) {
            long n = 0;
            std::from_chars(part.data(), part.data() + part.size(), n);
            out.push_back(n);
        }
        return out;
    };
    return parts(a) < parts(b);
}

std::expected<void, std::string> bring_guest(const HomeView& home, const Endpoint& at, const fs::path& probe_file,
                                             const Grant& grant) {
    auto kept = read_probe_(probe_file);
    std::error_code ec;
    const auto t = fs::last_write_time(probe_file, ec);
    const bool fresh = !ec && !kept.empty() && fs::file_time_type::clock::now() - t < std::chrono::minutes(10);
    auto j = kept;
    if (!fresh) {
        if (auto asked = ask_probe_(at)) {
            // What the host decided about the guest stays with the release it
            // decided it for.
            for (const auto* k : {"current_for", "update"})
                if (kept.contains(k) && asked->value("version", std::string{}) == kept.value("version", std::string{}))
                    (*asked)[k] = kept[k];
            j = std::move(*asked);
        }
    }
    const auto& mine = client_version();
    const auto theirs = j.value("version", std::string{});
    const bool current = mine.empty() || theirs == mine || (!theirs.empty() && older_release(mine, theirs)) ||
                         j.value("current_for", std::string{}) == mine;
    if (current) {
        if (!fresh) keep_probe_(probe_file, j);
        return {};
    }
    auto fail = [&](std::string why) -> std::expected<void, std::string> {
        j["update"] = {{"to", mine}, {"ok", false}, {"error", why}};
        keep_probe_(probe_file, j);
        return std::unexpected(std::format(
            "the {} host machine runs xlings {} and was not brought to {}: {} (log: {}); it is tried again on the next call",
            at.carrier, theirs.empty() ? std::string("of an older release") : theirs, mine, why,
            (probe_file.parent_path() / "update.log").string()));
    };
    const auto& source = guest_build_slot_();
    if (!source) return fail("this build cannot give the guest its Linux build");
    auto build = source(home);
    if (!build) return fail("the Linux build of xlings: " + build.error());
    if (build->version == theirs) {
        // The newest build there is (a candidate client): the guest has it.
        j["current_for"] = mine;
        keep_probe_(probe_file, j);
        return {};
    }
    std::string log = std::format("bringing the guest from {} to {} ({})\n", theirs.empty() ? "?" : theirs,
                                  build->version, build->dir.string());
    auto record = [&] {
        std::error_code lec;
        fs::create_directories(probe_file.parent_path(), lec);
        std::ofstream out(probe_file.parent_path() / "update.log", std::ios::app);
        out << log;
    };
    auto granted = grant(at, build->dir, false, "xlings-" + build->version);
    if (!granted) {
        log += granted.error() + "\n";
        record();
        return fail(granted.error());
    }
    std::vector<std::string> argv = at.launcher;
    argv.push_back((fs::path(*granted) / "xlings").generic_string());
    argv.insert(argv.end(), {"__carrier-adopt", fs::path(at.xlings).parent_path().generic_string()});
    const int rc = platform::stream::run(argv, [&](std::string_view, std::string_view bytes) { log.append(bytes); });
    record();
    if (rc != 0) return fail(std::format("adopting the build there exited {}", rc));
    auto after = ask_probe_(at);
    if (!after || after->value("version", std::string{}) != build->version)
        return fail(std::format("the guest still reports {}", after ? after->value("version", std::string{"?"}) : "nothing"));
    j = std::move(*after);
    j["current_for"] = mine;
    j["update"] = {{"to", build->version}, {"ok", true}};
    keep_probe_(probe_file, j);
    return {};
}

int guest_adopt(std::span<const std::string> args) {
    if (args.size() != 1 || !fs::path(args[0]).is_absolute()) {
        std::println(std::cerr, "usage: xlings __carrier-adopt <bin-dir>");
        return 2;
    }
    const fs::path bin(args[0]);
    const auto self = platform::get_executable_path();
    std::error_code ec;
    if (!fs::is_directory(bin, ec)) {
        std::println(std::cerr, "{} is not a directory", bin.string());
        return 1;
    }
    // luban first: a guest with the new xlings and the old luban is the state
    // a failure between the two leaves least wrong (xlings is what the host calls).
    for (const auto& [from, name] : {std::pair{self.parent_path() / "luban", std::string("luban")},
                                     std::pair{self, std::string("xlings")}}) {
        if (!fs::is_regular_file(from, ec)) {
            if (name == "xlings") {
                std::println(std::cerr, "{} is not a file", from.string());
                return 1;
            }
            continue;
        }
        const auto staged = bin / ("." + name + ".adopt");
        fs::remove(staged, ec);
        fs::copy_file(from, staged, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::permissions(staged, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                             fs::perms::others_read | fs::perms::others_exec, ec);
        if (!ec) fs::rename(staged, bin / name, ec);
        if (ec) {
            std::println(std::cerr, "cannot replace {}: {}", (bin / name).string(), ec.message());
            fs::remove(staged, ec);
            return 1;
        }
        std::println("{} <- {}", (bin / name).string(), from.string());
    }
    return 0;
}

std::expected<int, std::string> bring_userland(const fs::path& root, const fs::path& home, std::string_view arch) {
    const auto record = home / "state" / "carrier-userland.json";
    int at = 0;
    if (std::ifstream in(record); in) {
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object() && j.contains("revision") && j["revision"].is_number_integer()) at = j["revision"].get<int>();
    }
    if (at >= kUserlandRevision) return at;
    std::error_code ec;
    // 1: the image carries busybox and /bin/sh; the rest of its applets are
    // linked where a Linux has them (/bin, /usr/bin, ...).
    if (at < 1 && fs::is_regular_file(root / "bin/busybox", ec) && !fs::exists(root / "bin/ls", ec))
        (void)platform::run_argv({(root / "bin/busybox").string(), "--install", "-s"});
    // 2: the standard loader directory is the home's runtime -- a program a
    // hook downloads and starts names /lib64/ld-linux-x86-64.so.2 (#724).
    // Constant: <home>/abi switches generations underneath it. A directory
    // that exists (aarch64's /lib, which WSL makes for /lib/modules) is left
    // to the hook's worker, which merges the runtime into it.
    if (at < 2) {
        if (const auto* s = subos::std_loader::of_arch(arch)) {
            const auto family = fs::path("/") / home.lexically_relative(root) / "abi" / std::string(s->family);
            if (auto linked = subos::std_loader::link_if_absent(root, *s, family); !linked)
                return std::unexpected(linked.error());
        }
    }
    fs::create_directories(record.parent_path(), ec);
    try {
        platform::write_file_atomic(record.string(), nlohmann::json{{"revision", kUserlandRevision}}.dump() + "\n");
    } catch (const std::exception& e) {
        return std::unexpected(record.string() + ": " + e.what());
    }
    return kUserlandRevision;
}

int guest_probe() {
    std::error_code ec;
    // The machine's root is the one its home is in (/xlings in /), and only a
    // carrier's image says so (etc/xlings/root.json): nothing is linked on a
    // machine that is not one.
    const auto home = platform::get_executable_path().parent_path().parent_path();
    const auto root = home.parent_path();
    std::optional<int> userland;
    if (fs::is_regular_file(root / "etc/xlings/root.json", ec)) {
        auto brought = bring_userland(root, home, platform::build_arch());
        if (brought) userland = *brought;
        else std::println(std::cerr, "the userland was not brought forward: {}", brought.error());
    }
    bool kvm = false;
    if (fs::is_character_file("/dev/kvm", ec)) {
        std::fstream dev("/dev/kvm", std::ios::in | std::ios::out);
        kvm = dev.is_open();
    }
    nlohmann::json j{{"kvm", kvm}, {"kernel", platform::kernel_release()}};
    if (!client_version().empty()) j["version"] = client_version();
    if (userland) j["userland"] = *userland;
    std::println("{}", j.dump());
    return 0;
}

std::span<const Carrier> carriers() {
    static const std::vector<Carrier> all{local(), wsl2::make(), vz::make()};
    return all;
}

namespace {
ImageSource& image_source_slot() {
    static ImageSource source;
    return source;
}
}  // namespace

void set_image_source(ImageSource source) { image_source_slot() = std::move(source); }
const ImageSource& image_source() { return image_source_slot(); }

const Carrier* find(std::string_view name) {
    for (const auto& c : carriers())
        if (c.name == name) return &c;
    return nullptr;
}

std::vector<std::string_view> carriers_of(std::string_view platform) {
    if (platform == "windows") return {"local", "wsl2"};
    if (platform == "macos") return {"local", "vz"};
    // XLINGS_WSL_EXE / XLINGS_VZ_HELPER (stand-ins) let the guest carriers'
    // lifecycles be exercised on a Linux CI host; nothing else enables them.
    std::vector<std::string_view> out{"local"};
    if (const char* seam = std::getenv("XLINGS_WSL_EXE"); seam && *seam) out.push_back("wsl2");
    if (const char* seam = std::getenv("XLINGS_VZ_HELPER"); seam && *seam) out.push_back("vz");
    return out;
}

int terminal(const Endpoint& at, std::span<const std::string> args, const std::map<std::string, std::string>& env) {
    if (at.launcher.empty())
        for (const auto& [k, v] : env) platform::set_env_variable(k, v);
    return platform::run_argv(command(at, args, env));
}

std::expected<Reply, std::string> control(const Endpoint& at, std::string_view capability, const nlohmann::json& args) {
    const std::vector<std::string> call{"interface", std::string(capability), "--args", args.dump()};
    std::string out, err;
    const int rc = platform::stream::run(command(at, call, {}), [&](std::string_view stream, std::string_view bytes) {
        (stream == "stderr" ? err : out).append(bytes);
    });
    Reply reply;
    reply.diagnostics = std::move(err);
    std::istringstream lines(out);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        if (j.value("kind", "") == "result") reply.result = std::move(j);
        else reply.events.push_back(std::move(j));
    }
    if (!reply.result.is_object()) {
        return std::unexpected(std::format("{} carrier: `xlings interface {}` answered no result (exit {}){}{}",
                                           at.carrier, capability, rc, reply.diagnostics.empty() ? "" : ": ",
                                           reply.diagnostics));
    }
    reply.exit_code = reply.result.value("exitCode", rc);
    return reply;
}

int relay(const Endpoint& at, std::string_view capability, const nlohmann::json& args,
          const std::map<std::string, std::string>& env, const std::function<void(std::string_view)>& line,
          std::string* diagnostics) {
    if (at.launcher.empty())
        for (const auto& [k, v] : env) platform::set_env_variable(k, v);
    const std::vector<std::string> call{"interface", std::string(capability), "--args", args.dump()};
    std::string pending;
    auto flush = [&](bool all) {
        for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            std::string_view one(pending.data(), nl);
            if (!one.empty() && one.back() == '\r') one.remove_suffix(1);
            if (!one.empty()) line(one);
            pending.erase(0, nl + 1);
        }
        if (all && !pending.empty()) {
            line(pending);
            pending.clear();
        }
    };
    const int rc = platform::stream::run(command(at, call, at.launcher.empty() ? std::map<std::string, std::string>{} : env),
        [&](std::string_view stream, std::string_view bytes) {
            if (stream == "stderr") {
                if (diagnostics) diagnostics->append(bytes);
                return;
            }
            pending.append(bytes);
            flush(false);
        },
        [] { return xlings::stop_requested(); });
    flush(true);
    return rc;
}

std::expected<Choice, Refused> choose(std::string_view platform, const Want& want,
                                      const std::function<Probe(std::string_view)>& probe) {
    const auto available = carriers_of(platform);
    auto checked = [&](std::string_view name, std::string why) -> std::expected<Choice, Refused> {
        const auto p = probe(name);
        if (!p.supported) return std::unexpected(Refused{p.reason, p.route});
        return Choice{std::string(name), std::move(why)};
    };
    if (!want.requested.empty() && want.requested != "auto") {
        if (std::ranges::find(available, want.requested) == available.end())
            return std::unexpected(Refused{std::format("the {} carrier does not exist on {}", want.requested, platform),
                                           std::format("--carrier {}", available.front())});
        if (want.requested == "local" && platform != "linux" && (want.abi == "linux" || want.root))
            return std::unexpected(Refused{std::format("a Linux {} cannot run on {}'s own kernel",
                                                       want.root ? "root" : "SubOS", platform),
                                           std::format("--carrier {}", available.back())});
        return checked(want.requested, "asked for");
    }
    if (available.size() == 1) return checked(available.front(), "this machine's kernel");
    if (want.abi == "linux") return checked(available.back(), "its packages are Linux programs");
    if (want.root) return checked(available.back(), "a root is a Linux userland");
    if (want.strong) return checked(available.back(), "its policy needs isolation this kernel cannot give here");
    return checked(available.front(), "this machine's own programs");
}

}  // namespace xlings::carrier
