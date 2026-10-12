module luban.cli;

import std;
import xlings.cli.model;
import xlings.libs.json;
import xlings.observe;
import xlings.platform;
import xlings.platform.stream;
import xlings.subos.home_view;
import xlings.subos.elevation;
import luban.service;
import luban.vm;

namespace luban::cli {

namespace spec = xlings::cli::spec;
namespace svc = luban::service;
using spec::Level;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

// What each luban command is in xlings's words, at the foot of its help: the
// way to everything xlings can do starts from here.
const std::map<std::string, std::string, std::less<>>& equivalents() {
    static const std::map<std::string, std::string, std::less<>> value{
        {"new", "xlings subos new <name> --from subos:luban-<edition>"},
        {"enter", "xlings subos use <name> [--persist home|none]"},
        {"run", "xlings subos exec <name> -- <command...>"},
        {"ls", "xlings subos list"},
        {"status", "xlings subos status <name>"},
        {"config", "xlings subos config <name> --<key> <value> (isolation)"},
        {"stop", "xlings subos stop <name>"},
        {"history", "xlings subos rollback <name> --list"},
        {"upgrade", "xlings subos upgrade <name> [--to <version>] [--dry-run] [-y]"},
        {"rollback", "xlings subos rollback <name> [--to <N>]"},
        {"export", "xlings subos export <name> --tar|--rootfs <file> (images: luban's)"},
        {"rm", "xlings subos remove <name>"},
        {"setup", "xlings self doctor --isolation --fix"},
    };
    return value;
}

std::string read_first_line(const fs::path& p) {
    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    return line;
}

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

struct Globals {
    bool json { false };
    bool agent { false };
    bool yes { false };
    bool verbose { false };
    bool quiet { false };
};

// ── The terminal's rendering of the service's events (design §5.2) ─────
//
// One line per phase: `▸` while it runs (rewritten in place on a terminal,
// with its progress), `✓` / `✗` when it ends. A step's own log lines are
// shown with -v only; warnings and errors always, errors with their way out.
// --json prints the events as they are.
class Renderer {
public:
    explicit Renderer(const Globals& g)
        : g_(g),
          tty_(!g.json && !g.agent && xlings::platform::stdout_is_terminal()),
          color_(tty_ && !std::getenv("NO_COLOR")) {}
    ~Renderer() { close_(); }

    void operator()(const svc::Event& e) {
        using K = svc::Event::Kind;
        if (g_.json) {
            Json j{{"event", kind_(e.kind)}};
            if (!e.phase.empty()) j["phase"] = e.phase;
            if (!e.state.empty()) j["state"] = e.state;
            if (!e.text.empty()) j["text"] = e.text;
            if (!e.hint.empty()) j["hint"] = e.hint;
            if (!e.level.empty()) j["level"] = e.level;
            if (e.percent >= 0) j["percent"] = e.percent;
            if (!e.data_kind.empty()) { j["data_kind"] = e.data_kind; j["data"] = e.data; }
            std::println("{}", j.dump());
            return;
        }
        switch (e.kind) {
        case K::Phase:
            if (e.state == "begin") {
                close_();
                open_ = e.text;
                if (tty_) std::print("{} {}", paint_("▸", 36), open_), std::cout.flush();
                else if (!g_.quiet) std::println("▸ {}", open_);
            } else {
                const bool ok = e.state == "done";
                const auto text = e.text.empty() ? open_ : e.text;
                if (tty_) std::print("\r\033[K");
                if (!ok || !g_.quiet) std::println("{} {}", ok ? paint_("✓", 32) : paint_("✗", 31), text);
                open_.clear();
            }
            break;
        case K::Progress:
            if (tty_ && !open_.empty()) {
                std::string detail = e.text;
                if (detail.size() > 60) detail = detail.substr(0, 57) + "...";
                std::print("\r\033[K{} {}{}  {}", paint_("▸", 36), open_,
                           e.percent >= 0 ? std::format("  {:3.0f}%", e.percent * 100) : std::string{}, paint_(detail, 2));
                std::cout.flush();
            }
            break;
        case K::Log:
            if (e.level == "warn") above_(std::format("{} {}", paint_("!", 33), e.text));
            else if (e.level == "error") above_(std::format("{} {}", paint_("✗", 31), e.text));
            else if (g_.verbose) above_(paint_("  " + e.text, 2));
            break;
        case K::Notice:
            above_(std::format("  {}", e.text));
            if (!e.hint.empty()) above_(std::format("    {}", paint_(e.hint, 1)));
            break;
        case K::Error:
            close_();
            std::println(std::cerr, "{} {}", paint_("✗", 31), e.text);
            if (!e.hint.empty()) std::println(std::cerr, "    {}", paint_(e.hint, 1));
            break;
        case K::Data:
            break;
        }
    }

