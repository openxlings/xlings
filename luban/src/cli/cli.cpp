module luban.cli;

import std;
import xlings.cli.model;
import xlings.libs.json;
import xlings.platform;
import xlings.platform.stream;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.subos.tools;
import xlings.subos.elevation;

namespace luban::cli {

namespace spec = xlings::cli::spec;
using spec::Level;
namespace fs = std::filesystem;

namespace {

// What each luban command is, in xlings's words: shown at the foot of its
// help, so the way to everything xlings can do starts from here.
const std::map<std::string, std::string, std::less<>>& equivalents() {
    static const std::map<std::string, std::string, std::less<>> value{
        {"new", "xlings subos new <name> --from subos:luban-<edition>"},
        {"enter", "xlings subos use <name>"},
        {"run", "xlings subos exec <name> -- <command...>"},
        {"ls", "xlings subos list"},
        {"status", "xlings subos status <name>"},
        {"config", "xlings subos config <name> --<key> <value>"},
        {"history", "xlings subos rollback <name> --list"},
        {"rollback", "xlings subos rollback <name> [--to <N>]"},
        {"export", "xlings subos export <name> --iso|--drive|--qcow2|--tar|--rootfs <file>"},
        {"rm", "xlings subos remove <name>"},
        {"setup", "xlings self doctor --isolation --fix"},
        {"boot", "xlings subos boot [name] [--once|--fallback|--mark-good|--now]"},
    };
    return value;
}

const std::set<std::string, std::less<>>& official_editions() {
    static const std::set<std::string, std::less<>> value{"nano", "tiny", "core", "agent-workspace"};
    return value;
}

bool flag(std::span<const std::string> args, std::string_view a, std::string_view b = {}) {
    return std::ranges::any_of(args, [&](const std::string& s) { return s == a || (!b.empty() && s == b); });
}

std::string read_first_line(const fs::path& p) {
    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    return line;
}

struct Captured { int code{0}; std::string out; std::string err; };

Captured capture(const std::vector<std::string>& argv) {
    Captured c;
    c.code = xlings::platform::stream::run(argv, [&](std::string_view channel, std::string_view bytes) {
        (channel == "stderr" ? c.err : c.out).append(bytes);
    });
    return c;
}

int usage(std::string_view message, bool json) {
    if (json) std::println("{}", nlohmann::json{{"error", "usage"}, {"message", message}}.dump());
    else std::println(std::cerr, "luban: {}", message);
    return 2;
}

int unknown_command(std::string_view word, bool json) {
    const auto near = spec::suggest(tree(), tree(), word);
    if (json)
        return std::println("{}", nlohmann::json{{"error", "unknown_command"}, {"command", word},
                                                 {"suggestions", near}}.dump()), 2;
    std::string line = std::format("luban: unknown command '{}'", word);
    if (!near.empty()) line += std::format(" -- did you mean `luban {}`?", near.front());
    std::println(std::cerr, "{}\n  luban --help lists the commands", line);
    return 2;
}

// The xlings protocol this luban needs, asked before it is relied on: an
// older xlings is told to update instead of failing halfway through.
std::optional<std::string> handshake(const fs::path& xlings) {
    auto c = capture({xlings.string(), "interface", "--version"});
    if (c.code != 0)
        return std::format("cannot run {} ({}): luban drives xlings", xlings.string(),
                           c.err.empty() ? std::format("exit {}", c.code) : c.err.substr(0, c.err.find('\n')));
    auto j = nlohmann::json::parse(c.out, nullptr, false);
    const auto have = j.is_object() ? j.value("protocol_version", "") : std::string{};
    auto parts = [](std::string_view v) {
        std::array<int, 2> n{0, 0};
        const auto dot = v.find('.');
        std::from_chars(v.data(), v.data() + std::min(dot, v.size()), n[0]);
        if (dot != std::string_view::npos) std::from_chars(v.data() + dot + 1, v.data() + v.size(), n[1]);
        return n;
    };
    if (have.empty() || parts(have) < parts(kMinProtocol))
        return std::format("this xlings speaks interface protocol {}; luban needs {} or newer -- "
                           "update it: xlings self update", have.empty() ? "(unknown)" : have, kMinProtocol);
    return std::nullopt;
}

int overview(bool json) {
    const auto place = where();
    std::vector<nlohmann::json> envs;
    const auto xlings = xlings_path();
    auto listed = capture({xlings.string(), "interface", "list_subos"});
    for (std::string_view rest = listed.out; !rest.empty();) {
        const auto nl = rest.find('\n');
        auto line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_object() && j.value("dataKind", "") == "subos_list")
            for (auto& e : j["payload"]["entries"])
                if (e.value("name", "") != "default") envs.push_back(e);
    }
    const char* inside = std::getenv("XLINGS_ACTIVE_SUBOS");
    if (json) {
        nlohmann::json j{{"place", to_string(place)}, {"environments", envs}};
        if (place == Place::Environment && inside) j["environment"] = inside;
        std::println("{}", j.dump());
        return listed.code;
    }
    switch (place) {
    case Place::Host: std::println("Luban -- on this host"); break;
    case Place::Environment: std::println("Luban -- inside the environment '{}'", inside ? inside : "?"); break;
    case Place::Machine: std::println("Luban -- this machine"); break;
    }
    if (listed.code != 0) {
        std::println("  environments: could not be listed ({})", listed.err.substr(0, listed.err.find('\n')));
        return 1;
    }
    if (envs.empty()) {
        std::println("  no environments yet\n\n  start: luban new <name>          (Luban Core)");
        std::println("         luban new <name> agent-workspace");
        return 0;
    }
    std::println("  environments:");
    for (auto& e : envs)
        std::println("    {:<16} {}", e.value("name", ""), e.value("view", "") == "root" || e.value("kind", "") == "rootfs"
                                                                 ? "Luban root" : "environment");
    std::println("\n  enter: luban enter {}      more: luban --help", envs.front().value("name", ""));
    return 0;
}

int host_status(bool json) {
    auto doctor = capture({xlings_path().string(), "self", "doctor", "--isolation", "--json"});
    auto report = nlohmann::json::parse(doctor.out, nullptr, false);
    const bool ready = report.is_object() && report.value("ok", false);
    if (json) {
        std::println("{}", nlohmann::json{{"place", to_string(where())}, {"ready", ready},
                                          {"isolation", report.is_object() ? report : nlohmann::json()}}.dump());
        return ready ? 0 : 1;
    }
    std::println("Luban -- {}", to_string(where()));
    if (ready) {
        const auto backend = report["backend"].is_object() ? report["backend"].value("name", "") : std::string{};
        std::println("  ready: environments are isolated here ({})", backend);
        return 0;
    }
    std::println("  not ready: this machine needs a one-time setup before it can run a Luban root");
    std::println("  do it: luban setup     (details: xlings self doctor --isolation)");
    return 1;
}

}  // namespace

