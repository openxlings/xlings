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

Display::~Display() {
    stop_ = true;
    if (client_ && alive_) client_->close(kDisplayChannel);
    if (io_.joinable()) io_.join();
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
            client_->send(kDisplayChannel, opensal::wire::encode('H', std::format("{} {} {} 1", provided, s.width, s.height)));
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
    client_->send(kDisplayChannel, opensal::wire::encode('E', opensal::wire::encode_event(e)));
}

opensal::Size Display::take(std::vector<Update>& out) {
    std::lock_guard l(m_);
    out = std::move(updates_);
    updates_.clear();
    return size_;
}

}  // namespace luban::os
