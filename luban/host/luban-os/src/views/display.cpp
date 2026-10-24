module luban.os;

import std;
import xlings.subos.home_view;
import xlings.subos.located;
import xlings.libs.json;
import xlings.platform;
import opensal;
import opensal.wire;
import openhal.audio;
import luban.desktop.host;
import luban.service;
import luban.vm;
import luban.compat;
import opensal.wlr;
import openbus;
import xbus.node;
import openhal.display;
import :core;

namespace luban::os {

namespace {
// The channel a window's desktop uses (its console is 1), and its sound.
constexpr std::uint32_t kDisplayChannel = 2;
constexpr std::uint32_t kSoundChannel = 3;
}  // namespace

// Why a desktop ended, as far as its exit says it (design 2026-10-24 §6.2):
// who ended it, and where its log is.
std::string ended_why_(int code, const fs::path& log) {
    std::string why;
    if (code == 124) why = ": a time limit ended it";
    else if (code == 127) why = ": its program was not found";
    else if (code > 128 && code < 160) why = std::format(": signal {}", code - 128);
    return std::format("the desktop ended ({}){}{}", code, why, log.empty() ? std::string{} : " -- " + log.string());
}

std::optional<std::string> desktop_session(const service::Environment& e) {
    const auto plan = service::enter_plan(e);
    if (plan.target != "desktop") return std::nullopt;
    return plan.session;
}

std::unique_ptr<Display> Display::machine(const fs::path& home, const std::string& name, const std::string& session,
                                          opensal::Size size, ClipboardGrant clip, bool sound, std::function<void()> wake,
                                          std::string& error) {
    auto client = vm::Client::connect(home, name);
    if (!client) { error = client.error(); return nullptr; }
    // Its sound, granted (design 2026-10-20 §20.1): the system's `luban`
    // sink, relayed by `luban __audio` there, played here.
    return over_(std::move(*client), name, true, size, clip, std::move(wake), error,
                 [session, size, sound](vm::Client& cl) {
                     if (!cl.open_display(kDisplayChannel, session, int(size.width), int(size.height))) return false;
                     if (sound) (void)cl.open_command(kSoundChannel, {"luban", "__audio"}, 0, 0, nlohmann::json::object(), true);
                     return true;
                 },
                 {}, session);
}

std::unique_ptr<Display> Display::over_(vm::Client client, const std::string& name, bool machine, opensal::Size size,
                                        ClipboardGrant clip, std::function<void()> wake, std::string& error,
                                        const std::function<bool(vm::Client&)>& open, const fs::path& log,
                                        const std::string& session) {
    std::unique_ptr<Display> d(new Display());
    d->name_ = name;
    d->session_ = session;
    d->machine_ = machine;
    d->narrow_ = true;   // a serial port, a relay's pipe: frames compressed (opensal 0.6)
    d->log_ = log;
    d->wake_ = std::move(wake);
    d->wanted_ = size;
    d->clip_ = clip;
    d->client_.emplace(std::move(client));
    if (!open(*d->client_)) {
        error = machine ? "the machine did not take a desktop" : "the desktop did not start";
        return nullptr;
    }
    auto* self = d.get();
    d->io_ = std::thread([self] {
        while (!self->stop_) {
            auto frames = self->client_->poll(100);
            if (!frames) break;
            for (auto& [type, body] : *frames) {
                if (body.first == kSoundChannel) {
                    if (type == 'D') self->sound_(body.second);
                    continue;
                }
                if (body.first != kDisplayChannel) continue;
                if (type == 'D') self->provider_(body.second);
                else if (type == 'E' && !self->log_.empty()) {
                    std::ofstream out(self->log_, std::ios::app);
                    out << body.second;
                } else if (type == 'X') {
                    auto j = nlohmann::json::parse(body.second, nullptr, false);
                    std::lock_guard l(self->m_);
                    self->error_ = j.is_object() ? j.value("error", std::string{}) : std::string{};
                    if (self->error_.empty() && j.is_object() && j.value("code", 0) != 0)
                        self->error_ = ended_why_(j.value("code", 0), self->log_);
                    self->alive_ = false;
                }
            }
            if (!self->alive_) break;
        }
        self->alive_ = false;
        if (self->wake_) self->wake_();
    });
    return d;
}

std::unique_ptr<Display> Display::sandbox(const fs::path& xlings, const fs::path& luban, const fs::path& home,
                                          const std::string& name, const std::string& session, const std::string& persist,
                                          opensal::Size size, ClipboardGrant clip, std::function<void()> wake, std::string& error) {
    if constexpr (xlings::platform::is_windows) {
        // On a PC the sandbox is in the Luban host machine: its desktop there,
        // opensal.wire on a piped command of the host machine's agent.
        auto agent = vm::host_machine_agent(home);
        if (!agent) { error = agent.error(); return nullptr; }
        std::vector<std::string> argv{"/xlings/bin/xlings", "subos", "use", name};
        if (!persist.empty() && persist != "all") argv.insert(argv.end(), {"--persist", persist});
        argv.insert(argv.end(), {"--cmd", std::format("exec /xlings/bin/luban __desktop '{}' --size {}x{}",
                                                      session.empty() ? "-" : session, size.width, size.height)});
        return over_(std::move(*agent), name, false, size, clip, std::move(wake), error,
                     [argv](vm::Client& cl) { return cl.open_command(kDisplayChannel, argv, 0, 0, nlohmann::json::object(), true); },
                     home / "logs" / "luban-os.log", session);
    }
    auto env = xlings::platform::environment();
    env["XLINGS_HOME"] = home.string();
    env.erase("XLINGS_SUBOS_MODE");
    env.erase("XLINGS_ACTIVE_SUBOS");
    // `subos use --cmd`: the session as the environment's own (its sandbox, what it keeps).
    auto quoted = [](std::string_view w) {
        std::string out = "'";
        for (char ch : w) { if (ch == '\'') out += "'\\''"; else out += ch; }
        return out + "'";
    };
    std::vector<std::string> argv{xlings.string(), "subos", "use", name};
    if (!persist.empty() && persist != "all") argv.insert(argv.end(), {"--persist", persist});
    argv.insert(argv.end(), {"--cmd", std::format("exec {} __desktop {} --size {}x{}", quoted(luban.string()),
                                                  quoted(session.empty() ? "-" : session), size.width, size.height)});
    return process(argv, std::move(env), home / "logs" / "luban-os.log", name, size, clip, std::move(wake), error, session);
}

std::unique_ptr<Display> Display::bus(const fs::path& home, const std::string& name, const std::string& session,
                                      const std::string& persist, opensal::Size size, ClipboardGrant clip,
                                      std::function<void()> wake, std::string& error) {
    namespace hd = openhal::display;
    // Its desktop, running on its own (started when it is not).
    vm::DesktopOptions o{.mode = {std::max(64u, size.width), std::max(64u, size.height), 1.0}, .session = session, .persist = persist};
    auto st = vm::desktop_start(home, name, o);
    if (!st) { error = st.error(); return nullptr; }
    auto fd = vm::connect_bus(home);
    if (!fd) { error = fd.error(); return nullptr; }
    { std::vector<int> close{*fd}; xlings::platform::close_fds(close); }
    auto node = xbus::Node::connect({.name = "luban-os", .actor = "program:luban-os",
                                     .router = xbus::Address{"unix", vm::bus_socket(home).string()}, .telemetry = false});
    if (!node) { error = node.error(); return nullptr; }
    std::unique_ptr<Display> d(new Display());
    d->name_ = name;
    d->session_ = session;
    d->wake_ = std::move(wake);
    d->wanted_ = size;
    d->clip_ = clip;
    d->home_ = home;
    d->bus_node_ = *node;
    auto* self = d.get();
    // This window, as a display: the desktop opens it.
    for (std::uint32_t n = 0; n < 64 && self->bus_path_.empty(); ++n) {
        const auto path = hd::device_path((*node)->root(), n);
        xbus::OfferOptions offer{.kind = openbus::EndpointKind::stream, .verb = "display", .effect = "reversible",
                                 .interfaces = {std::string(hd::kInterface)},
                                 .properties = {{"display-kind", "window"}, {"mode", hd::format_mode({size.width, size.height, 1.0})},
                                                {"input", "1"}, {"system", name}, {"name", "Luban OS window"}}};
        auto offered = (*node)->offer_stream(path, offer, [self](xbus::Stream s) {
            std::lock_guard l(self->m_);
            if (self->bus_stream_ && !self->bus_stream_->ended()) { s.close(); return; }
            self->bus_stream_ = s;
            self->bus_opened_.notify_all();
        }, {.window = 8u << 20});
        if (offered) self->bus_path_ = path;
        else if (offered.error().code != openbus::ErrorCode::taken) { error = offered.error().message; return nullptr; }
    }
    if (self->bus_path_.empty()) { error = "no display path is free on the bus"; return nullptr; }
    // The desktop takes it as its primary: the window shows it at its size.
    auto attached = vm::desktop(home, name, {{"op", "attach"}, {"path", self->bus_path_}, {"role", "primary"}, {"kind", "window"}});
    if (!attached) { error = attached.error(); return nullptr; }
    d->io_ = std::thread([self] {
        {
            std::unique_lock l(self->m_);
            self->bus_opened_.wait_for(l, std::chrono::seconds(60), [self] { return self->bus_stream_.has_value() || self->stop_; });
            if (!self->bus_stream_) {
                if (self->error_.empty() && !self->stop_) self->error_ = "the desktop did not open this window's display";
                self->alive_ = false;
                if (self->wake_) self->wake_();
                return;
            }
        }
        auto s = *self->bus_stream_;
        while (!self->stop_) {
            auto chunk = s.read(std::chrono::milliseconds(200));
            if (!chunk) {
                if (s.ended()) break;
                continue;
            }
            self->provider_(chunk->bytes);
            if (!self->alive_) break;
        }
        // Its end: the desktop's, said with why (its supervisor's word), or this window's.
        if (!self->stop_) {
            auto st = vm::desktop(self->home_, self->name_, {{"op", "status"}});
            std::lock_guard l(self->m_);
            if (self->error_.empty()) self->error_ = st ? std::string("the desktop let this window go") : std::string("the desktop ended");
        }
        self->alive_ = false;
        if (self->wake_) self->wake_();
    });
    return d;
}

std::unique_ptr<Display> Display::process(const std::vector<std::string>& argv, std::map<std::string, std::string> env,
                                          const fs::path& log_file, const std::string& name, opensal::Size size,
                                          ClipboardGrant clip, std::function<void()> wake, std::string& error,
                                          const std::string& session) {
    namespace platform = xlings::platform;
    auto to = platform::make_pipe(), from = platform::make_pipe();
    if (!to || !from) { error = "no pipe for the desktop"; return nullptr; }
    std::error_code ec;
    fs::create_directories(log_file.parent_path(), ec);
    const int log = platform::open_for_append(log_file);
    const int pid = platform::fork_process();
    if (pid == 0) {
        platform::new_session();
        const std::array<int, 3> stdio{(*to)[0], (*from)[1], log >= 0 ? log : (*from)[1]};
        platform::redirect_stdio(stdio);
        platform::exec_program(argv, env);
        platform::exit_now(127);
    }
    std::vector<int> theirs{(*to)[0], (*from)[1]};
    if (log >= 0) theirs.push_back(log);
    platform::close_fds(theirs);
    if (pid < 0) {
        std::vector<int> rest{(*to)[1], (*from)[0]};
        platform::close_fds(rest);
        error = "cannot start the desktop";
        return nullptr;
    }
    std::unique_ptr<Display> d(new Display());
    d->name_ = name;
    d->session_ = session;
    d->wake_ = std::move(wake);
    d->wanted_ = size;
    d->clip_ = clip;
    d->log_file_ = log_file;
    d->to_ = (*to)[1];
    d->from_ = (*from)[0];
    d->pid_ = pid;
    auto* self = d.get();
    d->io_ = std::thread([self] {
        std::array<char, 65536> buf{};
        while (!self->stop_) {
            std::array<platform::PollFd, 1> p{{{.fd = self->from_}}};
            platform::poll_fds(p, 100);
            if (!p[0].readable && !p[0].closed) continue;
            auto n = platform::read_some(self->from_, buf);
            if (!n || *n == 0) break;
            self->provider_(std::string_view(buf.data(), *n));
            if (!self->alive_) break;
        }
        // Closed (stop_): the desktop ends at its input's end -- unless it is
        // writing a frame nobody reads, which it finishes first. What it
        // writes is drained until it ends; one that does not end in 3 s is
        // ended (its session: it and what it started).
        if (self->stop_) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            bool ended = false;
            while (std::chrono::steady_clock::now() < deadline) {
                std::array<platform::PollFd, 1> p{{{.fd = self->from_}}};
                platform::poll_fds(p, 100);
                if (!p[0].readable && !p[0].closed) continue;
                auto n = platform::read_some(self->from_, buf);
                if (!n || *n == 0) { ended = true; break; }
            }
            if (!ended) {
                (void)platform::send_signal_group(self->pid_, platform::sig::terminate);
                std::optional<platform::ExitStatus> st;
                for (int k = 0; k < 20 && !(st = platform::wait_process(self->pid_, false)); ++k)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (!st) (void)platform::send_signal_group(self->pid_, platform::sig::kill);
                else {
                    self->alive_ = false;
                    if (self->wake_) self->wake_();
                    return;
                }
            }
        }
        // Its end. Closed by this window, a desktop that other windows still
        // show (design 2026-10-20 §11) goes on: it is waited for a moment, then left.
        std::optional<platform::ExitStatus> st;
        if (!self->stop_) st = platform::wait_process(self->pid_, true);
        else
            for (int k = 0; k < 60 && !(st = platform::wait_process(self->pid_, false)); ++k)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (st && platform::exit_code(*st) != 0) {
            std::lock_guard l(self->m_);
            if (self->error_.empty()) self->error_ = ended_why_(platform::exit_code(*st), self->log_file_);
        }
        self->alive_ = false;
        if (self->wake_) self->wake_();
    });
    return d;
}

