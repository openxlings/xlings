module luban.os;

import std;
import xlings.libs.json;
import xlings.platform;
import opensal;
import opensal.wire;
import luban.service;
import luban.vm;
import luban.desktop.wlr;

namespace luban::os {

namespace {
// The channel a window's desktop uses (its console is 1).
constexpr std::uint32_t kDisplayChannel = 2;
}  // namespace

std::optional<std::string> desktop_session(const fs::path& home, const std::string& name, const std::string& setting) {
    if (setting == "none") return std::nullopt;
    const auto root = service::root_of(home, name);
    if (!root) return std::nullopt;
    // The root's /usr/share links lead into this home's payloads, which are
    // the same paths here: what the guest will find, read before it boots.
    const auto found = luban::desktop::wlr::sessions({*root / "usr" / "local" / "share" / "wayland-sessions",
                                                      *root / "usr" / "share" / "wayland-sessions"});
    if (found.empty()) return std::nullopt;
    if (setting.empty()) return found.front().id;
    for (const auto& s : found)
        if (s.id == setting) return s.id;
    return std::nullopt;
}

std::unique_ptr<Display> Display::machine(const fs::path& home, const std::string& name, const std::string& session,
                                          opensal::Size size, std::function<void()> wake, std::string& error) {
    auto client = vm::Client::connect(home, name);
    if (!client) { error = client.error(); return nullptr; }
    std::unique_ptr<Display> d(new Display());
    d->name_ = name;
    d->wake_ = std::move(wake);
    d->wanted_ = size;
    d->client_.emplace(std::move(*client));
    if (!d->client_->open_display(kDisplayChannel, session, int(size.width), int(size.height))) {
        error = "the machine did not take a desktop";
        return nullptr;
    }
    auto* self = d.get();
    d->io_ = std::thread([self] {
        while (!self->stop_) {
            auto frames = self->client_->poll(100);
            if (!frames) break;
            for (auto& [type, body] : *frames) {
                if (body.first != kDisplayChannel) continue;
                if (type == 'D') self->provider_(body.second);
                else if (type == 'X') {
                    auto j = nlohmann::json::parse(body.second, nullptr, false);
                    std::lock_guard l(self->m_);
                    self->error_ = j.is_object() ? j.value("error", std::string{}) : std::string{};
                    if (self->error_.empty() && j.is_object() && j.value("code", 0) != 0)
                        self->error_ = std::format("the desktop ended ({})", j.value("code", 0));
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
                                          opensal::Size size, std::function<void()> wake, std::string& error) {
    namespace platform = xlings::platform;
    auto to = platform::make_pipe(), from = platform::make_pipe();
    if (!to || !from) { error = "no pipe for the desktop"; return nullptr; }
    auto env = platform::environment();
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
    const int log = platform::open_for_append(home / "logs" / "luban-os.log");
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
    d->wake_ = std::move(wake);
    d->wanted_ = size;
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
        if (auto st = platform::wait_process(self->pid_, true); st && platform::exit_code(*st) != 0) {
            std::lock_guard l(self->m_);
            if (self->error_.empty()) self->error_ = std::format("the desktop ended ({})", platform::exit_code(*st));
        }
        self->alive_ = false;
        if (self->wake_) self->wake_();
    });
    return d;
}

Display::~Display() {
    stop_ = true;
    if (client_ && alive_) client_->close(kDisplayChannel);
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
    std::lock_guard l(sending_);
    return to_ >= 0 && xlings::platform::write_all(to_, wire_bytes);
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
            // What this window provides: the surface, its size, input.
            using I = opensal::Interface;
            const auto provided = I::Surface | I::Mode | I::Pointer | I::Keyboard;
            opensal::Size s;
            { std::lock_guard l(m_); s = wanted_; }
            send_(opensal::wire::encode('H', std::format("{} {} {} 1", provided, s.width, s.height)));
        } else if (msg.kind == 's') {
            std::istringstream in(msg.payload);
            opensal::Size s;
            if (in >> s.width >> s.height) {
                std::lock_guard l(m_);
                size_ = s;
                updates_.clear();
            }
        } else if (msg.kind == 'f') {
            if (auto f = opensal::wire::decode_frame(msg.payload)) {
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
    if (const auto* r = std::get_if<opensal::Resized>(&e)) {
        std::lock_guard l(m_);
        wanted_ = r->size;
    }
    send_(opensal::wire::encode('E', opensal::wire::encode_event(e)));
}

opensal::Size Display::take(std::vector<Update>& out) {
    std::lock_guard l(m_);
    out = std::move(updates_);
    updates_.clear();
    return size_;
}

}  // namespace luban::os