const spec::CommandSpec& tree() {
    using spec::CommandSpec;
    static const CommandSpec value{
        .name = "luban",
        .description = "Luban: environments and the system they run on",
        .options = {
            {"--json", "Machine-readable output", true},
            {"--agent", "For an agent: plain output, never a question", true, Level::More},
            {"-y, --yes", "Answer yes to confirmations", true, Level::More},
            {"-v, --verbose", "More detail", true, Level::Expert},
            {"-q, --quiet", "Less output", true, Level::Expert},
            {"-h, --help", "This help"},
            {"--version", "The luban version"},
        },
        .children = {
            {.name = "new", .description = "Make an environment (edition: core by default)",
             .arguments = {{"name", "Its name", true}, {"edition", "core, tiny, nano, agent-workspace, or ns:name"}},
             .options = {{"--from <EDITION>", "The edition, as an option", false, Level::More},
                         {"--proxy <URL>", "Its network goes only through this SOCKS5h proxy", false, Level::More},
                         {"--carrier <CARRIER>", "Where it runs: local, wsl2, vz", false, Level::Expert}}},
            {.name = "enter", .description = "Go into an environment", .arguments = {{"name", "Which", true}}},
            {.name = "run", .description = "Run one command in an environment",
             .arguments = {{"name", "Which", true}, {"command", "The command and its arguments, after --", true, true}}},
            {.name = "ls", .description = "List the environments", .aliases = {"list"}},
            {.name = "status", .description = "This host, or one environment", .arguments = {{"name", "Which"}}},
            {.name = "config", .description = "Show or set an environment's settings (proxy, tz, policy, allow, mount)",
             .arguments = {{"name", "Which", true}, {"key", "proxy, tz, policy, allow, disallow, mount, unmount, fetch"},
                           {"value", "The new value"}},
             .level = Level::More},
            {.name = "history", .description = "An environment's generations", .arguments = {{"name", "Which"}},
             .level = Level::More},
            {.name = "rollback", .description = "Go back a generation", .arguments = {{"name", "Which"}},
             .options = {{"--to <N>", "This generation"}}, .level = Level::More},
            {.name = "export", .description = "Make an image: .iso, .img, .qcow2, .tar(.zst|.gz), or a directory",
             .arguments = {{"name", "Which", true}, {"file", "Where; the format follows the name", true}},
             .options = {{"--format <FORMAT>", "iso, img, qcow2, tar or dir, whatever the name says"},
                         {"--size <SIZE>", "Disk size (img, qcow2)"}},
             .level = Level::More},
            {.name = "write", .description = "Make a boot drive from an image or an environment",
             .arguments = {{"source", "An image file, or an environment", true}, {"device", "The drive, e.g. /dev/sdb", true}},
             .options = {{"--serial <SERIAL>", "The drive's serial (required with --agent)"}},
             .level = Level::More},
            {.name = "try", .description = "Boot an environment or an image in a local virtual machine",
             .arguments = {{"source", "An environment or an image file", true}},
             .options = {{"--proxy <URL>", "Its only network is this proxy (a private machine)"},
                         {"--memory <MB>", "Its memory (default 2048)"},
                         {"--nographic", "Its console in this terminal"}},
             .level = Level::More},
            {.name = "rm", .description = "Remove an environment (asks first)", .aliases = {"remove"},
             .arguments = {{"name", "Which", true}}, .level = Level::More},
            {.name = "setup", .description = "This host's one-time setup (isolation)", .level = Level::More},
            {.name = "help", .description = "Help; --all for every command",
             .options = {{"--all", "Every command"}, {"--expert", "And the expert ones"}}, .level = Level::More},
            {.name = "boot", .description = "This machine's boot entries", .arguments = {{"name", "An environment"}},
             .options = {{"--once", "Only the next boot (a trial)"}, {"--fallback", "When the default fails"},
                         {"--mark-good", "This boot worked"}, {"--now", "Switch to it now"}},
             .level = Level::Expert},
        },
    };
    return value;
}