Display::~Display() {
    stop_ = true;
    if (bus_node_) {
        // This window goes: its display with it; the desktop goes on (its virtual display).
        {
            std::lock_guard l(m_);
            if (bus_stream_) bus_stream_->close();
            bus_opened_.notify_all();
        }
        if (io_.joinable()) io_.join();
        bus_node_->close();
        return;
    }
    if (client_ && alive_) {
        client_->close(kSoundChannel);
        client_->close(kDisplayChannel);
    }
    if (to_ >= 0) {
        // The stream's end stops the bridge, and it its compositor.
        std::vector<int> fds{to_};
        xlings::platform::close_fds(fds);
        to_ = -1;
    }
    if (io_.joinable()) io_.join();
    if (from_ >= 0) { std::vector<int> fds{from_}; xlings::platform::close_fds(fds); }
}

bool Display::send_(std::string_view wire_bytes) {
    if (client_) return client_->send(kDisplayChannel, wire_bytes);
    if (bus_node_) {
        std::optional<xbus::Stream> s;
        { std::lock_guard l(m_); s = bus_stream_; }
        return s && s->write(wire_bytes, std::chrono::seconds(10));
    }
    std::lock_guard l(sending_);
    return to_ >= 0 && xlings::platform::write_all(to_, wire_bytes);
}

