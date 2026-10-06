export module xlings.subos.stage0;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.subos.boot;
import xlings.subos.rootfs;
import xlings.subos.home_view;

// Stage-0 (design part 2 §8.2): xlings as the first process of a machine
// whose root is a SubOS -- the kernel's `init=<home>/boot/xlings-init`.
//
// It is the static xlings, so it depends on no generation of any SubOS: a
// broken glibc, a half-written /usr, a SubOS that will not boot -- stage-0 is
// still there to choose another. In order:
//
//   1. the kernel's file systems, / read-write;
//   2. the system home from /etc/xlings/root.json (default /xlings), and
//      which SubOS to boot from its boot.json (a trial, the default while it
//      has tries left, the fallback);
//   3. /usr pointed at that SubOS's current generation, its factory /etc and
//      sysusers applied;
//   4. its init exec'd -- and when that fails, the next candidate.
//
// Before any home or Config is read: at this point /proc is not mounted, so
// nothing that finds a home by its executable could.
export namespace xlings::subos::stage0 {

namespace fs = std::filesystem;

inline constexpr std::string_view kName = "xlings-init";
inline constexpr std::string_view kAnchor = "/etc/xlings/root.json";

// What a SubOS boots into: the `init` its instance.json declares, else the
// first of /sbin/init, /usr/bin/init present in its /usr.
std::optional<fs::path> init_of(const HomeView& home, std::string_view name);

// Runs as PID 1. Returns only when it cannot be stage-0 (not PID 1).
int run(int argc, char* argv[]);

}  // namespace xlings::subos::stage0

namespace xlings::subos::stage0 {

namespace {

void say(std::string_view msg) {
    std::cerr << "xlings-init: " << msg << std::endl;
}

nlohmann::json read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return nlohmann::json::object();
    auto j = nlohmann::json::parse(std::string{std::istreambuf_iterator<char>(in), {}}, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

bool is_root_subos(const HomeView& home, std::string_view name) {
    std::error_code ec;
    if (!fs::is_directory(home.instance(name), ec)) return false;
    if (read_json(home.instance_file(name)).value("kind", "view") != "rootfs") return false;
    return rootfs::current(home.instance(name)).has_value();
}

void log_event(const HomeView& home, const nlohmann::json& e) {
    std::error_code ec;
    fs::create_directories(home.home / "logs", ec);
    std::ofstream(home.home / "logs" / "boot.ndjson", std::ios::app) << e.dump() << "\n";
}

}  // namespace

std::optional<fs::path> init_of(const HomeView& home, std::string_view name) {
    const auto usr = rootfs::usr_of(home.instance(name));
    std::error_code ec;
    auto in_usr = [&](const fs::path& p) -> std::optional<fs::path> {
        // /sbin and /bin are links into /usr; look where they lead in this SubOS.
        auto rel = p.lexically_relative("/");
        auto first = rel.begin()->string();
        fs::path at = first == "usr" ? usr / rel.lexically_relative("usr")
                    : (first == "sbin" || first == "bin") ? usr / "bin" / rel.lexically_relative(first)
                    : fs::path("/") / rel;
        if (fs::exists(at, ec)) return p;
        return std::nullopt;
    };
    if (auto declared = read_json(home.instance_file(name)).value("init", std::string()); !declared.empty())
        return in_usr(declared);
    for (auto candidate : {"/sbin/init", "/usr/bin/init"})
        if (auto p = in_usr(candidate)) return p;
    return std::nullopt;
}

int run(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    if (!platform::is_pid1()) {
        say("runs as a machine's first process (the kernel's init=); this is not PID 1");
        return 1;
    }
    for (auto [type, target] : {std::pair{"proc", "/proc"}, std::pair{"sysfs", "/sys"},
                                std::pair{"devtmpfs", "/dev"}, std::pair{"tmpfs", "/run"}}) {
        if (!platform::mount_kernel_fs(type, target))
            say(std::format("{} on {}: {} (continuing)", type, target,
                            platform::error_text(platform::last_error())));
    }
    if (!platform::remount_root_rw()) say("/ stays read-only: the boot entry cannot be recorded");

    const auto anchor = read_json(std::string(kAnchor));
    const HomeView home{fs::path(anchor.value("home", std::string("/xlings")))};
    auto config = boot::load(home.boot_file());
    if (!config) {
        say(config.error() + "; booting the defaults");
        config = boot::Config{};
    }
    // A trial naming nothing bootable is dropped, not kept for every boot.
    if (config->once && !is_root_subos(home, *config->once)) {
        say(std::format("the trial '{}' is not a bootable SubOS; dropped", *config->once));
        config->once.reset();
        (void)boot::save(home.boot_file(), *config);
    }
    const auto cands = boot::candidates(*config, [&](std::string_view n) { return is_root_subos(home, n); });
    for (const auto& c : cands) {
        const auto init = init_of(home, c.subos);
        if (!init) {
            say(std::format("'{}' has no init in its /usr; trying the next entry", c.subos));
            log_event(home, {{"event", "boot-skip"}, {"subos", c.subos}, {"why", "no init"}});
            // A trial that cannot start is used up like one that did.
            if (c.via == "once") {
                config->once.reset();
                (void)boot::save(home.boot_file(), *config);
            }
            continue;
        }
        const auto dir = home.instance(c.subos);
        if (auto laid = rootfs::lay_out("/", rootfs::usr_of(dir), home.home); !laid) {
            say(laid.error());
            continue;
        }
        (void)rootfs::fill_machine_etc("/etc", dir);
        (void)rootfs::apply_sysusers("/etc", rootfs::usr_of(dir));
        auto next = boot::record_boot(*config, c);
        if (auto saved = boot::save(home.boot_file(), next); !saved) say(saved.error());
        log_event(home, {{"event", "boot"}, {"subos", c.subos}, {"via", c.via},
                         {"generation", rootfs::current(dir).value_or(0)}, {"init", init->string()}});
        say(std::format("booting '{}' ({}), generation {}", c.subos, c.via,
                        rootfs::current(dir).value_or(0)));
        const std::map<std::string, std::string> env{
            {"PATH", "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin"}, {"HOME", "/root"},
            {"TERM", "linux"}, {"XLINGS_HOME", home.home.string()}};
        const int err = platform::exec_path({init->string()}, env);
        say(std::format("{} did not start: {}; trying the next entry", init->string(),
                        platform::error_text(err)));
        log_event(home, {{"event", "boot-failed"}, {"subos", c.subos}, {"init", init->string()}});
        config = next;
    }
    say("nothing could be booted");
    for (auto shell : {"/bin/sh", "/usr/bin/sh"}) {
        std::error_code ec;
        if (fs::exists(shell, ec)) platform::exec_path({shell}, {{"PATH", "/usr/bin:/bin"}});
    }
    // PID 1 must not exit: the kernel would panic over the message above.
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

}  // namespace xlings::subos::stage0