Place where() {
    if (const char* mode = std::getenv("XLINGS_SUBOS_MODE"); mode && *mode) return Place::Environment;
    std::error_code ec;
    if (fs::exists("/etc/xlings/root.json", ec)) return Place::Machine;
    const auto init = read_first_line("/proc/1/cmdline");
    if (init.find("luban-init") != std::string::npos || init.find("xlings-init") != std::string::npos)
        return Place::Machine;
    return Place::Host;
}

std::string_view to_string(Place p) {
    switch (p) {
    case Place::Host: return "host";
    case Place::Environment: return "environment";
    case Place::Machine: return "machine";
    }
    return "host";
}

std::string edition_ref(std::string_view edition) {
    if (edition.empty()) return "subos:luban-core";
    if (edition.find(':') != std::string_view::npos) return std::string(edition);
    const auto at = edition.find('@');
    const auto name = edition.substr(0, at);
    const auto version = at == std::string_view::npos ? std::string_view{} : edition.substr(at);
    if (official_editions().contains(name)) return std::format("subos:luban-{}{}", name, version);
    if (name.starts_with("luban-")) return std::format("subos:{}{}", name, version);
    return std::string(edition);
}

std::string default_carrier() {
    if constexpr (xlings::platform::is_windows) return "wsl2";
    else if constexpr (xlings::platform::is_macos) return "vz";
    else return {};
}

std::string export_format(std::string_view file) {
    const auto ends = [&](std::string_view s) { return file.ends_with(s); };
    if (ends(".iso")) return "iso";
    if (ends(".img") || ends(".raw")) return "img";
    if (ends(".qcow2")) return "qcow2";
    if (ends(".tar") || ends(".tar.gz") || ends(".tgz") || ends(".tar.zst") || ends(".tar.xz")) return "tar";
    if (ends("/")) return "dir";
    std::error_code ec;
    if (fs::is_directory(fs::path(file), ec)) return "dir";
    return {};
}