    // A conclusion line, after the phases.
    void say(std::string_view text) {
        close_();
        if (!g_.json) std::println("{}", text);
    }
    void next(std::string_view label, std::string_view command) {
        close_();
        if (!g_.json && !g_.quiet) std::println("  {:<8} {}", label, paint_(command, 1));
    }
    void value(const Json& j) {
        if (g_.json) std::println("{}", Json{{"result", j}}.dump());
    }
    std::string paint(std::string_view s, int code) const { return paint_(s, code); }

private:
    static std::string kind_(svc::Event::Kind k) {
        switch (k) {
        case svc::Event::Kind::Phase: return "phase";
        case svc::Event::Kind::Progress: return "progress";
        case svc::Event::Kind::Log: return "log";
        case svc::Event::Kind::Notice: return "notice";
        case svc::Event::Kind::Error: return "error";
        case svc::Event::Kind::Data: return "data";
        }
        return "log";
    }
    std::string paint_(std::string_view s, int code) const {
        if (!color_) return std::string(s);
        return std::format("\033[{}m{}\033[0m", code, s);
    }
    // A line above the running phase.
    void above_(std::string_view line) {
        if (tty_ && !open_.empty()) {
            std::print("\r\033[K{}\n{} {}", line, paint_("▸", 36), open_);
            std::cout.flush();
        } else {
            std::println("{}", line);
        }
    }
    void close_() {
        if (tty_ && !open_.empty()) {
            std::println("");
            open_.clear();
        }
    }
    const Globals& g_;
    bool tty_;
    bool color_;
    std::string open_;
};

int usage(std::string_view message, const Globals& g) {
    if (g.json) std::println("{}", Json{{"error", "usage"}, {"message", message}}.dump());
    else std::println(std::cerr, "luban: {}", message);
    return 2;
}

int unknown_command(std::string_view word, const Globals& g) {
    const auto near = spec::suggest(tree(), tree(), word);
    if (g.json)
        return std::println("{}", Json{{"error", "unknown_command"}, {"command", word}, {"suggestions", near}}.dump()), 2;
    std::string line = std::format("luban: unknown command '{}'", word);
    if (!near.empty()) line += std::format(" -- did you mean `luban {}`?", near.front());
    std::println(std::cerr, "{}\n  luban --help lists the commands", line);
    return 2;
}

bool ask_(std::string_view question) {
    std::print("{} [y/N] ", question);
    std::cout.flush();
    std::string answer;
    std::getline(std::cin, answer);
    answer = trim_(answer);
    return answer == "y" || answer == "Y" || answer == "yes";
}

bool can_ask_(const Globals& g) { return !g.agent && !g.json && xlings::platform::stdin_is_terminal(); }

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

// An environment's image in a temporary file; removed with this object.
struct Exported {
    fs::path file;
    ~Exported() { if (!file.empty()) { std::error_code ec; fs::remove(file, ec); } }
};

std::expected<fs::path, int> source_image_(const svc::Context& c, const std::string& source, std::string_view ext,
                                           Exported& keep) {
    std::error_code ec;
    if (fs::is_regular_file(source, ec)) return fs::absolute(source);
    keep.file = fs::temp_directory_path() / std::format("luban-{}-{}{}", source, xlings::platform::get_pid(), ext);
    auto out = svc::export_environment(c, {.name = source, .file = keep.file});
    if (!out.ok()) return std::unexpected(out.code);
    return keep.file;
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

std::optional<std::string> option_(std::span<const std::string> args, std::string_view name) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name && i + 1 < args.size()) return args[i + 1];
        if (args[i].starts_with(std::string(name) + "=")) return args[i].substr(name.size() + 1);
    }
    return std::nullopt;
}