// The system's sound (openhal.audio): its stream played on this computer's
// output; one that cannot be is said once in the log and left.
void Display::sound_(std::string_view bytes) {
    namespace audio = openhal::audio;
    sound_decoder_.feed(bytes);
    for (;;) {
        auto m = sound_decoder_.next();
        if (!m || !*m) return;
        if (const auto* o = std::get_if<audio::Open>(&**m)) {
            if (o->direction != audio::Direction::output) continue;
            auto s = host::Speaker::open(o->format.rate, o->format.channels);
            if (s) speaker_ = std::move(*s);
            else if (!log_.empty()) std::ofstream(log_, std::ios::app) << "sound: " << s.error() << "\n";
        } else if (const auto* p = std::get_if<audio::Samples>(&**m)) {
            if (speaker_) speaker_->play(p->bytes);
        } else if (const auto* v = std::get_if<audio::Volume>(&**m)) {
            if (speaker_) speaker_->volume(v->level, v->muted);
        } else {
            speaker_.reset();
        }
    }
}

void Display::provider_(std::string_view bytes) {
    decoder_.feed(bytes);
    bool changed = false;
    for (;;) {
        auto m = decoder_.next();
        if (!m) {
            std::lock_guard l(m_);
            error_ = "the desktop's stream is not opensal.wire";
            alive_ = false;
            break;
        }
        if (!*m) break;
        auto& msg = **m;
        if (msg.kind == 'h') {
            // What this window provides: the surface, its size, input, the
            // host's input method, and the clipboard as far as it is granted.
            // The shell, too, for a desktop without its own (it asks for it).
            using I = opensal::Interface;
            auto provided = I::Surface | I::Mode | I::Pointer | I::Keyboard | I::Text | I::Shell | I::Cursor | I::CursorImage
                            | I::PointerLock | I::Displays;
            if (clip_.copy || clip_.paste) provided = provided | I::Clipboard;
            if (narrow_) provided = provided | I::Compressed;
            opensal::Interfaces wanted = 0;
            std::from_chars(msg.payload.data(), msg.payload.data() + msg.payload.size(), wanted);
            shell_ = opensal::has(wanted, I::Shell);
            {
                // Who draws the pointer: it, when it asked; else the host's
                // arrow for an old xdesk, or nothing (it is in the frames).
                std::lock_guard l(m_);
                const bool asked = opensal::has(wanted, I::Cursor);
                pointer_ = {};
                pointer_.host_arrow = luban::compat::legacy_xdesk_pointer(session_, asked);
                pointer_.shape = asked || pointer_.host_arrow ? "default" : "none";
            }
            opensal::Size s;
            double density = 1;
            { std::lock_guard l(m_); s = wanted_; density = density_; }
            send_(opensal::wire::encode('H', std::format("{} {} {} {} 1", provided, s.width, s.height, density)));
        } else if (msg.kind == 'c') {
            std::lock_guard l(m_);
            pointer_.shape = msg.payload == "none" ? std::string("none") : std::string(opensal::cursor_named(msg.payload));
            pointer_.image.reset();
            changed = true;
        } else if (msg.kind == 'C') {
            if (auto image = opensal::wire::decode_cursor_image(msg.payload)) {
                std::lock_guard l(m_);
                pointer_.shape = "custom";
                pointer_.image = std::move(*image);
                changed = true;
            }
        } else if (msg.kind == 'p') {
            if (auto p = opensal::wire::decode_pointer_locate(msg.payload)) {
                std::lock_guard l(m_);
                pointer_.locate = *p;
            }
        } else if (msg.kind == 'd') {
            if (auto r = opensal::wire::decode_display(msg.payload)) {
                std::optional<opensal::Size> resend;
                double density = 1;
                {
                    std::lock_guard l(m_);
                    // Made the primary: the desktop takes this window's size again.
                    if (role_ && role_->kind == opensal::DisplayRole::Kind::mirror && r->kind == opensal::DisplayRole::Kind::primary)
                        resend = wanted_, density = density_;
                    role_changed_ = role_ != *r;
                    role_ = *r;
                    changed = changed || role_changed_;
                }
                if (resend) {
                    send_(opensal::wire::encode('E', opensal::wire::encode_event(opensal::DensityChanged{density})));
                    send_(opensal::wire::encode('E', opensal::wire::encode_event(opensal::Resized{*resend})));
                }
            }
        } else if (msg.kind == 'l') {
            std::lock_guard l(m_);
            pointer_lock_ = msg.payload == "1";
            changed = true;
        } else if (msg.kind == 's') {
            std::istringstream in(msg.payload);
            opensal::Size s;
            if (in >> s.width >> s.height) {
                std::lock_guard l(m_);
                size_ = s;
                updates_.clear();
            }
        } else if (msg.kind == 't') {
            std::lock_guard l(m_);
            text_input_ = msg.payload;
            changed = true;
        } else if (msg.kind == 'b') {
            if (clip_.copy) {
                std::lock_guard l(m_);
                clipboard_ = msg.payload;
                changed = true;
            }
        } else if (msg.kind == 'a') {
            if (auto apps = opensal::wire::decode_applications(msg.payload)) {
                std::lock_guard l(m_);
                applications_ = std::move(*apps);
                changed = true;
            }
        } else if (msg.kind == 'w') {
            if (auto windows = opensal::wire::decode_windows(msg.payload)) {
                std::lock_guard l(m_);
                windows_ = std::move(*windows);
                changed = true;
            }
        } else if (msg.kind == 'f' || msg.kind == 'z') {
            // 'z': the rows compressed (opensal 0.6), what came over a narrow channel.
            std::optional<std::string> unpacked;
            if (msg.kind == 'z' && !(unpacked = opensal::wire::decode_compressed(msg.payload))) continue;
            if (auto f = opensal::wire::decode_frame(unpacked ? std::string_view(*unpacked) : std::string_view(msg.payload))) {
                std::lock_guard l(m_);
                // A frame over the whole surface makes everything before it moot.
                if (f->first.rect.x == 0 && f->first.rect.y == 0 && f->first.rect.width == size_.width
                    && f->first.rect.height == size_.height)
                    updates_.clear();
                updates_.push_back({f->first.rect, std::string(f->second)});
                changed = true;
            }
        }
    }
    if (changed && wake_) wake_();
}