std::expected<std::vector<std::string>, std::string> to_xlings(std::span<const std::string> args) {
    if (args.empty()) return std::unexpected("no command");
    const auto& cmd = args.front();
    std::vector<std::string> positional, options;
    std::vector<std::string> after;   // after `--`
    std::map<std::string, std::string, std::less<>> values;
    static const std::set<std::string, std::less<>> takes{"--from", "--proxy", "--carrier", "--to", "--format",
                                                           "--size", "--serial"};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--") { after.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end()); break; }
        if (a.starts_with("--") && a.find('=') != std::string::npos) {
            values[a.substr(0, a.find('='))] = a.substr(a.find('=') + 1);
            continue;
        }
        if (takes.contains(a)) {
            if (i + 1 >= args.size()) return std::unexpected(std::format("{} needs a value", a));
            values[a] = args[++i];
            continue;
        }
        if (a.starts_with('-')) { options.push_back(a); continue; }
        positional.push_back(a);
    }
    const auto need = [&](std::size_t n, std::string_view what) -> std::optional<std::string> {
        if (positional.size() < n) return std::format("`luban {}` needs {}", cmd, what);
        return std::nullopt;
    };
    const auto value = [&](std::string_view k) -> std::optional<std::string> {
        if (auto it = values.find(k); it != values.end()) return it->second;
        return std::nullopt;
    };
    std::vector<std::string> x;
    if (cmd == "new") {
        if (auto e = need(1, "a name")) return std::unexpected(*e);
        if (positional.size() > 2) return std::unexpected("`luban new <name> [edition]`: one name, one edition");
        auto edition = value("--from").value_or(positional.size() > 1 ? positional[1] : "core");
        x = {"subos", "new", positional[0], "--from", edition_ref(edition)};
        if (auto p = value("--proxy")) x.insert(x.end(), {"--proxy", *p});
        // The same command everywhere (part 3 §5): a Luban edition is a Linux
        // root, so on a Windows or macOS host it runs on that platform's
        // carrier -- a WSL2 distribution of the home's own, a vz machine.
        if (auto c = value("--carrier")) x.insert(x.end(), {"--carrier", *c});
        else if (const auto carrier = default_carrier(); !carrier.empty()) x.insert(x.end(), {"--carrier", carrier});
    } else if (cmd == "enter") {
        if (auto e = need(1, "a name")) return std::unexpected(*e);
        x = {"subos", "use", positional[0]};
    } else if (cmd == "run") {
        if (auto e = need(1, "a name")) return std::unexpected(*e);
        auto command = after;
        if (command.empty()) command.assign(positional.begin() + 1, positional.end());
        if (command.empty()) return std::unexpected("`luban run <name> -- <command...>`: no command");
        x = {"subos", "exec", positional[0], "--"};
        x.insert(x.end(), command.begin(), command.end());
    } else if (cmd == "ls" || cmd == "list") {
        x = {"subos", "list"};
    } else if (cmd == "status") {
        if (auto e = need(1, "a name")) return std::unexpected(*e);
        x = {"subos", "status", positional[0]};
    } else if (cmd == "config") {
        if (auto e = need(1, "a name")) return std::unexpected(*e);
        x = {"subos", "config", positional[0]};
        if (positional.size() == 2)
            return std::unexpected(std::format("`luban config {} {}` needs a value; `luban config {}` shows them all",
                                               positional[0], positional[1], positional[0]));
        if (positional.size() >= 3) {
            const auto& key = positional[1];
            const auto& v = positional[2];
            if (key == "proxy") {
                if (v == "off" || v == "none") x.insert(x.end(), {"--net", "host"});
                else x.insert(x.end(), {"--net", "proxy", "--proxy", v});
            } else if (key == "policy") x.insert(x.end(), {"--sandbox", v});
            else if (key == "tz" || key == "allow" || key == "disallow" || key == "mount" || key == "unmount"
                     || key == "fetch" || key == "observe")
                x.insert(x.end(), {"--" + key, v});
            else
                return std::unexpected(std::format("no setting '{}': proxy, tz, policy, allow, disallow, mount, "
                                                   "unmount, fetch, observe", key));
        }
    } else if (cmd == "history" || cmd == "rollback") {
        std::string name = positional.empty() ? std::string{} : positional[0];
        if (name.empty())
            if (const char* inside = std::getenv("XLINGS_ACTIVE_SUBOS"); inside && *inside) name = inside;
        if (name.empty()) return std::unexpected(std::format("`luban {} <name>`: which environment?", cmd));
        x = {"subos", "rollback", name};
        if (cmd == "history") x.push_back("--list");
        else if (auto to = value("--to")) x.insert(x.end(), {"--to", *to});
    } else if (cmd == "export") {
        if (auto e = need(2, "a name and a file")) return std::unexpected(*e);
        auto format = value("--format").value_or(export_format(positional[1]));
        if (format.empty())
            return std::unexpected(std::format("cannot tell the format from '{}': name it .iso, .img, .qcow2, .tar, "
                                               ".tar.zst or end it with / (a directory), or give --format", positional[1]));
        static const std::map<std::string, std::string, std::less<>> flag_of{
            {"iso", "--iso"}, {"img", "--drive"}, {"qcow2", "--qcow2"}, {"tar", "--tar"}, {"dir", "--rootfs"}};  // tool-ok: format names, not programs
        auto f = flag_of.find(format);
        if (f == flag_of.end()) return std::unexpected(std::format("no format '{}': iso, img, qcow2, tar, dir", format));
        x = {"subos", "export", positional[0], f->second, positional[1]};
        if (auto s = value("--size")) x.insert(x.end(), {"--size", *s});
    } else if (cmd == "rm" || cmd == "remove") {
        if (auto e = need(1, "a name")) return std::unexpected(*e);
        x = {"subos", "remove", positional[0]};
    } else if (cmd == "setup") {
        x = {"self", "doctor", "--isolation", "--fix"};
    } else if (cmd == "boot") {
        x = {"subos", "boot"};
        x.insert(x.end(), positional.begin(), positional.end());
    } else {
        return std::unexpected("");
    }
    // Options luban does not take itself go to xlings as they are.
    for (const auto& o : options) x.push_back(o);
    return x;
}