int write_cmd_(const svc::Context& c, std::span<const std::string> rest, const Globals& g) {
    const bool json = g.json, agent = g.agent, yes = g.yes;
    if constexpr (!xlings::platform::is_linux) {
        std::println(std::cerr, "luban: writing a drive is Linux-only in this version -- `luban export <name> x.iso` "
                                "here, then write the ISO with this system's own drive writer");
        return 1;
    }
    const auto pos = positionals_(rest, {"--serial"});
    if (pos.size() != 2) return usage("`luban write <image or environment> <drive>`", g);
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
    auto image = source_image_(c, pos[0], ".img", keep);
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

// One connection of a machine's sound to this host's PipeWire / PulseAudio
// socket: qemu runs this per connection (guestfwd ...-cmd:), the connection on
// stdin/stdout.
int pipe_unix_(const std::string& socket) {
    namespace platform = xlings::platform;
    const int fd = platform::unix_connect_stream(socket);
    if (fd < 0) return 1;
    std::array<char, 65536> buf{};
    bool in_open = true;
    for (;;) {
        std::array<platform::PollFd, 2> p{{{.fd = in_open ? 0 : -1}, {.fd = fd}}};
        platform::poll_fds(p, -1);
        if (in_open && (p[0].readable || p[0].closed)) {
            auto n = platform::read_some(0, buf);
            if (!n || *n == 0) in_open = false;
            else if (!platform::write_all(fd, std::string_view(buf.data(), *n))) return 0;
        }
        if (p[1].readable || p[1].closed) {
            auto n = platform::read_some(fd, buf);
            if (!n || *n == 0) return 0;
            if (!platform::write_all(1, std::string_view(buf.data(), *n))) return 0;
        }
    }
}

int host_status(const svc::Context& c, const Globals& g) {
    auto report = c.port.json({"self", "doctor", "--isolation", "--json"});
    const bool ready = report && report->is_object() && report->value("ok", false);
    if (g.json) {
        std::println("{}", Json{{"place", to_string(where())}, {"ready", ready},
                                {"isolation", report ? *report : Json()}}.dump());
        return ready ? 0 : 1;
    }
    std::println("Luban OS -- {}", to_string(where()));
    if (ready) {
        const auto backend = (*report)["backend"].is_object() ? (*report)["backend"].value("name", "") : std::string{};
        std::println("  ready: environments are isolated here ({})", backend);
        return 0;
    }
    std::println("  not ready: this machine needs a one-time setup before it can run a Luban root");
    std::println("  do it: luban setup");
    return 1;
}

std::string edition_label_(const svc::Environment& e) {
    if (e.edition.empty()) return e.kind;
    auto label = e.edition;
    if (label.starts_with("subos:luban-")) label = label.substr(12);
    return label;
}

int overview(const svc::Context& c, const Globals& g) {
    auto envs = svc::list(c);
    if (g.json) {
        Json list = Json::array();
        if (envs) for (const auto& e : *envs) list.push_back({{"name", e.name}, {"edition", e.edition}, {"kind", e.kind}});
        std::println("{}", Json{{"place", to_string(where())}, {"environments", list}}.dump());
        return envs ? 0 : 1;
    }
    switch (where()) {
    case Place::Host: std::println("Luban OS -- on this host"); break;
    case Place::Environment: {
        const char* inside = std::getenv("XLINGS_ACTIVE_SUBOS");
        std::println("Luban OS -- inside the environment '{}'", inside ? inside : "?");
        break;
    }
    case Place::Machine: std::println("Luban OS -- this machine"); break;
    }
    if (!envs) {
        std::println("  environments: could not be listed ({})", envs.error());
        return 1;
    }
    if (envs->empty()) {
        std::println("  no environments yet\n\n  start   luban new box --edition tiny");
        return 0;
    }
    for (const auto& e : *envs)
        std::println("  {:<16} {:<22} {}", e.name, edition_label_(e), e.running ? "running" : "");
    std::println("\n  enter   luban enter {}\n  more    luban --help", envs->front().name);
    return 0;
}

std::optional<std::string> image_of_(std::string_view arg) {
    std::error_code ec;
    if (!fs::is_regular_file(arg, ec)) return std::nullopt;
    const auto f = svc::export_format(arg);
    if (f == "iso" || f == "img" || f == "qcow2") return std::string(arg);
    return std::nullopt;
}

int cmd_new(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    if (p.words.size() != 1) return usage("`luban new <name> [--edition E]`: one name", g);
    svc::NewOptions o{.name = p.words[0], .edition = p.option("--edition").value_or("core")};
    if (auto x = p.option("--proxy")) o.proxy = *x;
    if (auto x = p.option("--carrier")) o.carrier = *x;
    else o.carrier = default_carrier();
    auto out = svc::create(c, o);
    r.value(out.value);
    if (!out.ok()) return out.code;
    std::string facts = out.value.value("edition", std::string{});
    if (facts.starts_with("subos:luban-")) facts = facts.substr(12);
    if (out.value.contains("packages")) facts += std::format(" · {} packages", out.value.value("packages", 0));
    if (out.value.contains("generation") && out.value["generation"].is_number())
        facts += std::format(" · generation {}", out.value["generation"].get<int>());
    r.say(std::format("  {}  {}", o.name, facts));
    r.next("enter", std::format("luban enter {}", o.name));
    return 0;
}

int cmd_enter(const svc::Context& c, const Parsed& p, const Globals& g) {
    if (p.words.size() != 1) return usage("`luban enter <name|image> [--mode sandbox|machine] [--temp]`", g);
    const auto& target = p.words[0];
    vm::EnterOptions o{.temp = p.flag("--temp")};
    if (auto d = p.option("--display")) {
        if (*d != "terminal" && *d != "window") return usage("--display terminal|window", g);
        o.display = *d;
    }
    if (image_of_(target)) {
        o.temp = true;
        return vm::enter(c, target, o);
    }
    auto settings = svc::read_settings(c.home, target);
    if (!settings) {
        if (c.sink) c.sink(svc::error(settings.error()));
        return 1;
    }
    // A machine that is running is entered as the machine it is.
    const auto mode = p.option("--mode").value_or(vm::running(c.home, target) ? std::string("machine") : settings->mode);
    if (mode != "sandbox" && mode != "machine") return usage("--mode sandbox|machine", g);
    if (mode == "machine") return vm::enter(c, target, o);
    if (o.display == "window") return usage("a sandboxed environment's programs use this desktop; a window is a machine's: --mode machine", g);
    return svc::enter_sandbox(c, target, o.temp);
}

int cmd_status(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    if (p.words.empty()) return host_status(c, g);
    auto e = svc::describe(c, p.words[0]);
    if (!e) return usage(e.error(), g);
    Json j{{"name", e->name}, {"kind", e->kind}, {"edition", e->edition}, {"packages", e->packages},
           {"running", e->running}, {"mode", e->settings.mode}, {"persist", e->settings.persist},
           {"shell", e->shell}, {"carrier", e->carrier}};
    if (e->generation) j["generation"] = *e->generation;
    if (g.json) { r.value(j); return 0; }
    std::println("{}  {}{}", e->name, edition_label_(*e), e->running ? "  (running)" : "");
    std::println("  kind      {}{}", e->kind, e->generation ? std::format(", generation {}", *e->generation) : "");
    std::println("  packages  {}", e->packages);
    std::println("  starts    as a {}{}", e->settings.mode, e->settings.persist == "all" ? "" : std::format(", keeping {}", e->settings.persist == "home" ? "only /home" : "nothing"));
    if (!e->shell.empty()) std::println("  shell     {}", e->shell);
    if (e->carrier != "local") std::println("  carrier   {}", e->carrier);
    std::println("  isolation luban config {}", e->name);
    return 0;
}

int cmd_config(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    if (p.words.size() != 1) return usage("`luban config <name> [--setting value ...]`", g);
    std::vector<std::pair<std::string, std::string>> changes;
    for (const auto& [k, v] : p.options) changes.emplace_back(k.substr(2), v);
    auto out = svc::configure(c, p.words[0], changes);
    if (!out.ok()) return out.code;
    if (g.json) { r.value(out.value); return 0; }
    const auto& v = out.value;
    if (!changes.empty()) r.say(std::format("{} {} changed", r.paint("✓", 32), p.words[0]));
    std::println("{}", p.words[0]);
    std::println("  mode      {}", v.value("mode", ""));
    std::println("  persist   {}", v.value("persist", ""));
    if (!v.value("shell", std::string{}).empty()) std::println("  shell     {}", v.value("shell", ""));
    std::println("  desktop   {}", v.value("desktop", std::string{}).empty() ? "(the edition's)" : v.value("desktop", ""));
    std::println("  on-close  {}", v.value("on_close", ""));
    if (auto iso = v.find("isolation"); iso != v.end() && iso->is_object()) {
        std::println("  policy    {}", iso->value("extends", std::string("?")));
        if (auto in = iso->find("isolation"); in != iso->end() && in->is_object()) {
            std::println("  network   {}", in->value("net", std::string("?")));
            if (auto grants = in->find("grants"); grants != in->end() && grants->is_array() && !grants->empty()) {
                std::string list;
                for (const auto& gr : *grants) list += (list.empty() ? "" : ", ") + gr.get<std::string>();
                std::println("  grants    {}", list);
            }
        }
    }
    return 0;
}

int cmd_history(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    std::string name = p.words.empty() ? std::string{} : p.words[0];
    if (name.empty())
        if (const char* inside = std::getenv("XLINGS_ACTIVE_SUBOS"); inside && *inside) name = inside;
    if (name.empty()) return usage("`luban history <name>`: which environment?", g);
    auto out = svc::history(c, name);
    if (!out.ok()) return out.code;
    if (g.json) { r.value(out.value); return 0; }
    for (const auto& row : out.value["generations"])
        std::println("{} {:>3}  {}  {}", row.value("current", false) ? "*" : " ", row.value("generation", 0),
                     row.value("created", ""), row.value("reason", ""));
    return 0;
}

int cmd_export(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    if (p.words.size() != 2) return usage("`luban export <name> <file>`", g);
    svc::ExportOptions o{.name = p.words[0], .file = p.words[1]};
    if (auto x = p.option("--format")) o.format = *x;
    if (auto x = p.option("--size")) o.size = *x;
    if (auto x = p.option("--boot")) o.boot = *x;
    auto out = svc::export_environment(c, o);
    r.value(out.value);
    if (!out.ok()) return out.code;
    const auto format = out.value.value("format", std::string("luban"));
    if (format == "iso" || format == "img" || format == "qcow2") {
        r.next("boot it", std::format("luban enter {}", p.words[1]));
        if (format != "qcow2") r.next("or write", std::format("luban write {} <drive>", p.words[1]));
    } else if (format == "luban") {
        r.next("import", std::format("luban import {}", p.words[1]));
    }
    return 0;
}

int cmd_import(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    if (p.words.size() != 1) return usage("`luban import <file.luban> [--name N]`", g);
    auto out = svc::import_environment(c, p.words[0], p.option("--name").value_or(""));
    r.value(out.value);
    if (!out.ok()) return out.code;
    r.next("enter", std::format("luban enter {}", out.value.value("name", "")));
    return 0;
}

int cmd_rm(svc::Context c, const Parsed& p, const Globals& g) {
    if (p.words.size() != 1) return usage("`luban rm <name>`", g);
    if (!c.yes) {
        if (!can_ask_(g)) {
            auto out = svc::remove(c, p.words[0]);
            return out.code;
        }
        if (!ask_(std::format("remove {} and every file in it?", p.words[0]))) {
            std::println("nothing removed");
            return 1;
        }
        c.yes = true;
    }
    return svc::remove(c, p.words[0]).code;
}

int cmd_boot(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    std::vector<std::string> args = p.words;
    for (const auto& [k, v] : p.options) {
        args.push_back(k);
        if (!v.empty()) args.push_back(v);
    }
    auto out = svc::boot(c, args);
    if (!out.ok()) return out.code;
    if (p.words.empty() && !p.flag("--mark-good")) {
        if (g.json) { r.value(out.value); return 0; }
        const auto& b = out.value;
        std::println("default   {}", b.value("default", std::string("default")));
        std::println("fallback  {}", b.value("fallback", std::string("default")));
        if (b.contains("once") && b["once"].is_string()) std::println("next boot {} (once)", b["once"].get<std::string>());
        if (!b.value("booted", std::string{}).empty())
            std::println("this boot {} via {}{}", b.value("booted", ""), b.value("via", ""), b.value("good", false) ? ", marked good" : "");
    }
    return 0;
}

}  // namespace

