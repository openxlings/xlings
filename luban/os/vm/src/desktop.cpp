// A desktop that runs by itself (vm.cppm; design 2026-10-24 §4, §6.2): one
// supervisor a running desktop -- `luban __desktop-session <home> <name>`,
// detached, as a machine's supervisor is -- that starts the system's desktop
// on a virtual display of its own and lets other displays come and go:
//
//   host/<id>/device/display/<n>          the virtual display (openhal 0.3 §6.2):
//   host/<id>/device/display/<n>/control  describe, capture, record, mode, input
//   host/<id>/device/input/<n>            its input (openhal 0.3 §6.6)
//   host/<id>/system/<bus>/service/desktop   luban.desktop@1: status, attach,
//                                            detach, end
//   host/<id>/system/<bus>/topic/desktop/events   started, display-attach,
//                                            display-detach, ended (with why)
//
// The desktop itself reaches each display through its system's socket on the
// bus (`luban __desktop <session> --display <path>`, run in the system): the
// first is the virtual display; a window attached later asks to be the
// primary, and when it goes the desktop goes on on the virtual display.
module luban.vm;

import std;
import xlings.libs.json;
import xlings.observe;
import xlings.platform;
import xlings.subos.home_view;
import xlings.subos.located;
import luban.service;
import luban.vm.protocol;
import openbus;
import xbus.node;
import openhal.display;
import openhal.emu;
import opensal;
import opensal.wire;

namespace luban::vm {

namespace {

using Json = nlohmann::json;
namespace platform = xlings::platform;
namespace hd = openhal::display;

fs::path desktop_dir_(const fs::path& home, std::string_view name) { return home / "run" / "desktops" / std::string(name); }
fs::path desktop_log_(const fs::path& home, std::string_view name) { return home / "logs" / "desktops" / std::string(name); }

std::string service_path_(std::string_view name) { return system_bus_root(name) + "/service/desktop"; }
std::string events_path_(std::string_view name) { return system_bus_root(name) + "/topic/desktop/events"; }

std::expected<std::shared_ptr<xbus::Node>, std::string> host_node_(const fs::path& home, std::string node_name) {
    // The router, started when nobody answers (connect_bus).
    auto fd = connect_bus(home);
    if (!fd) return std::unexpected(fd.error());
    std::vector<int> close{*fd};
    platform::close_fds(close);
    return xbus::Node::connect({.name = std::move(node_name), .actor = "program:luban",
                                .router = xbus::Address{"unix", bus_socket(home).string()}, .telemetry = false});
}

// The virtual display on the bus: a stream endpoint whose openings are the
// desktop attaching, its control and its input.
struct VirtualOnBus {
    std::shared_ptr<xbus::Node> node;
    std::shared_ptr<openhal::emu::VirtualDisplay> vd;
    std::string path;
    std::mutex m;
    std::optional<xbus::Stream> stream;     // the desktop, attached
    bool hidden { false };                  // told it is not seen (a mirror nobody captures)