fs::path xlings_path() {
    if (const char* forced = std::getenv("LUBAN_XLINGS"); forced && *forced) return forced;
    const auto exe = std::string(xlings::platform::exe_suffix);
    std::error_code ec;
    if (auto self = xlings::platform::get_executable_path(); !self.empty()) {
        const auto sibling = fs::path(self).parent_path() / ("xlings" + exe);
        if (fs::is_regular_file(sibling, ec)) return sibling;
    }
    if (const char* home = std::getenv("XLINGS_HOME"); home && *home) {
        const auto entry = fs::path(home) / "bin" / ("xlings" + exe);
        if (fs::exists(entry, ec)) return entry;
    }
    return "xlings" + exe;
}

namespace {

std::string trim_(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    return s;
}

fs::path home_dir_() {
    if (const char* h = std::getenv("XLINGS_HOME"); h && *h) return h;
    if (const char* u = std::getenv("HOME"); u && *u) return fs::path(u) / ".xlings";
    return ".xlings";
}

std::string human_size_(std::uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1000 && u < 4) { v /= 1000; ++u; }
    return std::format("{:.1f} {}", v, units[u]);
}

// One option's value from luban's argv (`--x v` or `--x=v`).
std::optional<std::string> option_(std::span<const std::string> args, std::string_view name) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name && i + 1 < args.size()) return args[i + 1];
        if (args[i].starts_with(std::string(name) + "=")) return args[i].substr(name.size() + 1);
    }
    return std::nullopt;
}

std::vector<std::string> positionals_(std::span<const std::string> args, std::set<std::string, std::less<>> valued) {
    std::vector<std::string> out;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (valued.contains(args[i])) { ++i; continue; }
        if (args[i].starts_with('-')) continue;
        out.push_back(args[i]);
    }
    return out;
}

// An environment's image in a temporary file, made by xlings; removed with
// this object.
struct Exported {
    fs::path file;
    ~Exported() { if (!file.empty()) { std::error_code ec; fs::remove(file, ec); } }
};

std::expected<fs::path, int> source_image_(const std::string& source, std::string_view flag, std::string_view ext,
                                           Exported& keep) {
    std::error_code ec;
    if (fs::is_regular_file(source, ec)) return fs::absolute(source);
    keep.file = fs::temp_directory_path() / std::format("luban-{}-{}{}", source, xlings::platform::get_pid(), ext);
    std::println(std::cerr, "luban: making an image of '{}' ...", source);
    const int rc = xlings::platform::run_argv({xlings_path().string(), "subos", "export", source,
                                              std::string(flag), keep.file.string()});
    if (rc != 0) return std::unexpected(rc);
    return keep.file;
}

// The raw copy, run by `luban write` itself or -- when the drive is not
// writable -- through the one door for administrator rights.
int raw_write_(const fs::path& from, const fs::path& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::in | std::ios::out);
    if (!in || !out) {
        std::println(std::cerr, "luban: cannot open {} for writing", to.string());
        return 1;
    }
    std::error_code ec;
    const auto total = fs::file_size(from, ec);
    std::vector<char> buffer(4u << 20);
    std::uintmax_t done = 0;
    int shown = -1;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = in.gcount();
        if (n <= 0) break;
        out.write(buffer.data(), n);
        if (!out) { std::println(std::cerr, "luban: writing {} failed", to.string()); return 1; }
        done += static_cast<std::uintmax_t>(n);
        const int pct = total ? static_cast<int>(done * 100 / total) : 100;
        if (pct / 10 != shown / 10) { std::print(std::cerr, "\r  {:3}%  {}", pct, human_size_(done)); shown = pct; }
    }
    out.flush();
    out.close();
    if (!xlings::platform::sync_file(to)) { std::println(std::cerr, "\nluban: {} did not flush", to.string()); return 1; }
    std::println(std::cerr, "\r  100%  {} written and flushed", human_size_(done));
    return 0;
}

