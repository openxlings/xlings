module luban.cli;

import std;
import luban.compat;
import xlings.subos.root_etc;
import xlings.libs.sha512;
import xlings.cli.model;
import xlings.libs.json;
import xlings.i18n;
import xlings.observe;
import xlings.platform;
import xlings.platform.stream;
import xlings.subos.home_view;
import xlings.subos.elevation;
import luban.service;
import luban.vm;
import opensal.wlr;
import opensal;
import openhal;
import openhal.audio;
import openas;
import xlings.carrier;
import openbus;
import xbus.node;
import :render;

namespace luban::cli {

namespace spec = xlings::cli::spec;
namespace svc = luban::service;
namespace vm = luban::vm;
using spec::Level;
namespace fs = std::filesystem;
using Json = nlohmann::json;

std::string default_carrier();

namespace {

// What each luban command is in xlings's words, at the foot of its help: the
// way to everything xlings can do starts from here.
const std::map<std::string, std::string, std::less<>>& equivalents() {
    static const std::map<std::string, std::string, std::less<>> value{
        {"new", "xlings subos new <name> --from luban:<edition>"},
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
    // The home it was installed into, as the xlings beside it finds its own:
    // <home>/bin/luban beside <home>/bin/xlings, with <home>/.xlings.json. A
    // carrier's machine (/xlings) and a portable home are such homes.
    std::error_code ec;
    if (const auto bin = xlings::platform::get_executable_path().parent_path();
        fs::exists(bin / (xlings::platform::is_windows ? "xlings.exe" : "xlings"), ec)
        && fs::exists(bin.parent_path() / ".xlings.json", ec))
        return bin.parent_path();
    if (const char* u = std::getenv("HOME"); u && *u) return fs::path(u) / ".xlings";
    return ".xlings";
}

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

// A line typed without being shown (a password): the terminal raw, nothing
// echoed, backspace honoured, restored however it ends.
std::string read_hidden_(std::string_view prompt) {
    std::print("{}", prompt);
    std::cout.flush();
    std::string line;
    const auto saved = xlings::platform::make_terminal_raw(0);
    for (char ch = 0; xlings::platform::read_exact(0, &ch, 1);) {
        if (ch == '\r' || ch == '\n') break;
        if (ch == 3 || ch == 4) { line.clear(); break; }   // Ctrl-C / Ctrl-D: nothing
        if ((ch == 127 || ch == 8) && !line.empty()) line.pop_back();
        else if (static_cast<unsigned char>(ch) >= 32) line += ch;
    }
    if (saved) xlings::platform::restore_terminal(0, *saved);
    std::println("");
    return line;
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

// What this computer can do with Luban (part 4 §6.3): one line per way of
// running a system, and the devices -- the same report the Luban OS app and
// an agent read. Exit 0 when a system can run here (a sandbox or a machine).
// What is on a bus (design 2026-10-22 §7): the host's -- its router, the
// systems linked under it, what its programs offer -- or one system's: what
// its programs offer, seen through the link. Nothing started the home's
// router yet: said, with what starts it.
int bus_status(const svc::Context& c, const std::optional<std::string>& name, const Globals& g, Renderer& r) {
    const xbus::Address at{"unix", vm::bus_socket(c.home).string()};
    auto n = xbus::Node::connect({.name = "luban-status", .actor = "program:luban", .router = at,
                                  .connect_wait = std::chrono::milliseconds(800), .telemetry = false});
    if (!n) {
        const auto why = std::string("this home's bus is not running: a machine starts it, or xbus router --listen ") + at.text();
        if (g.json) { r.value(Json{{"bus", nullptr}, {"why", why}}); return 0; }
        std::println("{}", why);
        return 0;
    }
    auto& bus = **n;
    const auto root = bus.root();
    const auto under = name ? vm::system_bus_root(*name) : root;
    auto found = bus.resolve(under + "/**").value_or(std::vector<xbus::Resolved>{});
    // A system's own router endpoints are its bus, not what is on it.
    std::erase_if(found, [&](const xbus::Resolved& e) {
        return e.path.starts_with(under + "/bus/") || (!name && e.path.starts_with(root + "/system/"));
    });
    Json objects = Json::array();
    for (const auto& e : found) {
        objects.push_back({{"path", e.path.substr(under.size() + 1)}, {"kind", std::string(openbus::name_of(e.kind))}, {"node", e.name},
                           {"interfaces", e.interfaces}, {"via", e.via.size()}});
    }
    Json links = Json::array();
    if (const auto l = bus.call("bus/links"); l && !l->empty()) links = Json::parse(l->front(), nullptr, false);
    if (!links.is_array()) links = Json::array();
    const bool linked = !name || std::ranges::any_of(links, [&](const Json& x) { return x.value("root", std::string{}) == under; });
    Json out{{"bus", at.text()}, {"root", under}, {"objects", objects}};
    if (name) out["linked"] = linked;
    else out["systems"] = links;
    if (g.json) { r.value(out); return 0; }
    if (name && !linked) {
        std::println("{} is not on this home's bus (it is linked while it runs as a machine)", *name);
        return 0;
    }
    std::println("{}  bus at {}", name ? *name : std::string("this host"), at.where);
    if (!name) {
        for (const auto& l : links) std::println("  system    {}", l.value("root", std::string{}).substr(root.size() + 1));
    }
    if (objects.empty()) std::println("  nothing offered on it");
    for (const auto& o : objects) {
        std::string ifs;
        for (const auto& i : o["interfaces"]) ifs += (ifs.empty() ? "" : ", ") + i.get<std::string>();
        std::println("  {:<8}  {:<40} {}{}", o.value("kind", std::string{}), o.value("path", std::string{}), o.value("node", std::string{}),
                     ifs.empty() ? "" : "  (" + ifs + ")");
    }
    std::println("  more: xbus ls '{}/**' --router {}", under, at.text());
    return 0;
}

int host_status(const svc::Context& c, const Globals& g) {
    const auto caps = svc::capabilities(svc::gather(c.home, c.port));
    const auto* sandbox = caps.mode("sandbox");
    const auto* machine = caps.mode("machine");
    const bool ready = (sandbox && sandbox->ready) || (machine && machine->ready);
    const auto versions = svc::versions(c);
    if (g.json) {
        auto out = svc::to_json(caps);
        out["place"] = to_string(where());
        out["ready"] = sandbox && sandbox->ready;
        out["isolation"] = caps.isolation;
        out["versions"] = svc::to_json(versions);
        std::println("{}", out.dump());
        return ready ? 0 : 1;
    }
    std::println("Luban OS {} -- {} ({}), {}", svc::kVersion, caps.os, caps.arch, to_string(where()));
    // The parts' versions in one line; where they disagree, each with what to
    // do (design 2026-10-19 §11).
    {
        std::string line = std::format("xlings {} · luban {}", versions.xlings.empty() ? "?" : versions.xlings, versions.luban);
        if (!versions.app.empty()) line += std::format(" · luban-os {}", versions.app);
        if (!versions.index.empty()) line += std::format(" · index {}", versions.index);
        std::println("  {:<9}{}", xlings::i18n::tr("luban.status.versions"), line);
        for (const auto& m : versions.mismatches) std::println("  {:<9}  {} -- {}", "", m.why, m.next);
        if (g.verbose && !versions.looked)
            std::println("  {:<9}  {}", "", xlings::i18n::tr("luban.status.unlisted"));
    }
    for (const auto& m : caps.modes) {
        if (m.ready) {
            std::println("  {:<9}ready      {}", m.name, m.how);
            continue;
        }
        std::println("  {:<9}not ready  {}", m.name, m.reason);
        if (!m.route.empty()) std::println("  {:<9}           do it: {}", "", m.route);
    }
    // The devices, counted by kind: what each is, `--json`.
    std::vector<std::string> parts, unread;
    for (const auto& d : caps.devices) {
        const auto kind = std::string(openhal::name_of(d.kind));
        if (!d.unreadable.empty()) {
            unread.push_back(kind);
            continue;
        }
        if (d.devices.empty()) continue;
        const auto first = d.devices.front().name;
        if (d.kind == openhal::Kind::Gpu)
            parts.push_back(d.devices.size() == 1 ? std::format("1 GPU ({})", first) : std::format("{} GPUs", d.devices.size()));
        else if (d.kind == openhal::Kind::Display)
            parts.push_back(std::format("{} display{}", d.devices.size(), d.devices.size() == 1 ? "" : "s"));
        else if (d.kind == openhal::Kind::AudioOutput)
            parts.push_back(std::format("{} audio output{}", d.devices.size(), d.devices.size() == 1 ? "" : "s"));
        else if (d.kind == openhal::Kind::AudioInput)
            parts.push_back(std::format("{} audio input{}", d.devices.size(), d.devices.size() == 1 ? "" : "s"));
    }
    std::string line;
    for (const auto& p : parts) line += (line.empty() ? "" : " · ") + p;
    if (!unread.empty()) {
        std::string what;
        for (const auto& u : unread) what += (what.empty() ? "" : ", ") + u;
        line += std::format("{}could not read: {} (why: luban status --json)", line.empty() ? "" : " · ", what);
    }
    std::println("  {:<9}{}", "devices", line.empty() ? std::string("none found") : line);
    return ready ? 0 : 1;
}

// What a system is called (design 2026-10-18 §4.5): its distribution's name.
std::string edition_label_(const svc::Environment& e) {
    if (e.edition.empty()) return e.kind;
    return svc::display_name(svc::lineage_of(e));
}

int overview(const svc::Context& c, const Globals& g, std::string_view pattern = {}, bool all = false) {
    auto envs = svc::list(c);
    // A name pattern (design 2026-10-19 §6): `luban ls a`. Without one, the
    // output is what it always was.
    if (envs && !pattern.empty()) std::erase_if(*envs, [&](const svc::Environment& e) { return !name_matches(e.name, pattern); });
    if (g.json) {
        Json list = Json::array();
        // What each is (the fields of `status <name> --json`): a host that asks
        // a carrier's machine reads all of them in one call.
        if (envs)
            for (const auto& e : *envs) {
                Json one{{"name", e.name}, {"edition", e.edition}, {"kind", e.kind}, {"packages", e.packages},
                         {"running", e.running}, {"shell", e.shell}, {"carrier", e.carrier}, {"desktops", e.desktops}};
                if (e.generation) one["generation"] = *e.generation;
                list.push_back(std::move(one));
            }
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
    if (envs->empty() && !pattern.empty()) {
        std::println("  {}", xlings::i18n::trf("luban.ls.no_match", pattern));
        return 0;
    }
    if (envs->empty()) {
        std::println("  no environments yet\n\n  start   luban new box");
        return 0;
    }
    for (const auto& e : *envs)
        std::println("  {:<16} {:<22} {}", e.name, edition_label_(e), e.running ? "running" : "");
    // What cancelled makings set aside: with --all, each; else one line.
    if (const auto left = svc::unfinished(c.home); !left.empty()) {
        if (all)
            for (const auto& u : left) std::println("  {:<16} {:<22}", u.name, "unfinished");
        else
            std::println("\n  {} unfinished making{}: luban ls --all", left.size(), left.size() == 1 ? "" : "s");
        std::println("  clear   luban rm --unfinished");
    }
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

// What a system would be (design 2026-10-18 §5.4): what it is, what it is
// built on, what it will be like, and what it does beyond Luban -- one screen,
// at most 80 columns, what needs attention marked first.
void print_preview_(const svc::NewPreview& v, const svc::NewOptions& o, Renderer& r, bool asking) {
    const auto& l = v.lineage;
    auto source = [](const svc::Layer& layer) {
        return layer.official() ? std::string("official") : std::format("community · {}", layer.index);
    };
    const auto& top = l.layers.front();
    std::println(" {:<52}{}", r.paint(svc::display_name(l), 1), source(top));
    std::println("");
    if (const auto* base = l.base(); base && base != &top)
        std::println(" {:<12} {:<38} {}", "Luban OS", std::format("{} {}", base->name(), base->version()), r.paint("official", 2));
    if (const auto* parent = l.parent(); parent && parent != l.base())
        std::println(" {:<12} {:<38} {}", "Based on", std::format("{} {}", parent->name(), parent->version()), source(*parent));
    std::println("");
    std::println(" It will be");
    const auto settings = l.luban("settings");
    std::string like = settings.value("mode", std::string("sandbox"));
    for (const auto& [k, val] : o.settings) if (k == "mode") like = val;
    std::string desktop = settings.value("desktop", std::string{});
    for (const auto& [k, val] : o.settings) if (k == "desktop") desktop = val;
    if (!desktop.empty() && desktop != "none") like += std::format(" · desktop {}", desktop);
    if (const auto shell = settings.value("login_shell", std::string{}); !shell.empty()) like += " · login shell " + shell;
    std::println("   {}", like);
    std::println("   {} packages", l.packages.size());
    if (!v.beyond.empty()) {
        std::println("");
        std::println(" More than Luban {}", l.base() ? l.base()->name() : std::string("(none)"));
        for (const auto& b : v.beyond)
            std::println(" {} {:<9}{:<44} {}", b.attention ? r.paint("!", 33) : std::string(" "), b.what, b.text, b.from);
    }
    if (!v.extras.empty()) {
        std::println("");
        std::println(" Also installs");
        for (const auto& e : v.extras)
            std::println("   {:<36} {:<9} {}", e.ref(), e.role(), e.official() ? std::string("official") : "community · " + e.index);
    }
    std::println("");
    if (asking) {
        std::string again = std::format("luban new {}", o.name);
        if (!o.edition.empty()) again += " --edition " + o.edition;
        for (const auto& x : o.xpkgs) again += " --xpkg " + x;
        std::println("   details  {} --dry-run", again);
    }
}

int cmd_new(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    if (p.words.size() == 2 && !p.option("--edition") && !p.option("--xpkg"))
        // The order before 2026.10.13.1, `luban new <name> <edition>`.
        return usage(std::format("the system is an option: `luban new {} --edition {}` (an official one) "
                                 "or `--xpkg {}` (any index)", p.words[0], p.words[1], p.words[1]), g);
    if (p.words.size() != 1) return usage("`luban new <name> [--edition E] [--xpkg COORD]...`: one name", g);
    svc::NewOptions o{.name = p.words[0], .edition = p.option("--edition").value_or("")};
    // --xpkg, as many as given; the settings luban config takes, in order.
    static constexpr std::array<std::string_view, 11> kSettings{"--desktop", "--enter", "--mode", "--persist", "--shell",
                                                                "--login", "--on-close", "--policy", "--tz", "--allow", "--runtime"};
    for (const auto& [k, v] : p.options) {
        if (k == "--xpkg") o.xpkgs.push_back(v);
        else if (std::ranges::find(kSettings, k) != kSettings.end()) o.settings.emplace_back(k.substr(2), v);
    }
    // What it would be, before anything is made (design 2026-10-18 §5.3).
    auto preview = svc::preview_new(c, o);
    if (!preview) return preview.error();
    if (p.flag("--dry-run")) {
        if (g.json) { r.value(preview->value()); return 0; }
        print_preview_(*preview, o, r, false);
        return 0;
    }
    // A third party's part, shown once and agreed to (design §5.4).
    if (preview->needs_confirmation() && !g.yes) {
        if (!can_ask_(g))
            return usage(std::format("{} is not Luban's own: agree with --yes (`luban new {} ... --dry-run` shows what it does)",
                                     svc::display_name(preview->lineage), o.name), g);
        print_preview_(*preview, o, r, true);
        std::print(" Make {} from it? [y/N] ", o.name);
        std::string answer;
        std::getline(std::cin, answer);
        answer = trim_(answer);
        if (answer != "y" && answer != "Y" && answer != "yes") {
            r.say("  nothing was made");
            return 1;
        }
    }
    o.agreed = preview->needs_confirmation();
    if (auto x = p.option("--proxy")) o.proxy = *x;
    // Its login user (part 3 §6.1): asked for once, before the download,
    // when a desktop needs one or --user names it; -y takes the defaults;
    // agent mode asks nothing and names the flags.
    if (auto x = p.option("--user")) o.user = *x;
    const auto shown = svc::display_name(preview->lineage);
    if (o.user.empty() && svc::needs_user(*preview, &o)) {
        if (g.yes) o.user = std::string(svc::kDefaultUser);
        else if (!can_ask_(g))
            return usage(std::format("{} needs a user to log in as: --user <name> (and --password-file <file> or --no-password), or -y for {}",
                                     shown, svc::kDefaultUser), g);
        else {
            std::print("▸ {} needs a user to log in as\n  user name [{}]: ", shown, svc::kDefaultUser);
            std::string answer;
            std::getline(std::cin, answer);
            answer = trim_(answer);
            o.user = answer.empty() ? std::string(svc::kDefaultUser) : answer;
        }
    }
    if (!o.user.empty() && !xlings::subos::root_etc::valid_user_name(o.user))
        return usage(std::format("'{}' is not a user name (a-z, 0-9, _ and -, a letter first)", o.user), g);
    if (!o.user.empty()) {
        std::string password;
        if (auto file = p.option("--password-file")) {
            std::ifstream in(*file);
            if (!in) return usage(std::format("cannot read {}", *file), g);
            std::getline(in, password);
        } else if (!p.flag("--no-password") && !g.yes && can_ask_(g)) {
            for (;;) {
                // Empty: the default one, said once it is made (design 2026-10-20 §4.5).
                const auto first = read_hidden_(std::format("  password (empty: {}, the default): ", svc::kDefaultPassword));
                if (first.empty()) break;
                if (read_hidden_("  again: ") == first) { password = first; break; }
                std::println("  the two did not match; again");
            }
        }
        if (!password.empty()) o.password_hash = xlings::sha512::crypt(password, xlings::sha512::random_salt());
        std::ranges::fill(password, '\0');
        o.no_password = p.flag("--no-password");
    }
    if (auto x = p.option("--carrier")) o.carrier = *x;
    else o.carrier = default_carrier();
    auto out = svc::create(c, o, &*preview);
    r.value(out.value);
    if (!out.ok()) return out.code;
    // `box  myos 2026.11.1 (Luban desktop) · desktop lxqt · 36 packages · generation 41` (design §4.5).
    std::string facts = out.value.value("distribution", shown);
    if (out.value.contains("desktop")) facts += std::format(" · desktop {}", out.value.value("desktop", std::string{}));
    if (out.value.contains("packages")) facts += std::format(" · {} packages", out.value.value("packages", 0));
    if (out.value.contains("generation") && out.value["generation"].is_number())
        facts += std::format(" · generation {}", out.value["generation"].get<int>());
    r.say(std::format("  {}  {}", o.name, facts));
    if (out.value.value("shell", std::string{}).empty()) {
        // Nothing to log into (nano): what it runs, it runs one command at a time.
        r.say("  it has no shell: commands run in it one at a time");
        r.next("run", std::format("luban run {} -- xlings --version", o.name));
    } else {
        r.next("enter", std::format("luban enter {}", o.name));
    }
    return 0;
}

// `luban run <name> [--display window] -- <command...>` (design 2026-10-18 §7,
// T0): here, or a graphical program alone in a window of its own -- the
// Luban OS app's, titled by the program, gone when it ends.
int cmd_run(const svc::Context& c, const Parsed& p, const Globals& g) {
    if (p.words.size() != 1 || p.after.empty()) return usage("`luban run <name> [--display window] -- <command...>`", g);
    const auto display = p.option("--display").value_or("terminal");
    if (display != "terminal" && display != "window") return usage("--display terminal|window", g);
    if (display == "terminal") {
        // A system that starts as a machine (or runs as one) runs it there,
        // with its own kernel -- not in a sandbox beside it.
        if constexpr (!xlings::platform::is_windows) {
            const auto settings = svc::read_settings(c.home, p.words[0]);
            if (vm::running(c.home, p.words[0]) || (settings && settings->mode == "machine"))
                return vm::run(c, p.words[0], p.after, p.option("--user").value_or(""));
        }
        return svc::run_command(c, p.words[0], p.after, p.option("--user").value_or(""));
    }
    if (p.option("--user")) return usage("--user is the terminal's: a window runs as the system's login user", g);
    const auto app = svc::window_app(c, p.words[0]);
    if (!app) {
        if (c.sink) c.sink(app.error());
        return 1;
    }
    // One word for the session: each argument quoted for the shell that runs it.
    std::string command;
    for (const auto& w : p.after) {
        const bool plain = !w.empty() && std::ranges::all_of(w, [](char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) || std::string_view("-_./=:,@+%").find(ch) != std::string_view::npos;
        });
        std::string q = "'";
        for (char ch : w) q += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
        command += (command.empty() ? "" : " ") + (plain ? w : q + "'");
    }
    return xlings::platform::run_argv({app->string(), "--attach", p.words[0], "--app", command});
}

int cmd_enter(const svc::Context& c, const Parsed& p, const Globals& g) {
    if (p.words.size() != 1) return usage("`luban enter <name|image> [--mode sandbox|machine] [--temp]`", g);
    const auto& target = p.words[0];
    vm::EnterOptions o{.temp = p.flag("--temp")};
    if (auto d = p.option("--display")) {
        if (*d != "terminal" && *d != "window") return usage("--display terminal|window", g);
        o.display = *d;
    }
    // Its desktop this once (design 2026-10-18 §3): shown in a window.
    const auto once = p.option("--desktop");
    if (once) {
        if (o.display == "terminal") return usage("--desktop is shown in a window: drop --display terminal", g);
        o.display = "window";
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
    if (auto e = svc::describe(c, target); e && e->kind == "root" && e->shell.empty()) {
        if (c.sink) c.sink(svc::error(std::format("{} has no shell to go into", target),
                                      std::format("luban run {} -- <command>", target)));
        return 1;
    }
    // Not told how (design 2026-10-18 §8, E5): what its plan says it shows
    // first -- its desktop in a window where there is a display to show one,
    // its console here otherwise, and why.
    if (o.display.empty()) {
        if (auto e = svc::describe(c, target); e && svc::enter_plan(*e).target == "desktop") {
            const bool shown = !xlings::platform::is_linux || std::getenv("WAYLAND_DISPLAY") || std::getenv("DISPLAY");
            if (shown && !svc::luban_os_app(c.home).empty()) o.display = "window";
            else if (c.sink)
                c.sink(svc::notice(std::format("{} shows its desktop in a window; {} -- its console here", target,
                                               shown ? "the Luban OS app is not installed (xlings install luban-os)"
                                                     : "this terminal has no display")));
        }
    }
    if (o.display == "window") {
        // The window is the Luban OS app's, sandbox or machine alike.
        const auto app = svc::window_app(c, target);
        if (!app) {
            if (c.sink) c.sink(app.error());
            return 1;
        }
        std::vector<std::string> argv{app->string(), "--attach", target};
        if (once) argv.insert(argv.end(), {"--desktop", *once});
        return xlings::platform::run_argv(argv);
    }
    // A machine that is running is entered as the machine it is.
    const auto mode = p.option("--mode").value_or(vm::running(c.home, target) ? std::string("machine") : settings->mode);
    if (mode != "sandbox" && mode != "machine") return usage("--mode sandbox|machine", g);
    if (auto u = p.option("--user")) o.user = *u;
    if (mode == "machine") return vm::enter(c, target, o);
    return svc::enter_sandbox(c, target, o.temp, o.user);
}

int cmd_status(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    // The self-check (design 2026-10-16 L §11): asked inside the system, by
    // the xlings of its session -- from the host through `subos exec`.
    if (p.flag("--check")) {
        std::vector<std::string> check{"subos", "__check"};
        if (g.json) check.push_back("--json");
        if (p.words.empty()) return c.port.terminal(check);
        std::vector<std::string> inside{"/run/xlings/xlings"};
        inside.insert(inside.end(), check.begin(), check.end());
        return svc::run_command(c, p.words[0], inside);
    }
    // What its latest operation did, the system's own record of it too
    // (design 2026-10-21 §7.4): xlings's journal, followed where it links.
    if (p.flag("--log")) {
        if (p.words.empty()) return usage("`luban status <name> --log`: which environment?", g);
        std::vector<std::string> log{"self", "log", "--subos", p.words[0]};
        if (g.json) log.push_back("--json");
        return c.port.terminal(log);
    }
    if (p.flag("--bus")) return bus_status(c, p.words.empty() ? std::nullopt : std::optional<std::string>(p.words[0]), g, r);
    if (p.words.empty()) return host_status(c, g);
    auto e = svc::describe(c, p.words[0]);
    if (!e) return usage(e.error(), g);
    Json j{{"name", e->name}, {"kind", e->kind}, {"edition", e->edition}, {"packages", e->packages},
           {"running", e->running}, {"mode", e->settings.mode}, {"persist", e->settings.persist},
           {"shell", e->shell}, {"carrier", e->carrier}, {"desktops", e->desktops}};
    if (e->generation) j["generation"] = *e->generation;
    // What it needs of this luban (design 2026-10-18 §9): its root's layout, its manifest's requires.
    const auto unmet = svc::unmet_needs(*e, svc::luban_provides());
    // System / desktop / enter (design 2026-10-18 §3, §4.5).
    const auto lineage = svc::lineage_of(*e);
    const auto plan = svc::enter_plan(*e);
    if (!lineage.layers.empty()) {
        j["distribution"] = svc::display_name(lineage);
        if (const auto* b = lineage.base()) j["luban_base"] = std::format("{} {}", b->name(), b->version());
        if (const auto* parent = lineage.parent()) j["parent"] = parent->ref;
        j["lineage"] = Json::array();
        for (const auto& l : lineage.layers) j["lineage"].push_back({{"ref", l.ref}, {"index", l.index}, {"official", l.official()}});
    }
    j["desktop"] = plan.target == "desktop" ? Json(plan.session) : Json();
    j["enter"] = plan.target;
    if (!plan.why.empty()) j["why"] = plan.why;
    j["layout"] = e->layout;
    j["needs"] = e->needs;
    j["unmet"] = Json::array();
    for (const auto& v : unmet) j["unmet"].push_back(v.why());
    auto notes = svc::notes(c, *e);
    // Its user still on the default password, said until changed (design 2026-10-20 §4.5).
    if (const auto root = svc::root_of(c.home, e->name)) {
        const auto user = xlings::subos::root_etc::login_user(*root / "etc");
        if (user != "root" && svc::default_password_in_use(*root, user))
            notes.push_back(svc::Note{std::format("{}'s password is still the default ({})", user, svc::kDefaultPassword),
                                      {std::format("luban run {} --user {} -- passwd", e->name, user)}});
    }
    // Its network in one line, the same whatever it runs as (design 2026-10-19
    // §27.2): like the host -- the host's proxy, as a machine reaches it --
    // only its proxy, or none.
    std::string network = "like the host";
    if (auto iso = c.port.json({"subos", "config", e->name, "--json"}); iso && iso->is_object()) {
        const auto in = iso->value("isolation", Json::object());
        const auto net = in.value("net", std::string("host"));
        if (net == "none") network = "none";
        else if (net == "proxy") network = "only its proxy " + in.value("proxy", std::string{});
        else {
            const auto host = xlings::platform::environment();
            const bool machine = e->settings.mode == "machine";
            network = svc::network_line(host, machine ? svc::host_proxy_env(host, svc::kQemuGateway, svc::kQemuNat) : host);
            // Running with what it was started with: a changed host proxy is the next start's.
            if (const auto started = machine ? vm::started_env(c.home, e->name) : std::nullopt)
                if (const auto was = svc::network_line(host, *started); was != network)
                    notes.push_back(svc::Note{xlings::i18n::trf("luban.status.proxy_changed", was),
                                              {std::format("luban stop {0} && luban enter {0}", e->name)}});
        }
    }
    j["network"] = network;
    // A running machine's GPU, as it was started (WuXian §7.5).
    const auto gpu = e->settings.mode == "machine" ? vm::started_gpu(c.home, e->name) : std::nullopt;
    if (gpu) j["gpu"] = *gpu;
    j["notes"] = Json::array();
    for (const auto& n : notes) j["notes"].push_back({{"text", n.text}, {"next", n.next}});
    if (g.json) { r.value(j); return 0; }
    std::println("{}  {}{}", e->name, edition_label_(*e), e->running ? "  (running)" : "");
    if (!lineage.layers.empty()) {
        std::string from;
        if (const auto* parent = lineage.parent(); parent && parent != lineage.base())
            from = std::format("   on {}{}", parent->ref, parent->official() ? "" : " (" + parent->index + ")");
        std::println("  system    {}{}", svc::display_name(lineage), from);
    }
    {
        std::string others;
        for (const auto& d : e->desktops)
            if (d != plan.session) others += (others.empty() ? "" : ", ") + d;
        std::println("  desktop   {}{}", plan.target == "desktop" ? plan.session : (e->desktops.empty() ? "none" : "none (console)"),
                     others.empty() ? std::string{} : "   also: " + others);
        if (!plan.why.empty()) std::println("  {} {}", r.paint("!", 33), plan.why);
    }
    std::println("  enter     {} · {}", plan.target, e->settings.mode);
    std::println("  kind      {}{}", e->kind, e->generation ? std::format(", generation {}", *e->generation) : "");
    std::println("  packages  {}", e->packages);
    std::println("  starts    as a {}{}", e->settings.mode, e->settings.persist == "all" ? "" : std::format(", keeping {}", e->settings.persist == "home" ? "only /home" : "nothing"));
    if (!e->shell.empty()) std::println("  shell     {}", e->shell);
    if (e->carrier != "local") std::println("  carrier   {}", e->carrier);
    std::println("  {:<9} {}", xlings::i18n::tr("luban.status.network"), network);
    if (gpu) std::println("  gpu       {}", *gpu);
    std::println("  isolation luban config {}", e->name);
    for (const auto& v : unmet) std::println("  {} {}: this luban is older than it -- xlings update luban", r.paint("!", 33), v.why());
    // What may be done, never forced (design 2026-10-19 §12.2).
    for (const auto& n : notes) {
        std::println("  {} {}", r.paint("·", 36), n.text);
        for (const auto& next : n.next) std::println("      {}", next);
    }
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
    // Before its move into its own userland, when that was kept (design
    // 2026-10-19 §12.4): the first line, its way back.
    const auto kept = svc::relayout_archive(c.home, name);
    if (kept) {
        std::error_code ec;
        out.value["relayout"] = {{"file", kept->string()}, {"size", fs::file_size(*kept, ec)}};
    }
    if (g.json) { r.value(out.value); return 0; }
    if (kept) {
        std::error_code ec;
        std::println("  before its move into its own userland (an archive, {} MB): luban rollback {} --to relayout",
                     fs::file_size(*kept, ec) / (1024 * 1024), name);
    }
    for (const auto& row : out.value["generations"])
        std::println("{} {:>3}  {}  {}", row.value("current", false) ? "*" : " ", row.value("generation", 0),
                     row.value("created", ""), row.value("reason", ""));
    return 0;
}

int cmd_upgrade(const svc::Context& c, const Parsed& p, const Globals& g, Renderer& r) {
    if (p.words.size() != 1) return usage("`luban upgrade <name> [--to V] [--dry-run]`", g);
    const bool dry = p.flag("--dry-run");
    auto out = svc::upgrade(c, p.words[0], p.option("--to").value_or(""), dry);
    r.value(out.value);
    if (!out.ok() || g.json || out.value.value("up_to_date", true)) return out.code;
    // What moves: the edition, its packages, its policy.
    const auto& v = out.value;
    const auto label = [](const std::string& ref) { return std::string(luban::compat::without_frozen_prefix(ref)); };
    r.say(std::format("  edition  {} -> {}", label(v.value("from", "")), label(v.value("to", ""))));
    for (const auto& row : v.value("upgrade", Json::array()))
        r.say(std::format("  upgrade  {} {} -> {}", row.value("key", ""), row.value("from", "?"), row.value("to", "")));
    for (const auto& row : v.value("add", Json::array())) r.say(std::format("  add      {}", row.get<std::string>()));
    for (const auto& row : v.value("keep", Json::array()))
        r.say(row.value("dropped", false)
                  ? std::format("  keep     {} (the edition no longer has it)", row.value("key", ""))
                  : std::format("  keep     {} {} (yours; the edition has {})", row.value("key", ""),
                                row.value("from", ""), row.value("edition", "")));
    if (auto pol = v.find("policy"); pol != v.end() && pol->is_object())
        r.say(pol->value("applied", false)
                  ? std::format("  policy   {} -> {}", pol->value("from", "(none)"), pol->value("to", ""))
                  : std::format("  policy   {} kept: {} allows more", pol->value("from", "(none)"), pol->value("to", "")));
    if (dry) r.next("apply", std::format("luban upgrade {}", p.words[0]));
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
    // What cancelled or failed makings set aside (design 2026-10-21 §7.3).
    if (p.flag("--unfinished")) {
        if (!p.words.empty()) return usage("`luban rm --unfinished` takes no name", g);
        const auto left = svc::unfinished(c.home);
        if (left.empty()) { std::println("nothing unfinished"); return 0; }
        for (const auto& u : left) std::println("  {:<16} {}", u.name, u.path.string());
        if (!c.yes) {
            if (!can_ask_(g)) return usage("`luban rm --unfinished -y`: nobody to ask", g);
            if (!ask_(std::format("remove these {} unfinished makings?", left.size()))) {
                std::println("nothing removed");
                return 1;
            }
        }
        std::println("removed {}", svc::remove_unfinished(c.home));
        return 0;
    }
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
        // This boot, as stage-0 recorded it (boot.json's `last`).
        if (auto last = b.find("last"); last != b.end() && last->is_object() && !last->value("subos", std::string{}).empty())
            std::println("this boot {} via {}{}", last->value("subos", ""), last->value("via", ""),
                         last->value("good", false) ? ", marked good" : "");
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

// Who runs this luban (openas §2): the actor whoever started it acts as,
// inherited through XLINGS_ACTOR, else what this process can tell.
openas::Actor principal(const Globals& g) {
    if (!g.as.empty())
        if (auto given = openas::parse(g.as)) return *given;
    if (const char* a = std::getenv("XLINGS_ACTOR"); a && *a)
        if (auto known = openas::parse(a)) return *known;
    if (g.agent) return {openas::Kind::Agent, {}, {}};
    return {xlings::platform::stdin_is_terminal() ? openas::Kind::Person : openas::Kind::Program, {}, {}};
}

// How the journal knows who it is (openas §2.5): `given` on this command
// line by whoever ran it, `inherited` from the process that started this
// one (XLINGS_ACTOR), `observed` (a terminal, agent mode) otherwise.
std::string_view principal_source(const Globals& g) {
    if (!g.as.empty() && openas::parse(g.as)) return "given";
    if (const char* a = std::getenv("XLINGS_ACTOR"); a && *a && openas::parse(a)) return "inherited";
    return "observed";
}

namespace {

std::string long_name_(const spec::OptionSpec& o) {
    for (auto a : spec::option_aliases(o))
        if (a.starts_with("--")) return std::string(a.substr(2));
    return {};
}

// The options a verb's questions are answered by, besides -y.
const std::map<std::string, std::vector<std::string>, std::less<>>& extra_answers_() {
    static const std::map<std::string, std::vector<std::string>, std::less<>> value{
        {"new", {"user", "password-file", "no-password"}},
        {"write", {"serial"}},
    };
    return value;
}

}  // namespace

// luban's verbs as openas describes them (part 4 §7): the command tree, each
// command with its effect, its arguments and options (the global ones too),
// and what answers its questions.
std::vector<openas::Verb> verbs() {
    std::vector<openas::Verb> out;
    for (const auto& cmd : tree().children) {
        if (cmd.effect.empty()) continue;   // not a verb: interface, the binding itself
        openas::Verb v{.name = cmd.name, .summary = cmd.description,
                       .effect = openas::effect_from(cmd.effect).value_or(openas::Effect::External)};
        for (const auto& a : cmd.arguments)
            v.arguments.push_back({.name = a.name, .description = a.description, .required = a.required, .repeated = a.variadic});
        for (const auto* list : {&cmd.options, &tree().options})
            for (const auto& o : *list) {
                if (list == &tree().options && !o.global) continue;
                const auto name = long_name_(o);
                if (name.empty()) continue;
                v.options.push_back({.name = name, .description = o.description, .flag = o.syntax.find('<') == std::string::npos});
            }
        if (openas::asks_first(v.effect)) v.answers.push_back("yes");
        if (auto it = extra_answers_().find(v.name); it != extra_answers_().end())
            for (const auto& a : it->second) v.answers.push_back(a);
        out.push_back(std::move(v));
    }
    return out;
}

Json describe_json() {
    Json verbs_json = Json::array();
    for (const auto& v : verbs()) {
        Json args = Json::array(), opts = Json::array();
        for (const auto& a : v.arguments)
            args.push_back({{"name", a.name}, {"description", a.description}, {"required", a.required}, {"repeated", a.repeated}});
        for (const auto& o : v.options) opts.push_back({{"name", o.name}, {"description", o.description}, {"flag", o.flag}});
        verbs_json.push_back({{"name", v.name}, {"summary", v.summary}, {"effect", std::string(openas::name_of(v.effect))},
                              {"arguments", std::move(args)}, {"options", std::move(opts)}, {"answers", v.answers}});
    }
    return {{"openas", std::string(openas::kVersion)}, {"client", "luban"}, {"version", std::string(kVersion)},
            {"verbs", std::move(verbs_json)}};
}

// The argv a verb's JSON arguments stand for: arguments by position (a
// repeated last one after `--`), options by name.
std::expected<std::vector<std::string>, std::string> argv_of(const openas::Verb& v, const Json& args) {
    if (!args.is_object()) return std::unexpected("the arguments are not a JSON object");
    std::vector<std::string> out{v.name}, after;
    std::set<std::string> used;
    const auto text = [](const Json& j) -> std::optional<std::string> {
        if (j.is_string()) return j.get<std::string>();
        if (j.is_number_integer()) return std::to_string(j.get<long long>());
        if (j.is_number()) return std::format("{}", j.get<double>());
        return std::nullopt;
    };
    for (const auto& a : v.arguments) {
        if (!args.contains(a.name)) {
            if (a.required) return std::unexpected(std::format("{} needs `{}`", v.name, a.name));
            continue;
        }
        used.insert(a.name);
        const auto& val = args[a.name];
        if (a.repeated) {
            if (!val.is_array()) return std::unexpected(std::format("`{}` is a list", a.name));
            for (const auto& one : val) {
                auto t = text(one);
                if (!t) return std::unexpected(std::format("`{}` holds a value that is not text", a.name));
                after.push_back(*t);
            }
            continue;
        }
        auto t = text(val);
        if (!t) return std::unexpected(std::format("`{}` is not text", a.name));
        out.push_back(*t);
    }
    for (const auto& o : v.options) {
        if (!args.contains(o.name)) continue;
        used.insert(o.name);
        const auto& val = args[o.name];
        if (o.flag) {
            if (!val.is_boolean()) return std::unexpected(std::format("`{}` is true or false", o.name));
            if (val.get<bool>()) out.push_back("--" + o.name);
            continue;
        }
        for (const auto& one : val.is_array() ? val : Json::array({val})) {
            auto t = text(one);
            if (!t) return std::unexpected(std::format("`{}` is not text", o.name));
            out.push_back("--" + o.name);
            out.push_back(*t);
        }
    }
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!used.contains(it.key())) return std::unexpected(std::format("{} has no `{}`", v.name, it.key()));
    if (!after.empty()) {
        out.push_back("--");
        out.insert(out.end(), after.begin(), after.end());
    }
    return out;
}

namespace {

// `luban interface`: the binding of the verbs for a program (part 4 §7.1).
// `--describe` prints the description; a verb runs as `luban --agent --json`
// with the arguments it was given, and what it reports comes back as openas
// envelope lines, ending with `result` (its exit code, the actor, the trace).
int interface_(std::span<const std::string> rest, const Globals& g) {
    const auto line = [](const Json& j) { std::println("{}", j.dump()); std::cout.flush(); };
    if (std::ranges::find(rest, std::string("--describe")) != rest.end()) {
        line(describe_json());
        return 0;
    }
    const auto fail = [&](std::string text, std::string hint, int code) {
        line({{"kind", "error"}, {"text", std::move(text)}, {"hint", std::move(hint)}, {"code", "E_INVALID_ARGS"}});
        line({{"kind", "result"}, {"exitCode", code}, {"actor", openas::format(principal(g))},
              {"trace", xlings::observe::ops::trace()}});
        return code;
    };
    auto parsed = parse(rest);
    if (!parsed || parsed->words.empty())
        return fail("which verb?", "luban interface --describe lists them; luban interface <verb> --args '{...}'", 1);
    const auto all = verbs();
    const auto it = std::ranges::find_if(all, [&](const openas::Verb& v) { return v.name == parsed->words[0]; });
    if (it == all.end()) return fail(std::format("no verb `{}`", parsed->words[0]), "luban interface --describe", 1);
    Json args = Json::object();
    if (auto text = parsed->option("--args")) args = Json::parse(*text, nullptr, false);
    else if (auto file = parsed->option("--args-file")) {
        std::ifstream in(*file);
        args = in ? Json::parse(in, nullptr, false) : Json();
    }
    if (args.is_discarded() || !args.is_object())
        return fail("the arguments are not a JSON object", "--args '{\"name\": \"box\"}'", 1);
    auto argv = argv_of(*it, args);
    if (!argv) return fail(argv.error(), std::format("luban interface --describe: what {} takes", it->name), 1);
    std::vector<std::string> child{xlings::platform::get_executable_path().string(), "--agent", "--json"};
    child.insert(child.end(), argv->begin(), argv->end());
    // Lines as they come, per stream: the verb's own events become envelope
    // lines; its result object is data; anything else is a log line.
    std::map<std::string, std::string, std::less<>> pending;
    const auto emit = [&](std::string_view stream, std::string_view text) {
        if (text.empty()) return;
        if (stream == "stdout") {
            auto j = Json::parse(text, nullptr, false);
            if (j.is_object() && j.contains("event") && j["event"].is_string()) {
                Json out = {{"kind", j["event"]}};
                for (auto f = j.begin(); f != j.end(); ++f)
                    if (f.key() != "event") out[f.key()] = f.value();
                line(out);
                return;
            }
            if (j.is_object() || j.is_array()) {
                line({{"kind", "data"}, {"data_kind", it->name}, {"data", j}});
                return;
            }
        }
        line({{"kind", "log"}, {"level", stream == "stderr" ? "warn" : "info"}, {"text", std::string(text)}});
    };
    const int rc = xlings::platform::stream::run(child, [&](std::string_view stream, std::string_view chunk) {
        auto& buf = pending[std::string(stream)];
        buf.append(chunk);
        for (auto nl = buf.find('\n'); nl != std::string::npos; nl = buf.find('\n')) {
            auto one = buf.substr(0, nl);
            if (!one.empty() && one.back() == '\r') one.pop_back();
            emit(stream, one);
            buf.erase(0, nl + 1);
        }
    });
    for (auto& [stream, rest_of] : pending) emit(stream, rest_of);
    line({{"kind", "result"}, {"exitCode", rc}, {"actor", openas::format(principal(g))}, {"trace", xlings::observe::ops::trace()}});
    return rc;
}

}  // namespace

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
            {"--as <ACTOR>", "Who this is (agent:<name>[;via=person:<name>]): recorded and inherited by what it runs", true, Level::Expert},
            {"-h, --help", "This help"},
            {"--version", "The luban version"},
        },
        .children = {
            {.name = "new", .description = "Make an environment",
             .arguments = {{"name", "Its name", true}},
             .options = {{"--edition <EDITION>", "An official system by its short name: core (default), desktop, tiny, nano, ..."},
                         {"--xpkg <COORD>", "Any package, from any index: a system (one), desktops, packages; repeat it", false, Level::More},
                         {"--desktop <SESSION>", "Its desktop (an official one is installed when it has none)"},
                         {"--enter <WHAT>", "console or desktop: what going in shows first", false, Level::More},
                         {"--mode <MODE>", "sandbox or machine", false, Level::More},
                         {"--persist <WHAT>", "all, home or none", false, Level::More},
                         {"--shell <SHELL>", "Its login shell", false, Level::More},
                         {"--login <NAME>", "Who luban enter logs in as", false, Level::Expert},
                         {"--on-close <WHAT>", "ask, background or poweroff", false, Level::Expert},
                         {"--policy <PRESET>", "dev, private, locked, or ns:name", false, Level::Expert},
                         {"--tz <ZONE>", "utc, proxy, or Area/City", false, Level::Expert},
                         {"--allow <GRANT>", "Grant a capability (clipboard, gpu, audio, ...)", false, Level::Expert},
                         {"--runtime <LIBC@VERSION>", "Its default runtime", false, Level::Expert},
                         {"--dry-run", "What it would be made of, and nothing made", false, Level::More},
                         {"--user <NAME>", "Its login user (a desktop needs one; default lubanoser)"},
                         {"--password-file <FILE>", "Its user's password, the file's first line", false, Level::More},
                         {"--no-password", "Its user logs in without a password, and its sudo asks nothing (otherwise: the default password lubanoser)", false, Level::More},
                         {"--proxy <URL>", "Its network goes only through this SOCKS5h proxy", false, Level::More},
                         {"--carrier <CARRIER>", "Where it runs: local, wsl2, vz", false, Level::Expert}}, .effect = "reversible"},
            {.name = "enter", .description = "Go into an environment, or boot an image",
             .arguments = {{"name", "An environment, or an image file (.iso, .img, .qcow2)", true}},
             .options = {{"--mode <MODE>", "sandbox (this machine's kernel) or machine (its own); default: its setting"},
                         {"--temp", "Nothing it writes stays"},
                         {"--user <NAME>", "Log in as this user this once (root, or one of its users)", false, Level::More},
                         {"--display <DISPLAY>", "terminal or window; default: its desktop decides", false, Level::More},
                         {"--desktop <SESSION>", "This desktop, this once, in a window", false, Level::More}}, .effect = "reversible"},
            {.name = "run", .description = "Run one command in an environment",
             .arguments = {{"name", "Which", true}, {"command", "The command and its arguments, after --", true, true}},
             .options = {{"--user <NAME>", "As this user (root, or one of its users)", false, Level::More},
                         {"--display <DISPLAY>", "terminal (here) or window: a graphical program alone in a window of its own", false,
                          Level::More}}, .effect = "reversible"},
            {.name = "ls", .description = "List the environments, or those whose name matches", .aliases = {"list"},
             .arguments = {{"pattern", "A name or part of one"}},
             .options = {{"--all", "And what cancelled makings set aside"}}, .effect = "read"},
            {.name = "search", .description = "What the index offers: systems to make, desktops to give one",
             .arguments = {{"keyword", "Part of a name, title or description"}},
             .options = {{"--distributions", "Only the systems that can be made"},
                         {"--desktops", "Only the desktops a system can have"}}, .effect = "read"},
            {.name = "status", .description = "This host, or one environment", .arguments = {{"name", "Which"}},
             .options = {{"--check", "Check that it is usable: its /usr, /etc, runtime closure, and programs"},
                         {"--log", "What its latest operation did, its own record of it too"},
                         {"--bus", "What is on its bus: what its programs offer; the host's: the systems linked under it"}}, .effect = "read"},
            {.name = "config", .description = "Show or change an environment's settings",
             .arguments = {{"name", "Which", true}},
             .options = {{"--mode <MODE>", "sandbox or machine: how `luban enter` starts it"},
                         {"--persist <WHAT>", "all, home (only /home stays) or none: what a session keeps"},
                         {"--shell <SHELL>", "Its login shell (fish, bash, sh, or a path)"},
                         {"--login <NAME>", "Who luban enter logs in as: root, or one of its users", false, Level::More},
                         {"--runtime <LIBC@VERSION>", "Its default runtime (glibc@2.44.3): a new generation runs on it", false, Level::More},
                         {"--desktop <SESSION>", "The desktop it starts, or none"},
                         {"--on-close <WHAT>", "ask, background or poweroff: closing its window"},
                         {"--enter <WHAT>", "console or desktop: what going in shows first"},
                         {"--policy <PRESET>", "dev, private, locked, or ns:name"},
                         {"--proxy <URL>", "Its only network is this SOCKS5h proxy; off for none"},
                         {"--tz <ZONE>", "utc, proxy, or Area/City"},
                         {"--allow <GRANT>", "Grant a capability (clipboard, gpu, audio, ...)"},
                         {"--disallow <GRANT>", "Take a grant back"},
                         {"--mount <HOST[:INSIDE][:ro|rw]>", "Map a host directory into it"},
                         {"--unmount <INSIDE>", "Remove a mapping"},
                         {"--fetch <ACTION>", "auto, ask or deny: installing from inside", false, Level::Expert},
                         {"--observe <LEVEL>", "off, basic, standard or full", false, Level::Expert}},
             .level = Level::More, .effect = "reversible"},
            {.name = "stop", .description = "Stop a running environment or machine", .arguments = {{"name", "Which", true}},
             .level = Level::More, .effect = "reversible"},
            {.name = "history", .description = "An environment's generations", .arguments = {{"name", "Which"}},
             .level = Level::More, .effect = "read"},
            {.name = "upgrade", .description = "Move an environment to its edition's newest version",
             .arguments = {{"name", "Which", true}},
             .options = {{"--to <VERSION>", "This version of the edition"}, {"--dry-run", "Show the plan only"}},
             .level = Level::More, .effect = "reversible"},
            {.name = "rollback", .description = "Go back a generation", .arguments = {{"name", "Which"}},
             .options = {{"--to <N>", "This generation, or `relayout`: what it was before its move into its own userland"}},
             .level = Level::More, .effect = "reversible"},
            {.name = "export", .description = "Write it out: .iso, .img, .qcow2, .tar(.zst), .luban, or a directory",
             .arguments = {{"name", "Which", true}, {"file", "Where; the format follows the name", true}},
             .options = {{"--format <FORMAT>", "iso, img, qcow2, tar, luban or dir, whatever the name says"},
                         {"--size <SIZE>", "Disk size (img, qcow2)"},
                         {"--boot <PROFILE>", "How an image boots: generic (machines), virt (VMs), or ns:name"}},
             .level = Level::More, .effect = "reversible"},
            {.name = "import", .description = "Make an environment from a .luban someone exported",
             .arguments = {{"file", "The .luban file", true}},
             .options = {{"--name <NAME>", "Its name here (default: the one it had)"}},
             .level = Level::More, .effect = "reversible"},
            {.name = "write", .description = "Make a boot drive from an image or an environment",
             .arguments = {{"source", "An image file, or an environment", true}, {"device", "The drive, e.g. /dev/sdb", true}},
             .options = {{"--serial <SERIAL>", "The drive's serial (required with --agent)"}},
             .level = Level::More, .effect = "irreversible"},
            {.name = "rm", .description = "Remove an environment (asks first)", .aliases = {"remove"},
             .arguments = {{"name", "Which"}},
             .options = {{"--unfinished", "What cancelled or failed makings set aside"}},
             .level = Level::More, .effect = "irreversible"},
            {.name = "setup", .description = "This host's one-time setup (isolation)", .level = Level::More, .effect = "external"},
            {.name = "help", .description = "Help; --all for every command",
             .options = {{"--all", "Every command"}, {"--expert", "And the expert ones"}}, .level = Level::More},
            {.name = "boot", .description = "This machine's boot entries", .arguments = {{"name", "An environment"}},
             .options = {{"--once", "Only the next boot (a trial)"}, {"--fallback", "When the default fails"},
                         {"--mark-good", "This boot worked"}, {"--now", "Switch to it now"}},
             .level = Level::Expert, .effect = "external"},
            {.name = "interface", .description = "For programs: the verbs described (openas), or one run with JSON arguments, reported as NDJSON",
             .arguments = {{"verb", "The verb to run"}},
             .options = {{"--describe", "Describe every verb: its arguments, options, effect and what answers it"},
                         {"--args <JSON>", "The verb's arguments and options, by name"},
                         {"--args-file <FILE>", "The same, read from a file"}},
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

// `luban __desktop <session | -> [--size WxH] [--socket PATH]`: a desktop
// session headless (or a compositor already running at PATH), bridged by
// opensal-wlr with opensal.wire on stdin and stdout -- what a sandbox runs for
// the Luban OS window. `-` is the first session the environment declares.
int desktop_(const std::vector<std::string>& rest) {
    namespace wlr = opensal::wlr;
    std::string id = rest.size() > 1 ? rest[1] : "-";
    std::string socket;
    opensal::Size size{1280, 800};
    for (std::size_t i = 2; i + 1 < rest.size(); i += 2) {
        if (rest[i] == "--socket") socket = rest[i + 1];
        else if (rest[i] == "--size") {
            unsigned w = 0, h = 0;
            if (std::sscanf(rest[i + 1].c_str(), "%ux%u", &w, &h) == 2 && w && h) size = {w, h};
        }
    }
    auto log = [](std::string_view line) { std::println(std::cerr, "luban __desktop: {}", line); };
    if (!socket.empty()) return wlr::bridge({.in = 0, .out = 1, .socket = socket, .log = log});
    auto env = xlings::platform::environment();
    const auto dirs = wlr::session_dirs(env);
    std::optional<wlr::Session> session;
    if (id == "-") { if (auto all = wlr::sessions(dirs); !all.empty()) session = all.front(); }
    else session = wlr::resolve_session(id, dirs);
    if (!session) {
        log(id == "-" ? std::string("no desktop session is declared here (share/wayland-sessions)")
            : id.starts_with(wlr::kAppSession) ? std::string("nothing here runs one application in a window (luban:compositor does: "
                                                             "xlings install luban:compositor in it)")
                                               : std::format("no desktop session {} here", id));
        return 127;
    }
    wlr::SessionOptions o;
    o.size = size;
    o.env = std::move(env);
    const fs::path runtime = o.env.contains("XDG_RUNTIME_DIR") ? fs::path(o.env.at("XDG_RUNTIME_DIR")) : fs::temp_directory_path();
    o.runtime_root = runtime / "luban";
    return wlr::run_session(*session, 0, 1, o);
}

// `luban __audio [FIFO]`: a system's sound, out to the window that shows it
// (design 2026-10-20 §20.1): the PCM its sound server writes into FIFO
// (luban:audio's `luban` sink: /tmp/luban-audio.pcm, 48 kHz stereo, signed
// 16-bit), as openhal.audio on standard output -- one output stream. Ends
// when its standard input does (the window went).
int audio_(const std::vector<std::string>& rest) {
    namespace platform = xlings::platform;
    namespace audio = openhal::audio;
    const std::filesystem::path fifo = rest.size() > 1 ? rest[1] : "/tmp/luban-audio.pcm";
    if (!platform::make_fifo(fifo, 0666)) {
        std::println(std::cerr, "luban __audio: no named pipe at {}", fifo.string());
        return 1;
    }
    // Read and write: no end of it while the sound server comes and goes.
    const int fd = platform::open_device(fifo);
    if (fd < 0) {
        std::println(std::cerr, "luban __audio: cannot open {}", fifo.string());
        return 1;
    }
    const audio::Format format{};
    if (!platform::write_all(1, audio::encode(audio::Open{.stream = 1, .format = format}))) return 0;
    std::array<char, 9600> buf{};      // 50 ms
    std::string carry;                  // a frame cut short, until the rest comes
    for (;;) {
        std::array<platform::PollFd, 2> p{{{.fd = 0}, {.fd = fd}}};
        platform::poll_fds(p, 1000);
        // What the server wrote first, then whether the window is still there.
        if (p[1].readable) {
            if (auto n = platform::read_some(fd, buf); n && *n > 0) {
                carry.append(buf.data(), *n);
                const auto whole = carry.size() - carry.size() % format.frame_bytes();
                if (whole > 0) {
                    if (!platform::write_all(1, audio::encode(audio::Samples{.stream = 1, .bytes = carry.substr(0, whole)}))) break;
                    carry.erase(0, whole);
                }
            }
        }
        if (p[0].closed || p[0].readable) {
            auto n = platform::read_some(0, buf);
            if (!n || *n == 0) break;
        }
    }
    std::vector<int> fds{fd};
    platform::close_fds(fds);
    return 0;
}

int run(int argc, char* argv[]) {
    // Its words, before anything is said (design 2026-10-19 §7).
    svc::register_words();
    std::vector<std::string> args(argv + 1, argv + argc);
    Globals g;
    std::vector<std::string> rest;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--") { rest.insert(rest.end(), args.begin() + static_cast<std::ptrdiff_t>(i), args.end()); break; }
        if (a == "--as" && i + 1 < args.size()) { g.as = args[++i]; continue; }
        if (a.starts_with("--as=")) { g.as = a.substr(5); continue; }
        if (a == "--json") g.json = true;
        else if (a == "--agent") g.agent = true;
        else if (a == "-y" || a == "--yes") g.yes = true;
        else if (a == "-v" || a == "--verbose") g.verbose = true;
        else if (a == "-q" || a == "--quiet") g.quiet = true;
        else rest.push_back(a);
    }
    if (const char* mode = std::getenv("XLINGS_AGENT_MODE"); mode && std::string_view(mode) == "1") g.agent = true;
    // A carrier's guest is brought to this release (luban and xlings are released together).
    xlings::carrier::set_client_version(std::string(kVersion));

    // Tab completion (design 2026-10-20 §4.8, G17): the words before the
    // cursor in, the candidates from the command tree out -- the systems'
    // names for the arguments that take one. fish passes the program name.
    if (!rest.empty() && rest.front() == "__complete") {
        std::vector<std::string> words(rest.begin() + 1, rest.end());
        if (!words.empty() && (words.front() == "luban" || words.front() == "luban.exe")) words.erase(words.begin());
        const auto systems = [](const xlings::cli::completion::Request& r) {
            std::vector<xlings::cli::completion::Candidate> out;
            if (r.argument.empty()) return out;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(home_dir_() / "subos", ec))
                if (e.is_directory(ec) && e.path().filename() != "current" && !e.path().filename().string().starts_with("."))
                    out.push_back({e.path().filename().string(), {}});
            return out;
        };
        for (const auto& c : xlings::cli::completion::complete_in(tree(), words, systems)) {
            if (c.description.empty()) std::println("{}", c.value);
            else std::println("{}\t{}", c.value, c.description);
        }
        return 0;
    }
    // Plumbing first: what qemu, the drive writer and the docs tools run.
    if (!rest.empty() && rest.front() == "__write" && rest.size() == 3) return raw_write_(rest[1], rest[2]);
    if (!rest.empty() && rest.front() == "__pipe" && rest.size() == 3) return pipe_(rest[1], rest[2]);
    if (!rest.empty() && rest.front() == "__vm-supervise" && rest.size() == 3) return vm::supervise(rest[1], rest[2]);
    if (!rest.empty() && rest.front() == "__bus-router" && rest.size() == 2) return vm::serve_bus(rest[1]);
    if (!rest.empty() && rest.front() == "__vm") return vm::host_machine(home_dir_(), std::span<const std::string>(rest).subspan(1));
    if (!rest.empty() && rest.front() == "__agent-stdio") vm::serve_stdio();
    if (!rest.empty() && rest.front() == "__pipe-unix" && rest.size() == 2) return pipe_unix_(rest[1]);
    if (!rest.empty() && rest.front() == "__desktop") return desktop_(rest);
    if (!rest.empty() && rest.front() == "__audio") return audio_(rest);
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
        const auto who = openas::format(principal(g));
        xlings::observe::ops::begin(c.home, trace,
                                    Json{{"client", "luban"}, {"version", std::string(kVersion)},
                                         {"argv", xlings::observe::ops::redact_argv(rest)},
                                         {"actor", g.agent ? "agent" : "cli"}, {"principal", who},
                                         {"principal_source", std::string(principal_source(g))}},
                                    !(inherited && trace == inherited));
        xlings::platform::set_env_variable("XLINGS_TRACE_ID", trace);
        // What this runs acts as this does (openas §2.5), and names this step
        // as its cause.
        xlings::platform::set_env_variable("XLINGS_ACTOR", who);
        if (const auto proc = xlings::observe::ops::proc(); !proc.empty())
            xlings::platform::set_env_variable("XLINGS_OPS_PARENT", proc);
    }
    const int rc = [&]() -> int {
        if (rest.empty()) return overview(c, g);
        const auto& cmd = rest.front();
        // Plumbing: start a machine here, its events on this output (what a
        // PC's luban relays from its host machine).
        if (cmd == "__machine-start" && rest.size() >= 2)
            return vm::start(c, rest[1], {.temp = std::ranges::find(rest, std::string("--temp")) != rest.end()});
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
        if (p.command == "run") return cmd_run(c, p, g);
        if (p.command == "ls") {
            if (p.words.size() > 1) return usage("`luban ls [<pattern>] [--all]`", g);
            return overview(c, g, p.words.empty() ? std::string_view{} : std::string_view(p.words[0]), p.flag("--all"));
        }
        if (p.command == "search") {
            if (p.words.size() > 1) return usage("`luban search [<keyword>] [--distributions | --desktops]`", g);
            if (p.flag("--distributions") && p.flag("--desktops")) return usage("--distributions or --desktops, not both", g);
            return search(c, p.words.empty() ? std::string_view{} : std::string_view(p.words[0]),
                          p.flag("--distributions") ? "distributions" : p.flag("--desktops") ? "desktops" : "", g.json, g.verbose);
        }
        if (p.command == "status") return cmd_status(c, p, g, render);
        if (p.command == "config") return cmd_config(c, p, g, render);
        if (p.command == "stop") {
            if (p.words.size() != 1) return usage("`luban stop <name>`", g);
            if (vm::running(c.home, p.words[0])) return vm::stop(c, p.words[0]).code;
            return svc::stop(c, p.words[0]).code;
        }
        if (p.command == "history") return cmd_history(c, p, g, render);
        if (p.command == "upgrade") return cmd_upgrade(c, p, g, render);
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
        if (p.command == "setup") {
            // Its confirmation (the one sudo step) is luban's -y too.
            std::vector<std::string> fix{"self", "doctor", "--isolation", "--fix"};
            if (c.yes) fix.push_back("-y");
            return c.port.terminal(fix);
        }
        if (p.command == "boot") return cmd_boot(c, p, g, render);
        if (p.command == "interface") return interface_(rest, g);
        return unknown_command(cmd, g);
    }();
    xlings::observe::ops::end(rc);
    return rc;
}

}  // namespace luban::cli