    bool send(std::string_view bytes) {
        std::optional<xbus::Stream> s;
        { std::lock_guard l(m); s = stream; }
        return s && s->write(bytes);
    }
    void show(bool shown) {
        {
            std::lock_guard l(m);
            if (hidden == !shown) return;
            hidden = !shown;
        }
        const std::vector<opensal::Event> e{opensal::Shown{shown}};
        (void)send(openhal::emu::VirtualDisplay::encode(e));
    }
    // A picture as it is now: shown again for a moment when it was hidden.
    std::string capture() {
        bool was_hidden = false;
        { std::lock_guard l(m); was_hidden = hidden; }
        if (was_hidden) {
            const auto before = vd->frames();
            show(true);
            for (int k = 0; k < 50 && vd->frames() == before; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        auto png = vd->capture();
        if (was_hidden) show(false);
        return png;
    }
};

// Bytes larger than a frame's payload (openbus §4.6: 1 MiB), as replies of
// at most 768 KiB in order; the caller puts them together.
void reply_bytes_(const xbus::Call& c, std::string_view bytes) {
    constexpr std::size_t kPiece = 768u << 10;
    for (std::size_t at = 0; at < bytes.size(); at += kPiece) c.reply(bytes.substr(at, kPiece));
}

std::expected<std::shared_ptr<VirtualOnBus>, std::string> offer_virtual_(std::shared_ptr<xbus::Node> node, hd::Mode mode,
                                                                          const std::string& system) {
    auto v = std::make_shared<VirtualOnBus>();
    v->node = node;
    v->vd = std::make_shared<openhal::emu::VirtualDisplay>(mode);
    const auto root = node->root();
    std::weak_ptr<VirtualOnBus> weak = v;
    for (std::uint32_t n = 0; n < 64; ++n) {
        const auto path = hd::device_path(root, n);
        xbus::OfferOptions o{.kind = openbus::EndpointKind::stream, .verb = "display", .effect = "reversible",
                             .interfaces = {std::string(hd::kInterface)},
                             .properties = {{"display-kind", "virtual"}, {"mode", hd::format_mode(mode)}, {"refresh", "60"},
                                            {"input", "1"}, {"system", system}, {"name", "virtual display of " + system}}};
        auto offered = node->offer_stream(path, o, [weak](xbus::Stream s) {
            auto self = weak.lock();
            if (!self) { s.close(); return; }
            {
                std::lock_guard l(self->m);
                if (self->stream && !self->stream->ended()) { s.close(); return; }   // one desktop draws on it
                self->stream = s;
                self->hidden = false;
            }
            std::thread([self, s]() mutable {
                for (;;) {
                    auto chunk = s.read(std::chrono::seconds(1));
                    if (!chunk) {
                        if (s.ended()) break;
                        continue;
                    }
                    const auto back = self->vd->feed(chunk->bytes);
                    if (!back.empty() && !s.write(back)) break;
                    // A mirror nobody looks at is sent nothing (design 2026-10-24 §4.2).
                    if (const auto r = self->vd->role(); r && r->kind == opensal::DisplayRole::Kind::mirror) self->show(false);
                    else if (r) self->show(true);
                }
                std::lock_guard l(self->m);
                self->stream.reset();
            }).detach();
        });
        if (!offered) {
            if (offered.error().code == openbus::ErrorCode::taken) continue;
            return std::unexpected("cannot offer a virtual display: " + offered.error().message);
        }
        v->path = path;
        break;
    }
    if (v->path.empty()) return std::unexpected(std::string("64 displays are offered here already"));
    // Its control (openhal 0.3 §6.5) and its input (§6.6).
    const auto control = [weak](xbus::Call c) {
        auto self = weak.lock();
        if (!self) { c.fail(openbus::ErrorCode::gone, "the display went"); return; }
        const auto j = Json::parse(c.body(), nullptr, false);
        const auto op = j.is_object() ? j.value("op", std::string("describe")) : std::string("describe");
        if (op == "describe") {
            Json d{{"kind", "virtual"}, {"mode", hd::format_mode(self->vd->mode())}, {"frames", self->vd->frames()},
                   {"attached", static_cast<bool>(self->stream)}};
            if (const auto r = self->vd->role())
                d["role"] = r->kind == opensal::DisplayRole::Kind::mirror ? std::format("mirror {}", r->of) : std::string("primary");
            c.reply(d.dump(), static_cast<std::uint16_t>(openbus::Codec::json));
        } else if (op == "capture") {
            reply_bytes_(c, self->capture());
        } else if (op == "record") {
            const int n = std::clamp(j.value("frames", 30), 1, 600);
            auto seen = self->vd->frames();
            self->show(true);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(std::clamp(j.value("seconds", 10), 1, 600));
            for (int k = 0; k < n && std::chrono::steady_clock::now() < until && !c.cancelled();) {
                if (self->vd->frames() == seen) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
                seen = self->vd->frames();
                // Each frame: a header (its number, size, when), then its PNG.
                const auto png = self->vd->capture();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                c.reply(Json{{"frame", k}, {"bytes", png.size()}, {"t_ms", ms}}.dump(), static_cast<std::uint16_t>(openbus::Codec::json));
                reply_bytes_(c, png);
                ++k;
            }
        } else if (op == "mode") {
            const auto m = hd::parse_mode(j.value("mode", std::string{}));
            if (!m) { c.fail(openbus::ErrorCode::malformed, "a mode is <w>x<h>[@<scale>]"); return; }
            (void)self->send(self->vd->set_mode(*m));
            c.reply(Json{{"mode", hd::format_mode(*m)}}.dump(), static_cast<std::uint16_t>(openbus::Codec::json));
        } else if (op == "input") {
            auto events = openhal::emu::input_events(j.contains("events") ? j["events"].dump() : std::string("[]"), self->vd->mode().scale);
            if (!events) { c.fail(openbus::ErrorCode::malformed, events.error()); return; }
            if (!self->send(openhal::emu::VirtualDisplay::encode(*events))) { c.fail(openbus::ErrorCode::gone, "no desktop draws on it"); return; }
            c.reply(Json{{"events", events->size()}}.dump(), static_cast<std::uint16_t>(openbus::Codec::json));
        } else {
            c.fail(openbus::ErrorCode::malformed, "describe, capture, record, mode or input");
            return;
        }
        c.end();
    };
    const auto number = v->path.substr(v->path.rfind('/') + 1);
    (void)node->offer(hd::control_path(v->path), {.verb = "display-control", .effect = "reversible", .interfaces = {std::string(hd::kControlInterface)}},
                      [control](xbus::Call c) { std::thread([control, c = std::move(c)]() mutable { control(std::move(c)); }).detach(); });
    (void)node->offer(root + "/device/input/" + number, {.verb = "input", .effect = "reversible", .interfaces = {std::string(hd::kInputInterface)}},
                      [control](xbus::Call c) {
                          // Its body is the steps: as the control's input.
                          std::thread([control, c = std::move(c)]() mutable { control(std::move(c)); }).detach();
                      });
    return v;
}

// A command started for the desktop (in the system): its process -- or, in a
// machine, the channel it runs on (its end said when it comes).
struct InMachine {
    std::optional<Client> client;
    std::atomic<int> code { -1 };     // -1: running
    std::thread waiting;
    ~InMachine() { if (waiting.joinable()) waiting.join(); }
};
struct Started {
    int pid { -1 };
    std::string display;       // the bus path it shows on
    std::string role;
    std::string kind;          // virtual | window | ...
    std::shared_ptr<InMachine> machine;
};

std::optional<int> spawn_(const std::vector<std::string>& argv, const std::map<std::string, std::string>& env, const fs::path& log) {
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    const int out = platform::open_for_append(log);
    const int none = platform::open_null();
    const int pid = platform::fork_process();
    if (pid == 0) {
        platform::new_session();
        const std::array<int, 3> stdio{none, out, out};
        platform::redirect_stdio(stdio);
        platform::exec_program(argv, env);
        platform::exit_now(127);
    }
    std::vector<int> fds;
    if (out >= 0) fds.push_back(out);
    if (none >= 0) fds.push_back(none);
    platform::close_fds(fds);
    if (pid < 0) return std::nullopt;
    return pid;
}

std::string quoted_(std::string_view w) {
    std::string out = "'";
    for (char ch : w) out += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
    return out + "'";
}

}  // namespace

std::expected<Json, std::string> desktop(const fs::path& home, std::string_view name, const Json& request) {
    auto node = host_node_(home, "luban");
    if (!node) return std::unexpected(node.error());
    auto answer = (*node)->call(service_path_(name), request.dump(), std::chrono::seconds(30),
                                static_cast<std::uint16_t>(openbus::Codec::json));
    (*node)->close();
    if (!answer) {
        if (answer.error().code == openbus::ErrorCode::not_found) return std::unexpected(std::format("{}'s desktop is not running", name));
        return std::unexpected(answer.error().message);
    }
    if (answer->empty()) return Json::object();
    auto j = Json::parse(answer->front(), nullptr, false);
    if (j.is_discarded()) return std::unexpected(std::string("the desktop's answer is not JSON"));
    return j;
}

std::expected<Json, std::string> desktop_start(const fs::path& home, std::string_view name, const DesktopOptions& o) {
    if (auto running = desktop(home, name, {{"op", "status"}})) return running;
    std::vector<std::string> argv{luban_binary(home).string(), "__desktop-session", home.string(), std::string(name),
                                  "--mode", hd::format_mode(o.mode)};
    if (!o.session.empty()) argv.insert(argv.end(), {"--session", o.session});
    if (!o.persist.empty()) argv.insert(argv.end(), {"--persist", o.persist});
    if (o.here) argv.push_back("--here");
    const auto log = desktop_log_(home, name) / "supervisor.log";
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    if (!platform::spawn_detached(argv, log)) return std::unexpected(std::format("the desktop's supervisor could not be started ({})", log.string()));
    // A machine boots first (minutes on a cold start, with no KVM).
    const auto settings = service::read_settings(home, name);
    const bool machine = !o.here && (running(home, name) || (settings && settings->mode == "machine"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(machine ? 300 : 60);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (auto st = desktop(home, name, {{"op", "status"}})) return st;
    }
    return std::unexpected(std::format("{}'s desktop did not come up; its log: {}", name, log.string()));
}

int desktop_supervise(const fs::path& home, std::string_view name, std::span<const std::string> args) {
    hd::Mode mode{1920, 1080, 1.0};
    std::string session = "-", persist;
    // --here: the desktop runs in this host's environment (its sessions
    // there), not in the system -- still on the system's socket, its nodes
    // under the system's path: what a test of the displays runs.
    bool here = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--here") { here = true; continue; }
        if (i + 1 >= args.size()) break;
        if (args[i] == "--mode") { if (auto m = hd::parse_mode(args[i + 1])) mode = *m; }
        else if (args[i] == "--session") session = args[i + 1];
        else if (args[i] == "--persist") persist = args[i + 1];
        ++i;
    }
    const auto say = [&](std::string_view line) { std::println(std::cerr, "{} luban __desktop-session {}: {}", xlings::observe::utc_now(), name, line); };
    auto node = host_node_(home, "luban-desktop-session");
    if (!node) { say(node.error()); return 1; }
    // Its system's socket: what its programs reach the bus through (§3.3).
    const auto bus_name = system_bus_root(name).substr(system_bus_root(name).rfind('/') + 1);
    auto opened = (*node)->call("bus/system", bus_name, std::chrono::seconds(10));
    if (!opened || opened->empty()) { say("no socket for the system: " + (opened ? std::string("no answer") : opened.error().message)); return 1; }
    const auto socket = Json::parse(opened->front(), nullptr, false).value("socket", std::string{});
    auto virt = offer_virtual_(*node, mode, std::string(name));
    if (!virt) { say(virt.error()); return 1; }
    say("its virtual display is " + (*virt)->path + " (" + hd::format_mode(mode) + ")");
    // A machine's desktop is the machine's (started when it is not running):
    // its programs reach the bus through its agent's router, linked to this one.
    const auto settings = service::read_settings(home, name);
    const bool machine = !here && (running(home, name) || (settings && settings->mode == "machine"));
    if (machine && !running(home, name)) {
        service::Context ctx{.home = home, .port = service::Port(service::xlings_path())};
        ctx.sink = [&](const service::Event& e) { if (!e.text.empty()) say(e.text); };
        if (start(ctx, name, EnterOptions{}) != 0) { say("the machine did not start"); return 1; }
    }
    // Its frames by reference (opensal 0.7 Region; design 2026-10-24 §5.1,
    // §5.4): a file the desktop writes its rows into and every display here
    // reads -- a machine's ivshmem memory, else a file in the system's own home
    // (writable where the system runs), bound into it.
    std::size_t kFramesBytes = std::size_t{128} << 20;
    const auto at = xlings::subos::locate(xlings::subos::HomeView{home}, name);
    auto frames_file = at.home.home / "run" / "desktops" / std::string(name) / "frames";
    if (machine) {
        if (const auto f = started_frames(home, name)) { frames_file = f->first; kFramesBytes = static_cast<std::size_t>(f->second); }
        else frames_file.clear();
    }
    std::error_code fec;
    if (!frames_file.empty()) fs::create_directories(frames_file.parent_path(), fec);
    auto frames = frames_file.empty() ? std::nullopt : platform::map_shared_file(frames_file, kFramesBytes);
    struct Unmap { std::optional<platform::SharedMemory>& f; ~Unmap() { if (f) platform::release_shared_memory(*f); } } unmap{frames};
    if (frames) (*virt)->vd->set_region(std::span<const std::byte>(frames->data, frames->size));
    else say("no frames region (" + frames_file.string() + "): frames go on the stream");

    std::mutex m;
    std::vector<Started> started;
    std::optional<platform::ExitStatus> ended;
    std::string ended_by;
    std::atomic<bool> stop{false};
    const auto when = xlings::observe::utc_now();
    const auto log = desktop_log_(home, name) / "session.log";
    // A display of the system's desktop: `luban __desktop <session> --display
    // <path>` in the system, through its socket. The first starts the desktop;
    // the others join it (its displays socket, opensal-wlr).
    std::uint32_t next_channel = 10;
    std::vector<std::shared_ptr<InMachine>> machine_runs;
    // In a machine: `luban __desktop` there, on a channel of its supervisor.
    const auto start_in_machine = [&](const std::string& display, const std::string& role) -> std::expected<std::shared_ptr<InMachine>, std::string> {
        auto run = std::make_shared<InMachine>();
        auto client = Client::connect(home, name);
        if (!client) return std::unexpected(client.error());
        run->client.emplace(std::move(*client));
        const auto ch = next_channel++;
        if (!run->client->open_command(ch, {"luban", "__desktop", session, "--size", std::format("{}x{}", mode.width, mode.height), "--display",
                                            display, "--role", role}, 0, 0, Json::object(), true))
            return std::unexpected(std::string("the machine did not take the command"));
        run->waiting = std::thread([run, ch, log] {
            for (;;) {
                auto frames = run->client->poll(500);
                if (!frames) { run->code = 1; return; }
                for (auto& [type, body] : *frames) {
                    if (body.first != ch) continue;
                    if (type == 'E' || type == 'D') std::ofstream(log, std::ios::app) << body.second;
                    if (type == 'X') {
                        const auto j = Json::parse(body.second, nullptr, false);
                        run->code = j.is_object() ? j.value("code", 0) : 0;
                        return;
                    }
                }
            }
        });
        return run;
    };
    const auto start_on = [&](const std::string& display, const std::string& role) -> std::expected<int, std::string> {
        if (machine) {
            auto run = start_in_machine(display, role);
            if (!run) return std::unexpected(run.error());
            machine_runs.push_back(*run);
            return -static_cast<int>(machine_runs.size());   // its run, not a pid
        }
        if (here) {
            auto env = platform::environment();
            env["OPENBUS_ROUTER"] = "unix:" + socket;
            if (frames) {
                env["OPENSAL_REGION"] = frames_file.string();
                env["OPENSAL_REGION_SIZE"] = std::to_string(kFramesBytes);
            }
            const auto pid = spawn_({luban_binary(home).string(), "__desktop", session, "--size", std::format("{}x{}", mode.width, mode.height),
                                     "--display", display, "--role", role}, env, log);
            if (!pid) return std::unexpected(std::string("the desktop's command could not be started"));
            return *pid;
        }
        const auto xlings = service::xlings_path();
        const auto region = frames ? std::format("OPENSAL_REGION=/run/luban/frames OPENSAL_REGION_SIZE={} ", kFramesBytes) : std::string{};
        const auto inner = std::format("{}OPENBUS_ROUTER=unix:/run/openbus/router.sock exec {} __desktop {} --size {}x{} --display {} --role {}",
                                       region, quoted_(luban_binary(home).string()), quoted_(session), mode.width, mode.height, quoted_(display),
                                       quoted_(role));
        std::vector<std::string> argv{xlings.string(), "subos", "use", std::string(name)};
        if (!persist.empty() && persist != "all") argv.insert(argv.end(), {"--persist", persist});
        argv.insert(argv.end(), {"--mount", socket + ":/run/openbus/router.sock"});
        if (frames) argv.insert(argv.end(), {"--mount", frames_file.string() + ":/run/luban/frames:rw"});
        argv.insert(argv.end(), {"--cmd", inner});
        auto env = platform::environment();
        env["XLINGS_HOME"] = home.string();
        env.erase("XLINGS_SUBOS_MODE");
        env.erase("XLINGS_ACTIVE_SUBOS");
        const auto pid = spawn_(argv, env, log);
        if (!pid) return std::unexpected(std::string("the desktop's command could not be started"));
        return *pid;
    };
    const auto publish = [&](std::string event, Json fields) {
        fields["event"] = std::move(event);
        fields["at"] = xlings::observe::utc_now();
        (*node)->publish(events_path_(name), fields.dump(), static_cast<std::uint16_t>(openbus::Codec::json));
    };
    (void)(*node)->offer(events_path_(name), {.kind = openbus::EndpointKind::topic, .codec = static_cast<std::uint16_t>(openbus::Codec::json)});
    {
        auto first = start_on((*virt)->path, "primary");
        if (!first) { say(first.error()); return 1; }
        started.push_back({*first, (*virt)->path, "primary", "virtual", *first < 0 ? machine_runs.back() : nullptr});
    }
    const auto status = [&] {
        std::lock_guard l(m);
        Json displays = Json::array();
        for (const auto& s : started) displays.push_back({{"path", s.display}, {"role", s.role}, {"kind", s.kind}, {"pid", s.pid}});
        Json j{{"name", name}, {"running", !ended}, {"since", when}, {"session", session}, {"virtual", (*virt)->path},
               {"mode", hd::format_mode(mode)}, {"displays", displays}, {"log", log.string()}, {"socket", socket}};
        if (frames) j["frames"] = {{"file", frames_file.string()}, {"size", kFramesBytes}};
        if (ended) {
            j["code"] = platform::exit_code(*ended);
            j["ended_by"] = ended_by;
        }
        return j;
    };
    const auto answer = [&](xbus::Call c) {
        const auto j = Json::parse(c.body(), nullptr, false);
        const auto op = j.is_object() ? j.value("op", std::string("status")) : std::string("status");
        if (op == "status") {
            c.reply(status().dump(), static_cast<std::uint16_t>(openbus::Codec::json));
        } else if (op == "attach") {
            const auto display = j.value("path", std::string{});
            const auto role = j.value("role", std::string("mirror"));
            if (display.empty() || (role != "primary" && !role.starts_with("mirror") && !role.starts_with("extend"))) {
                c.fail(openbus::ErrorCode::malformed, "attach takes a display's path and a role (primary, mirror, extend)");
                return;
            }
            auto pid = start_on(display, role);
            if (!pid) { c.fail(openbus::ErrorCode::gone, pid.error()); return; }
            {
                std::lock_guard l(m);
                started.push_back({*pid, display, role, j.value("kind", std::string("window")), *pid < 0 ? machine_runs.back() : nullptr});
            }
            publish("display-attach", {{"path", display}, {"role", role}});
            c.reply(status().dump(), static_cast<std::uint16_t>(openbus::Codec::json));
        } else if (op == "detach") {
            const auto display = j.value("path", std::string{});
            int pid = -1;
            {
                std::lock_guard l(m);
                for (const auto& s : started)
                    if (s.display == display && s.display != (*virt)->path) pid = s.pid;
            }
            std::shared_ptr<InMachine> in_machine;
            {
                std::lock_guard l(m);
                for (const auto& st : started)
                    if (st.display == display && st.machine) in_machine = st.machine;
            }
            if (in_machine) {
                // Its display gone: the command ends with its stream.
                (void)in_machine->client->close(0);
            } else if (pid < 0) {
                c.fail(openbus::ErrorCode::not_found, "no display " + display + " to detach (the virtual one stays)");
                return;
            } else {
                (void)platform::send_signal_group(pid, platform::sig::terminate);
            }
            c.reply(status().dump(), static_cast<std::uint16_t>(openbus::Codec::json));
        } else if (op == "end") {
            {
                std::lock_guard l(m);
                ended_by = j.value("by", std::string("asked to end (service/desktop)"));
            }
            stop = true;
            c.reply(Json{{"ending", true}}.dump(), static_cast<std::uint16_t>(openbus::Codec::json));
        } else {
            c.fail(openbus::ErrorCode::malformed, "status, attach, detach or end");
            return;
        }
        c.end();
    };
    (void)(*node)->offer(service_path_(name), {.verb = "desktop", .effect = "reversible", .interfaces = {"luban.desktop@1"}},
                         [answer](xbus::Call c) { std::thread([answer, c = std::move(c)]() mutable { answer(std::move(c)); }).detach(); });
    {
        std::error_code ec;
        fs::create_directories(desktop_dir_(home, name), ec);
        platform::write_file_atomic((desktop_dir_(home, name) / "state.json").string(),
                                    Json{{"pid", platform::current_pid()}, {"virtual", (*virt)->path}, {"since", when}, {"log", log.string()}}.dump(2));
    }
    publish("started", {{"virtual", (*virt)->path}, {"mode", hd::format_mode(mode)}});
    // Until the desktop (its first command) ends, or it is asked to end.
    int first_pid = started.front().pid;
    while (!stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard l(m);
        for (auto it = started.begin(); it != started.end();) {
            std::optional<platform::ExitStatus> st;
            if (it->machine) {
                if (const int code = it->machine->code.load(); code >= 0) st = platform::ExitStatus{.exited = true, .code = code};
            } else {
                st = platform::wait_process(it->pid, false);
            }
            if (!st) { ++it; continue; }
            if (it->pid == first_pid) {
                ended = *st;
                if (ended_by.empty()) ended_by = platform::exit_code(*st) == 0 ? "it ended" : std::format("it ended ({})", platform::exit_code(*st));
                stop = true;
            } else {
                publish("display-detach", {{"path", it->display}, {"code", platform::exit_code(*st)}});
            }
            it = started.erase(it);
        }
    }
    // Asked to end: every command it started, and their processes.
    {
        std::lock_guard l(m);
        for (const auto& s : started)
            if (!s.machine) (void)platform::send_signal_group(s.pid, platform::sig::terminate);
        for (const auto& s : started) {
            if (s.machine) continue;
            for (int k = 0; k < 30 && !platform::wait_process(s.pid, false); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            (void)platform::send_signal_group(s.pid, platform::sig::kill);
        }
        // A machine's: their channels closed; the machine stays (it is the system's, not the desktop's).
        for (const auto& r : machine_runs) if (r->client) (void)r->client->close(0);
    }
    const int code = ended ? platform::exit_code(*ended) : 0;
    say(std::format("the desktop ended: {} ({})", ended_by, code));
    publish("ended", {{"code", code}, {"why", ended_by}, {"log", log.string()}});
    std::error_code ec;
    fs::remove(desktop_dir_(home, name) / "state.json", ec);
    (*node)->close();
    return code;
}

}  // namespace luban::vm