int write_cmd_(std::span<const std::string> rest, bool json, bool agent, bool yes) {
    if constexpr (!xlings::platform::is_linux) {
        std::println(std::cerr, "luban: writing a drive is Linux-only in this version -- `luban export <name> x.iso` "
                                "here, then write the ISO with this system's own drive writer");
        return 1;
    }
    const auto pos = positionals_(rest, {"--serial"});
    if (pos.size() != 2) return usage("`luban write <image or environment> <drive>`", json);
    auto drive = inspect_drive(pos[1]);
    if (!drive.refused.empty()) {
        if (json) std::println("{}", nlohmann::json{{"error", "drive"}, {"device", pos[1]}, {"message", drive.refused}}.dump());
        else std::println(std::cerr, "luban: {}: {}", pos[1], drive.refused);
        return 1;
    }
    // An environment becomes a drive that boots it (and keeps what it
    // writes); an image file is written as it is.
    Exported keep;
    std::error_code ec;
    const bool file = fs::is_regular_file(pos[0], ec);
    const std::string summary = std::format("{} ({}, {}{})", drive.dev.string(), drive.model.empty() ? "drive" : drive.model,
                                            human_size_(drive.bytes),
                                            drive.serial != drive.name ? ", serial " + drive.serial : std::string{});
    // Asked before anything is made: the drive is erased.
    const bool can_ask = !agent && xlings::platform::stdin_is_terminal();
    const auto given = option_(rest, "--serial");
    if (!can_ask || yes) {
        if (!yes || !given || *given != drive.serial) {
            const auto cmd = std::format("luban write {} {} -y --serial {}", pos[0], pos[1], drive.serial);
            if (json) std::println("{}", nlohmann::json{{"error", "confirm"}, {"message", "erases " + summary},
                                                        {"command", cmd}}.dump());
            else std::println(std::cerr, "luban: this erases {}; to go ahead without a question:\n  {}", summary, cmd);
            return 2;
        }
    } else {
        std::println("this erases everything on {}", summary);
        std::print("type the drive's name ({}) to go ahead: ", drive.name);
        std::string answer;
        std::getline(std::cin, answer);
        if (trim_(answer) != drive.name) { std::println("nothing written"); return 1; }
    }
    auto image = source_image_(pos[0], "--drive", ".img", keep);
    if (!image) return image.error();
    const auto size = fs::file_size(*image, ec);
    if (size > drive.bytes) {
        std::println(std::cerr, "luban: the image ({}) is larger than {} ({})", human_size_(size), drive.dev.string(),
                     human_size_(drive.bytes));
        return 1;
    }
    std::println("writing {} to {} ...", file ? pos[0] : "'" + pos[0] + "'", drive.dev.string());
    std::ofstream probe(drive.dev, std::ios::binary | std::ios::in | std::ios::out);
    if (probe) {
        probe.close();
        return raw_write_(*image, drive.dev);
    }
    // Not this user's to write: the one door, recorded.
    const auto self = xlings::platform::get_executable_path();
    return xlings::subos::elevation::run(xlings::subos::HomeView{home_dir_()},
                                         {self.string(), "__write", image->string(), drive.dev.string()},
                                         "luban write: a boot drive");
}