void Display::send(const opensal::Event& e) {
    if (!alive_) return;
    bool mirror = false;
    {
        std::lock_guard l(m_);
        mirror = role_ && role_->kind == opensal::DisplayRole::Kind::mirror;
    }
    if (const auto* r = std::get_if<opensal::Resized>(&e)) {
        std::lock_guard l(m_);
        wanted_ = r->size;
    }
    // A hello sent after this says it too: in whichever order they cross, the last word is this.
    if (const auto* d = std::get_if<opensal::DensityChanged>(&e)) {
        std::lock_guard l(m_);
        density_ = d->scale;
    }
    // A mirror's size is its own: the desktop is the primary's.
    if (mirror && (std::holds_alternative<opensal::Resized>(e) || std::holds_alternative<opensal::DensityChanged>(e))) return;
    send_(opensal::wire::encode('E', opensal::wire::encode_event(e)));
}

std::optional<std::string> Display::take_text_input() {
    std::lock_guard l(m_);
    return std::exchange(text_input_, std::nullopt);
}

std::optional<std::string> Display::take_clipboard() {
    std::lock_guard l(m_);
    return std::exchange(clipboard_, std::nullopt);
}

std::optional<opensal::DisplayRole> Display::take_role() {
    std::lock_guard l(m_);
    if (!std::exchange(role_changed_, false)) return std::nullopt;
    return role_;
}

std::optional<bool> Display::take_pointer_lock() {
    std::lock_guard l(m_);
    return std::exchange(pointer_lock_, std::nullopt);
}

opensal::Size Display::take(std::vector<Update>& out) {
    std::lock_guard l(m_);
    out = std::move(updates_);
    updates_.clear();
    return size_;
}

}  // namespace luban::os
