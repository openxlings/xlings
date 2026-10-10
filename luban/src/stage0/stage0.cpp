// See stage0.cppm for the boot boundary: no Config and no guessing unreadable state.
module luban.stage0;

import std;
import xlings.libs.json;
import xlings.platform;
import luban.boot;
import luban.machine;
import xlings.subos.rootfs;
import xlings.subos.library_cache;
import xlings.subos.home_view;

namespace luban::stage0 {

namespace platform = xlings::platform;
namespace rootfs = xlings::subos::rootfs;
namespace library_cache = xlings::subos::library_cache;
using xlings::subos::HomeView;

namespace {

// The name it was started as: luban-init, or xlings-init (xlings itself).
std::string& self_name() {
    static std::string name = "luban-init";
    return name;
}

void say(std::string_view msg) {
    std::cerr << self_name() << ": " << msg << std::endl;
}

std::expected<nlohmann::json, std::string> read_json(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected(path.string() + ": cannot be inspected: " + ec.message());
    if (status.type() == fs::file_type::not_found) return nlohmann::json::object();
    if (!fs::is_regular_file(path, ec) || ec)
        return std::unexpected(path.string() + ": is not a readable regular file");
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(path.string() + ": cannot be read");
    std::string bytes;
    try { bytes.assign(std::istreambuf_iterator<char>(in), {}); }
    catch (const std::exception& e) { return std::unexpected(path.string() + ": " + e.what()); }
    const auto document = nlohmann::json::parse(bytes, nullptr, false);
    if (in.bad() || !document.is_object())
        return std::unexpected(path.string() + ": expected a JSON object");
    return document;
}

std::expected<bool, std::string> is_root_subos(const HomeView& home, std::string_view name) {
    std::error_code ec;
    const auto status = fs::status(home.instance(name), ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected("cannot inspect boot instance: " + ec.message());
    if (!fs::is_directory(status)) return false;
    const auto file = home.instance_file(name);
    const auto document = read_json(file);
    if (!document) return std::unexpected(document.error());
    const auto kind = document->find("kind");
    if (kind == document->end()) return false;
    if (!kind->is_string() || (*kind != "rootfs" && *kind != "view"))
        return std::unexpected(file.string() + ": unknown or invalid SubOS kind");
    if (*kind == "view") return false;
    return rootfs::current(home.instance(name)).has_value();
}

void log_event(const HomeView& home, const nlohmann::json& event) {
    std::error_code ec;
    fs::create_directories(home.home / "logs", ec);
    std::ofstream out(home.home / "logs" / "boot.ndjson", std::ios::app);
    out << event.dump() << "\n";
    out.close();
    if (ec || !out) say("cannot append boot audit");
}

[[noreturn]] void recover(std::string_view reason) {
    say(std::string(reason) + "; entering recovery; repair the named metadata before rebooting");
    for (;;) {
        for (auto shell : {"/bin/sh", "/usr/bin/sh"}) {
            std::error_code ec;
            if (!fs::exists(shell, ec)) continue;
            say("starting a recovery shell; exiting it returns to recovery");
            // Keep stage-0 as PID 1: an exiting recovery shell must not panic the kernel.
            platform::run_argv({shell});
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

}  // namespace

std::expected<fs::path, std::string> read_home_anchor(const fs::path& anchor) {
    std::error_code ec;
    const auto status = fs::symlink_status(anchor, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected(anchor.string() + ": cannot be inspected: " + ec.message());
    if (status.type() == fs::file_type::not_found) return fs::path("/xlings");
    const auto document = read_json(anchor);
    if (!document) return std::unexpected(document.error());
    const auto value = document->find("home");
    if (value == document->end() || !value->is_string())
        return std::unexpected(anchor.string() + ": home must be an absolute path string");
    const fs::path home = value->get<std::string>();
    if (home.empty() || !home.is_absolute() || home.lexically_normal() != home)
        return std::unexpected(anchor.string() + ": home must be a nonempty normalized absolute path");
    return home;
}

std::expected<std::optional<fs::path>, std::string> init_of(const HomeView& home,
                                                         std::string_view name) {
    const auto usr = rootfs::usr_of(home.instance(name));
    const auto file = home.instance_file(name);
    const auto document = read_json(file);
    if (!document) return std::unexpected(document.error());
    if (const auto kind = document->find("kind"); kind != document->end()
        && (!kind->is_string() || (*kind != "rootfs" && *kind != "view")))
        return std::unexpected(file.string() + ": unknown or invalid SubOS kind");
    auto in_usr = [&](const fs::path& path) -> std::expected<std::optional<fs::path>, std::string> {
        if (!path.is_absolute() || path == "/" || path.lexically_normal() != path)
            return std::unexpected(file.string() + ": init must be a normalized absolute executable path");
        const auto rel = path.relative_path();
        const auto first = rel.begin()->string();
        const fs::path at = first == "usr" ? usr / rel.lexically_relative("usr")
            : (first == "sbin" || first == "bin") ? usr / "bin" / rel.lexically_relative(first)
            : path;
        std::error_code ec;
        const auto entry = fs::symlink_status(at, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return std::unexpected("cannot inspect init " + at.string() + ": " + ec.message());
        if (entry.type() == fs::file_type::not_found) return std::nullopt;
        const auto status = fs::status(at, ec);
        if (ec || !fs::is_regular_file(status))
            return std::unexpected("init is unreadable or not a regular executable: " + at.string());
        if ((status.permissions() & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec))
            == fs::perms::none)
            return std::unexpected("init is not executable: " + at.string());
        return std::optional<fs::path>{path};
    };
    const auto declared = document->find("init");
    if (declared != document->end()) {
        if (!declared->is_string() || declared->get<std::string>().empty())
            return std::unexpected(file.string() + ": init must be a nonempty absolute path string");
        return in_usr(declared->get<std::string>());
    }
    for (auto candidate : {"/sbin/init", "/usr/bin/init"}) {
        auto init = in_usr(candidate);
        if (!init || init->has_value()) return init;
    }
    return std::nullopt;
}

int run(int argc, char* argv[]) {
    if (argc >= 1 && argv[0] && *argv[0]) self_name() = std::filesystem::path(argv[0]).filename().string();
    if (!platform::is_pid1()) {
        say("runs as a machine's first process (the kernel's init=); this is not PID 1");
        return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    // Where the budget goes, per phase (a boot that is slow says which step).
    nlohmann::json phases = nlohmann::json::object();
    auto lap = started;
    auto mark = [&](const char* phase) {
        const auto now = std::chrono::steady_clock::now();
        phases[phase] = phases.value(phase, std::int64_t{0}) +
            std::chrono::duration_cast<std::chrono::microseconds>(now - lap).count();
        lap = now;
    };
    for (auto [type, target] : {std::pair{"proc", "/proc"}, std::pair{"sysfs", "/sys"},
                                std::pair{"devtmpfs", "/dev"}, std::pair{"tmpfs", "/run"}}) {
        if (!platform::mount_kernel_fs(type, target))
            say(std::format("{} on {}: {} (continuing)", type, target,
                            platform::error_text(platform::last_error())));
    }
    if (!platform::remount_root_rw()) recover("/ stays read-only; boot state cannot be recorded");
    mark("mounts");
    const auto anchor = read_home_anchor(std::string(kAnchor));
    if (!anchor) recover(anchor.error());
    const HomeView home{*anchor};
    auto config = boot::load(home.boot_file());
    if (!config) recover(config.error());

    std::map<std::string, bool> bootable;
    for (const auto& name : {config->default_entry, config->fallback, config->once.value_or("")}) {
        if (name.empty() || bootable.contains(name)) continue;
        const auto root = is_root_subos(home, name);
        if (!root) recover(root.error());
        bootable[name] = *root;
    }
    if (config->once && !bootable[*config->once]) {
        auto next = *config;
        next.once.reset();
        if (auto saved = boot::save(home.boot_file(), next); !saved) recover(saved.error());
        say(std::format("the trial '{}' is not a bootable SubOS; dropped", *config->once));
        config = next;
    }
    mark("select");
    const auto candidates = boot::candidates(*config, [&](std::string_view name) {
        return bootable[std::string(name)];
    });
    for (const auto& candidate : candidates) {
        const auto init = init_of(home, candidate.subos);
        if (!init) recover(init.error());
        if (!init->has_value()) {
            say(std::format("'{}' has no init in its /usr; trying the next entry", candidate.subos));
            log_event(home, {{"event", "boot-skip"}, {"subos", candidate.subos}, {"why", "no init"}});
            if (candidate.via == "once") {
                auto next = *config;
                next.once.reset();
                if (auto saved = boot::save(home.boot_file(), next); !saved) recover(saved.error());
                config = next;
            }
            continue;
        }
        mark("init");
        const auto dir = home.instance(candidate.subos);
        if (auto laid = rootfs::lay_out("/", rootfs::usr_of(dir), home.home); !laid) {
            say(laid.error());
            continue;
        }
        mark("lay_out");
        if (auto etc = luban::machine::fill_machine_etc("/etc", dir); !etc) recover("machine /etc: " + etc.error());
        if (auto users = luban::machine::apply_sysusers("/etc", rootfs::usr_of(dir)); !users) recover("machine /etc: " + users.error());
        mark("machine_etc");
        if (auto cache = library_cache::refresh("/", home, candidate.subos); !cache) recover(cache.error());
        mark("library_cache");
        const auto next = boot::record_boot(*config, candidate);
        if (auto saved = boot::save(home.boot_file(), next); !saved) recover(saved.error());
        mark("record");
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        log_event(home, {{"event", "boot"}, {"subos", candidate.subos}, {"via", candidate.via},
                         {"generation", rootfs::current(dir).value_or(0)}, {"init", (*init)->string()},
                         {"stage0_elapsed_us", elapsed}, {"stage0_phases_us", phases}});
        say(std::format("booting '{}' ({}), generation {}", candidate.subos, candidate.via,
                        rootfs::current(dir).value_or(0)));
        const std::map<std::string, std::string> env{
            {"PATH", "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin"}, {"HOME", "/root"},
            {"TERM", "linux"}, {"XLINGS_HOME", home.home.string()}};
        const int error = platform::exec_path({(*init)->string()}, env);
        say(std::format("{} did not start: {}; trying the next entry", (*init)->string(),
                        platform::error_text(error)));
        log_event(home, {{"event", "boot-failed"}, {"subos", candidate.subos}, {"init", (*init)->string()}});
        config = next;
    }
    recover("nothing could be booted");
}

}  // namespace luban::stage0