// One connection of a private machine to its proxy: qemu runs this for each
// connection the guest makes to 10.0.2.100:1080 (guestfwd ...-cmd:), with the
// connection on stdin/stdout; it relays to the proxy and back. A proxy that
// is down refuses that connection -- the machine has no other way out.
int pipe_(const std::string& host, const std::string& port_text) {
    namespace net = xlings::platform::network;
    std::uint16_t port = 0;
    std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    auto addresses = net::resolve_proxy(host, port);
    if (!addresses || addresses->empty()) return 1;
    auto connecting = net::connect_to(addresses->front());
    if (!connecting) return 1;
    const int proxy = connecting->fd;
    for (int i = 0; !connecting->ready; ++i) {
        auto done = net::connected(proxy);
        if (!done || (*done && !**done) || i > 5000) return 1;
        if (*done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto send_all = [](int fd, std::string_view bytes) {
        while (!bytes.empty()) {
            auto n = net::send(fd, bytes);
            if (!n) return false;
            if (!*n) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            bytes.remove_prefix(**n);
        }
        return true;
    };
    std::array<char, 65536> buffer{};
    bool in_open = true;
    for (;;) {
        bool moved = false;
        if (in_open) {
            auto r = net::receive(0, buffer);
            if (!r) return 0;
            if (*r) {
                if (**r == 0) { in_open = false; net::shutdown_write(proxy); }
                else if (!send_all(proxy, std::string_view(buffer.data(), **r))) return 0;
                moved = true;
            }
        }
        auto r = net::receive(proxy, buffer);
        if (!r) return 0;
        if (*r) {
            if (**r == 0) return 0;
            if (!send_all(1, std::string_view(buffer.data(), **r))) return 0;
            moved = true;
        }
        if (!moved) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int try_cmd_(std::span<const std::string> rest, bool json) {
    const auto pos = positionals_(rest, {"--proxy", "--memory"});
    if (pos.size() != 1) return usage("`luban try <environment or image>`", json);
    std::string arch = "x86_64";
    if (auto m = read_first_line("/proc/sys/kernel/arch"); !m.empty()) arch = m;
    if (arch != "x86_64")
        return usage(std::format("`luban try` runs x86_64 images in this version (this machine is {})", arch), json);
    const xlings::subos::HomeView home{home_dir_()};
    auto qemu = xlings::subos::tools::first("qemu-system-x86_64", home, xlings::subos::Ports{});
    if (!qemu) {
        std::println(std::cerr, "luban: trying needs qemu -- {}", xlings::subos::tools::install_hint("qemu-system-x86_64"));
        return 1;
    }
    Exported keep;
    auto image = source_image_(pos[0], "--iso", ".iso", keep);
    if (!image) return image.error();
    std::vector<std::string> q{qemu->bin.string(), "-m", option_(rest, "--memory").value_or("2048"), "-smp", "2"};
    std::error_code ec;
    // A character device this user may open, never a file made by asking.
    if (std::fstream kvm; fs::is_character_file("/dev/kvm", ec)
        && (kvm.open("/dev/kvm", std::ios::in | std::ios::out), kvm.is_open()))
        q.insert(q.end(), {"-enable-kvm", "-cpu", "host"});
    else std::println(std::cerr, "luban: no KVM here -- it runs, slowly (emulated)");
    const auto ext = image->extension().string();
    if (ext == ".iso") q.insert(q.end(), {"-cdrom", image->string(), "-boot", "d"});
    else q.insert(q.end(), {"-drive", std::format("file={},format={},if=virtio", image->string(),
                                                  ext == ".qcow2" ? "qcow2" : "raw")});
    // Its network: the machine's NAT, or -- for a private one -- nothing but
    // the proxy (Luban design §C5 L3): restrict=on keeps every other route
    // out, and the guest reaches the proxy at 10.0.2.100:1080.
    if (auto proxy = option_(rest, "--proxy")) {
        const auto at = proxy->find("://");
        const auto endpoint = at == std::string::npos ? *proxy : proxy->substr(at + 3);
        const auto colon = endpoint.rfind(':');
        if (colon == std::string::npos) return usage("--proxy socks5h://HOST:PORT", json);
        const auto self = xlings::platform::get_executable_path().string();
        if (self.find(',') != std::string::npos) return usage("luban's own path has a comma; qemu cannot run it", json);
        q.insert(q.end(), {"-nic", std::format("user,restrict=on,guestfwd=tcp:10.0.2.100:1080-cmd:{} __pipe {} {}", self,
                                               endpoint.substr(0, colon), endpoint.substr(colon + 1))});
        std::println(std::cerr, "luban: its only network is the proxy, at socks5h://10.0.2.100:1080 inside");
    } else {
        q.insert(q.end(), {"-nic", "user"});
    }
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if (flag(rest, "--nographic") || !((display && *display) || (wayland && *wayland))) {
        q.push_back("-nographic");
        std::println(std::cerr, "luban: its console is this terminal; Ctrl-A X leaves");
    }
    return xlings::platform::run_argv(q);
}

}  // namespace

Drive inspect_drive(const fs::path& dev, const fs::path& sys, const fs::path& mounts) {
    Drive d;
    d.dev = dev;
    d.name = dev.filename().string();
    std::error_code ec;
    const auto block = sys / "class" / "block" / d.name;
    if (dev.parent_path() != "/dev" || !fs::exists(block, ec)) { d.refused = "not a drive this machine has"; return d; }
    if (fs::exists(block / "partition", ec)) {
        d.refused = "a partition, not a drive -- name the whole drive";
        return d;
    }
    d.bytes = 512 * static_cast<std::uintmax_t>(std::strtoull(read_first_line(block / "size").c_str(), nullptr, 10));
    d.model = trim_(read_first_line(block / "device" / "model"));
    d.serial = trim_(read_first_line(block / "device" / "serial"));
    if (d.serial.empty()) d.serial = d.name;
    // In use: it, or one of its partitions, is mounted or held (LVM, RAID,
    // dm-crypt) -- which is how the drive this system runs from is refused.
    std::vector<std::string> names{d.name};
    for (fs::directory_iterator it(block, ec), end; !ec && it != end; it.increment(ec))
        if (fs::exists(it->path() / "partition")) names.push_back(it->path().filename().string());
    for (const auto& n : names) {
        const auto holders = (n == d.name ? block : block / n) / "holders";
        if (fs::exists(holders, ec) && !fs::is_empty(holders, ec)) {
            d.refused = std::format("in use ({} is held by another device)", n);
            return d;
        }
    }
    std::ifstream table(mounts);
    for (std::string line; std::getline(table, line);) {
        const auto source = line.substr(0, line.find(' '));
        if (!source.starts_with("/dev/")) continue;
        auto real = source;
        if (auto c = fs::weakly_canonical(source, ec); !ec) real = c.string();
        const auto name = fs::path(real).filename().string();
        if (std::ranges::find(names, name) != names.end()) {
            const auto rest = line.substr(line.find(' ') + 1);
            d.refused = std::format("mounted ({} on {})", source, rest.substr(0, rest.find(' ')));
            return d;
        }
    }
    if (d.bytes == 0) d.refused = "it reports no size (no medium?)";
    return d;
}

int run(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);
    // Global options, wherever they are (before `--`).
    bool json = false, agent = false, yes = false, verbose = false, quiet = false;
    std::vector<std::string> rest;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--") { rest.insert(rest.end(), args.begin() + static_cast<std::ptrdiff_t>(i), args.end()); break; }
        if (a == "--json") json = true;
        else if (a == "--agent") agent = true;
        else if (a == "-y" || a == "--yes") yes = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "-q" || a == "--quiet") quiet = true;
        else rest.push_back(a);
    }
    if (const char* mode = std::getenv("XLINGS_AGENT_MODE"); mode && std::string_view(mode) == "1") agent = true;

    if (rest.empty()) return overview(json);
    const auto& cmd = rest.front();
    // Internal: the raw copy, as `luban write` runs it with administrator rights.
    if (cmd == "__write" && rest.size() == 3) return raw_write_(rest[1], rest[2]);
    // Internal: a private machine's connection to its proxy (`luban try --proxy`).
    if (cmd == "__pipe" && rest.size() == 3) return pipe_(rest[1], rest[2]);
    if (cmd == "--version" || cmd == "-V") {
        std::println("luban {}", kVersion);
        return 0;
    }
    const auto* node = spec::find_in(tree(), std::array<std::string_view, 1>{cmd});
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        const bool all = flag(rest, "--all"), expert = flag(rest, "--expert");
        std::print("{}", spec::render_help(tree(), tree(), "luban", expert ? Level::Expert : all ? Level::More : Level::Common));
        if (!all && !expert) std::println("install software: xlings install <package>");
        return 0;
    }
    if (!node) return unknown_command(cmd, json);
    if (flag(rest, "--help", "-h")) {
        std::print("{}", spec::render_help(tree(), *node, "luban " + node->name, Level::Expert));
        if (auto it = equivalents().find(node->name); it != equivalents().end())
            std::println("\nin xlings: {}", it->second);
        return 0;
    }
    if (node->name == "status" && rest.size() == 1) return host_status(json);
    if (node->name == "write") return write_cmd_(rest, json, agent, yes);
    if (node->name == "try") return try_cmd_(rest, json);

    std::vector<std::string> forwarded(rest.begin(), rest.end());
    forwarded.front() = node->name;
    auto mapped = to_xlings(forwarded);
    if (!mapped) return mapped.error().empty() ? unknown_command(cmd, json) : usage(mapped.error(), json);

    const auto xlings = xlings_path();
    if (auto refused = handshake(xlings)) {
        if (json) std::println("{}", nlohmann::json{{"error", "xlings"}, {"message", *refused}}.dump());
        else std::println(std::cerr, "luban: {}", *refused);
        return 1;
    }
    // The global options go after the command they modify -- `subos remove`
    // reads its own -y -- and before a `--`, after which every word is the
    // user's command line.
    std::vector<std::string> globals;
    if (agent) globals.push_back("--agent");
    if (yes) globals.push_back("-y");
    if (verbose) globals.push_back("-v");
    if (quiet) globals.push_back("-q");
    if (json && (node->name == "status" || node->name == "config")) globals.push_back("--json");
    auto x = *mapped;
    x.insert(std::ranges::find(x, std::string("--")), globals.begin(), globals.end());
    x.insert(x.begin(), xlings.string());
    return xlings::platform::run_argv(x);
}

}  // namespace luban::cli