std::optional<std::string> Parsed::option(std::string_view name) const {
    for (const auto& [k, v] : options)
        if (k == name) return v;
    return std::nullopt;
}

bool Parsed::flag(std::string_view name) const {
    return std::ranges::any_of(options, [&](const auto& kv) { return kv.first == name; });
}

const spec::CommandSpec& tree() {
    using spec::CommandSpec;
    static const CommandSpec value{
        .name = "luban",
        .description = "Luban OS: environments, and the machines they run as",
        .options = {
            {"--json", "Machine-readable output: the events and the result", true},
            {"--agent", "For an agent: plain output, never a question", true, Level::More},
            {"-y, --yes", "Answer yes to confirmations", true, Level::More},
            {"-v, --verbose", "Each step as well", true, Level::More},
            {"-q, --quiet", "Only what failed", true, Level::Expert},
            {"-h, --help", "This help"},
            {"--version", "The luban version"},
        },
        .children = {
            {.name = "new", .description = "Make an environment",
             .arguments = {{"name", "Its name", true}},
             .options = {{"--edition <EDITION>", "nano, tiny, core (default), desktop, agent-workspace, or ns:name[@version]"},
                         {"--proxy <URL>", "Its network goes only through this SOCKS5h proxy", false, Level::More},
                         {"--carrier <CARRIER>", "Where it runs: local, wsl2, vz", false, Level::Expert}}},
            {.name = "enter", .description = "Go into an environment, or boot an image",
             .arguments = {{"name", "An environment, or an image file (.iso, .img, .qcow2)", true}},
             .options = {{"--mode <MODE>", "sandbox (this machine's kernel) or machine (its own); default: its setting"},
                         {"--temp", "Nothing it writes stays"},
                         {"--display <DISPLAY>", "terminal or window; default: its desktop decides", false, Level::More}}},
            {.name = "run", .description = "Run one command in an environment",
             .arguments = {{"name", "Which", true}, {"command", "The command and its arguments, after --", true, true}}},
            {.name = "ls", .description = "List the environments", .aliases = {"list"}},
            {.name = "status", .description = "This host, or one environment", .arguments = {{"name", "Which"}}},
            {.name = "config", .description = "Show or change an environment's settings",
             .arguments = {{"name", "Which", true}},
             .options = {{"--mode <MODE>", "sandbox or machine: how `luban enter` starts it"},
                         {"--persist <WHAT>", "all, home (only /home stays) or none: what a session keeps"},
                         {"--shell <SHELL>", "Its login shell (fish, bash, sh, or a path)"},
                         {"--desktop <SESSION>", "The desktop it starts, or none"},
                         {"--on-close <WHAT>", "ask, background or poweroff: closing its window"},
                         {"--policy <PRESET>", "dev, private, locked, or ns:name"},
                         {"--proxy <URL>", "Its only network is this SOCKS5h proxy; off for none"},
                         {"--tz <ZONE>", "utc, proxy, or Area/City"},
                         {"--allow <GRANT>", "Grant a capability (clipboard, gpu, audio, ...)"},
                         {"--disallow <GRANT>", "Take a grant back"},
                         {"--mount <HOST[:INSIDE][:ro|rw]>", "Map a host directory into it"},
                         {"--unmount <INSIDE>", "Remove a mapping"},
                         {"--fetch <ACTION>", "auto, ask or deny: installing from inside", false, Level::Expert},
                         {"--observe <LEVEL>", "off, basic, standard or full", false, Level::Expert}},
             .level = Level::More},
            {.name = "stop", .description = "Stop a running environment or machine", .arguments = {{"name", "Which", true}},
             .level = Level::More},
            {.name = "history", .description = "An environment's generations", .arguments = {{"name", "Which"}},
             .level = Level::More},
            {.name = "upgrade", .description = "Move an environment to its edition's newest version",
             .arguments = {{"name", "Which", true}},
             .options = {{"--to <VERSION>", "This version of the edition"}, {"--dry-run", "Show the plan only"}},
             .level = Level::More},
            {.name = "rollback", .description = "Go back a generation", .arguments = {{"name", "Which"}},
             .options = {{"--to <N>", "This generation"}}, .level = Level::More},
            {.name = "export", .description = "Write it out: .iso, .img, .qcow2, .tar(.zst), .luban, or a directory",
             .arguments = {{"name", "Which", true}, {"file", "Where; the format follows the name", true}},
             .options = {{"--format <FORMAT>", "iso, img, qcow2, tar, luban or dir, whatever the name says"},
                         {"--size <SIZE>", "Disk size (img, qcow2)"},
                         {"--boot <PROFILE>", "How an image boots: generic (machines), virt (VMs), or ns:name"}},
             .level = Level::More},
            {.name = "import", .description = "Make an environment from a .luban someone exported",
             .arguments = {{"file", "The .luban file", true}},
             .options = {{"--name <NAME>", "Its name here (default: the one it had)"}},
             .level = Level::More},
            {.name = "write", .description = "Make a boot drive from an image or an environment",
             .arguments = {{"source", "An image file, or an environment", true}, {"device", "The drive, e.g. /dev/sdb", true}},
             .options = {{"--serial <SERIAL>", "The drive's serial (required with --agent)"}},
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

std::string default_carrier() {
    if constexpr (xlings::platform::is_windows) return "wsl2";
    else if constexpr (xlings::platform::is_macos) return "vz";
    else return {};
}

std::expected<Parsed, std::string> parse(std::span<const std::string> args) {
    Parsed p;
    if (args.empty()) return p;
    const auto* node = spec::find_in(tree(), std::array<std::string_view, 1>{args.front()});
    p.command = node ? node->name : args.front();
    std::set<std::string, std::less<>> valued;
    if (node)
        for (const auto& o : node->options) {
            const auto& usage = o.syntax;
            const auto sp = usage.find(' ');
            if (sp == std::string::npos) continue;
            for (std::string_view part : {std::string_view(usage).substr(0, sp)}) {
                const auto comma = part.find(',');
                valued.insert(std::string(comma == std::string_view::npos ? part : part.substr(comma + 2)));
            }
        }
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--") {
            p.after.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        }
        if (a.starts_with("--") && a.find('=') != std::string::npos) {
            p.options.emplace_back(a.substr(0, a.find('=')), a.substr(a.find('=') + 1));
        } else if (a.starts_with("-") && a.size() > 1) {
            if (valued.contains(a)) {
                if (i + 1 >= args.size()) return std::unexpected(std::format("{} needs a value", a));
                p.options.emplace_back(a, args[++i]);
            } else {
                p.options.emplace_back(a, "");
            }
        } else {
            p.words.push_back(a);
        }
    }
    if (node) {
        for (const auto& [k, _] : p.options) {
            const bool known = std::ranges::any_of(node->options, [&](const auto& o) {
                const std::string_view u = o.syntax;
                const auto head = u.substr(0, u.find(' '));
                return head == k || (head.find(',') != std::string_view::npos && head.substr(head.find(',') + 2) == k)
                    || head.substr(0, head.find(',')) == k;
            });
            if (!known) return std::unexpected(std::format("`luban {}` has no option {}", p.command, k));
        }
    }
    return p;
}
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
    Globals g;
    std::vector<std::string> rest;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--") { rest.insert(rest.end(), args.begin() + static_cast<std::ptrdiff_t>(i), args.end()); break; }
        if (a == "--json") g.json = true;
        else if (a == "--agent") g.agent = true;
        else if (a == "-y" || a == "--yes") g.yes = true;
        else if (a == "-v" || a == "--verbose") g.verbose = true;
        else if (a == "-q" || a == "--quiet") g.quiet = true;
        else rest.push_back(a);
    }
    if (const char* mode = std::getenv("XLINGS_AGENT_MODE"); mode && std::string_view(mode) == "1") g.agent = true;

    // Plumbing first: what qemu, the drive writer and the docs tools run.
    if (!rest.empty() && rest.front() == "__write" && rest.size() == 3) return raw_write_(rest[1], rest[2]);
    if (!rest.empty() && rest.front() == "__pipe" && rest.size() == 3) return pipe_(rest[1], rest[2]);
    if (!rest.empty() && rest.front() == "__vm-supervise" && rest.size() == 3) return vm::supervise(rest[1], rest[2]);
    if (!rest.empty() && rest.front() == "__pipe-unix" && rest.size() == 2) return pipe_unix_(rest[1]);
    if (!rest.empty() && (rest.front() == "--version" || rest.front() == "-V")) {
        std::println("luban {}", kVersion);
        return 0;
    }
    if (rest.size() == 1 && rest.front() == "--command-reference-json") {
        std::println("{}", spec::help_json(tree()).dump());
        return 0;
    }
    if (!rest.empty() && (rest.front() == "--help" || rest.front() == "-h" || rest.front() == "help")) {
        const bool all = std::ranges::find(rest, std::string("--all")) != rest.end();
        const bool expert = std::ranges::find(rest, std::string("--expert")) != rest.end();
        std::print("{}", spec::render_help(tree(), tree(), "luban", expert ? Level::Expert : all ? Level::More : Level::Common));
        if (!all && !expert) std::println("install software: xlings install <package>");
        return 0;
    }

    Renderer render(g);
    svc::Context c{.home = home_dir_(), .port = svc::Port(svc::xlings_path()),
                   .sink = [&render](const svc::Event& e) { render(e); }, .yes = g.yes};
    if (!rest.empty()) {
        // The operation journal: luban's own, which the xlings it runs joins.
        const auto trace = xlings::observe::ops::trace_from_environment();
        const char* inherited = std::getenv("XLINGS_TRACE_ID");
        xlings::observe::ops::begin(c.home, trace,
                                    Json{{"client", "luban"}, {"version", std::string(kVersion)},
                                         {"argv", xlings::observe::ops::redact_argv(rest)},
                                         {"actor", g.agent ? "agent" : "cli"}},
                                    !(inherited && trace == inherited));
        xlings::platform::set_env_variable("XLINGS_TRACE_ID", trace);
    }
    const int rc = [&]() -> int {
        if (rest.empty()) return overview(c, g);
        const auto& cmd = rest.front();
        const auto* node = spec::find_in(tree(), std::array<std::string_view, 1>{cmd});
        if (!node) return unknown_command(cmd, g);
        if (std::ranges::find(rest, std::string("--help")) != rest.end() || std::ranges::find(rest, std::string("-h")) != rest.end()) {
            std::print("{}", spec::render_help(tree(), *node, "luban " + node->name, Level::Expert));
            if (auto it = equivalents().find(node->name); it != equivalents().end())
                std::println("\nin xlings: {}", it->second);
            return 0;
        }
        if (node->name == "write") return write_cmd_(c, rest, g);
        auto parsed = parse(rest);
        if (!parsed) return usage(parsed.error(), g);
        const auto& p = *parsed;
        // Everything else needs the xlings beside it to speak what it needs.
        if (node->name != "setup")
            if (auto refused = c.port.handshake()) return usage(*refused, g);
        if (p.command == "new") return cmd_new(c, p, g, render);
        if (p.command == "enter") return cmd_enter(c, p, g);
        if (p.command == "run") {
            if (p.words.empty() || p.after.empty()) return usage("`luban run <name> -- <command...>`", g);
            return svc::run_command(c, p.words[0], p.after);
        }
        if (p.command == "ls") return overview(c, g);
        if (p.command == "status") return cmd_status(c, p, g, render);
        if (p.command == "config") return cmd_config(c, p, g, render);
        if (p.command == "stop") {
            if (p.words.size() != 1) return usage("`luban stop <name>`", g);
            if (vm::running(c.home, p.words[0])) return vm::stop(c, p.words[0]).code;
            return svc::stop(c, p.words[0]).code;
        }
        if (p.command == "history") return cmd_history(c, p, g, render);
        if (p.command == "upgrade") {
            if (p.words.size() != 1) return usage("`luban upgrade <name> [--to V] [--dry-run]`", g);
            return svc::upgrade(c, p.words[0], p.option("--to").value_or(""), p.flag("--dry-run")).code;
        }
        if (p.command == "rollback") {
            std::string name = p.words.empty() ? std::string{} : p.words[0];
            if (name.empty())
                if (const char* inside = std::getenv("XLINGS_ACTIVE_SUBOS"); inside && *inside) name = inside;
            if (name.empty()) return usage("`luban rollback <name>`: which environment?", g);
            return svc::rollback(c, name, p.option("--to").value_or("")).code;
        }
        if (p.command == "export") return cmd_export(c, p, g, render);
        if (p.command == "import") return cmd_import(c, p, g, render);
        if (p.command == "rm") return cmd_rm(c, p, g);
        if (p.command == "setup") return c.port.terminal({"self", "doctor", "--isolation", "--fix"});
        if (p.command == "boot") return cmd_boot(c, p, g, render);
        return unknown_command(cmd, g);
    }();
    xlings::observe::ops::end(rc);
    return rc;
}

}  // namespace luban::cli
