module luban.os;

import std;
import luban.desktop.imgui;
import xlings.libs.json;
import xlings.platform;
import luban.vm;
import xdesk.term;

namespace luban::os {

namespace platform = xlings::platform;

namespace {

ImU32 rgb_(std::uint32_t v, float alpha = 1.0f) {
    return IM_COL32((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, static_cast<int>(alpha * 255));
}

std::uint32_t palette_(std::uint32_t i) {
    static constexpr std::array<std::uint32_t, 16> base{0x1d1f21, 0xcc6666, 0xb5bd68, 0xf0c674, 0x81a2be, 0xb294bb, 0x8abeb7, 0xc5c8c6,
                                                        0x666666, 0xd54e53, 0xb9ca4a, 0xe7c547, 0x7aa6da, 0xc397d8, 0x70c0b1, 0xeaeaea};
    if (i < 16) return base[i];
    if (i < 232) {
        const std::uint32_t k = i - 16;
        auto level = [](std::uint32_t v) { return v == 0 ? 0u : 55u + v * 40u; };
        return (level(k / 36) << 16) | (level((k / 6) % 6) << 8) | level(k % 6);
    }
    const std::uint32_t g = 8 + (i - 232) * 10;
    return (g << 16) | (g << 8) | g;
}

constexpr std::uint32_t kForeground = 0xd8dee9, kBackground = 0x13151a;

std::uint32_t color_(const term::Color& c, bool fg) {
    switch (c.kind) {
    case term::Color::Kind::Default: return fg ? kForeground : kBackground;
    case term::Color::Kind::Indexed: return palette_(c.value);
    case term::Color::Kind::Rgb: return c.value;
    }
    return fg ? kForeground : kBackground;
}

void utf8_(std::string& out, char32_t c) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

}  // namespace

std::unique_ptr<Console> Console::machine(const fs::path& home, const std::string& name, std::function<void()> wake,
                                          std::string& error) {
    auto client = vm::Client::connect(home, name);
    if (!client) { error = client.error(); return nullptr; }
    std::unique_ptr<Console> c(new Console());
    c->name_ = name;
    c->machine_ = true;
    c->wake_ = std::move(wake);
    c->client_.emplace(std::move(*client));
    if (!c->client_->open(1, c->rows_, c->cols_, vm::session_env(home, name))) { error = "the machine did not take a session"; return nullptr; }
    auto* self = c.get();
    c->io_ = std::thread([self] {
        while (!self->stop_) {
            auto frames = self->client_->poll(100);
            if (!frames) break;
            bool changed = false;
            for (auto& [type, body] : *frames) {
                if (body.first != 1) continue;
                if (type == 'D') {
                    std::string replies;
                    {
                        std::lock_guard l(self->m_);
                        self->screen_.feed(body.second);
                        replies = self->screen_.take_replies();
                    }
                    if (!replies.empty()) self->client_->send(1, replies);
                    changed = true;
                } else if (type == 'X') {
                    auto j = nlohmann::json::parse(body.second, nullptr, false);
                    self->code_ = j.is_object() ? j.value("code", 0) : 0;
                    self->alive_ = false;
                }
            }
            if ((changed || !self->alive_) && self->wake_) self->wake_();
            if (!self->alive_) return;
        }
        self->alive_ = false;
        if (self->wake_) self->wake_();
    });
    return c;
}

std::unique_ptr<Console> Console::sandbox(const fs::path& xlings, const fs::path& home, const std::string& name,
                                          const std::string& persist, std::function<void()> wake, std::string& error) {
    auto env = platform::environment();
    env["XLINGS_HOME"] = home.string();
    env.erase("XLINGS_SUBOS_MODE");
    env.erase("XLINGS_ACTIVE_SUBOS");
    std::vector<std::string> argv{xlings.string(), "subos", "use", name};
    if (!persist.empty() && persist != "all") argv.insert(argv.end(), {"--persist", persist});
    return process(argv, std::move(env), name, std::move(wake), error);
}

std::unique_ptr<Console> Console::process(const std::vector<std::string>& argv, std::map<std::string, std::string> env,
                                          const std::string& name, std::function<void()> wake, std::string& error) {
    auto pty = platform::open_pty();
    if (!pty) { error = "no pseudo-terminal here"; return nullptr; }
    std::unique_ptr<Console> c(new Console());
    c->name_ = name;
    c->wake_ = std::move(wake);
    platform::set_terminal_size((*pty)[0], static_cast<unsigned short>(c->rows_), static_cast<unsigned short>(c->cols_));
    env["TERM"] = "xterm-256color";
    const int pid = platform::fork_process();
    if (pid == 0) {
        platform::new_session();
        const std::array<int, 3> stdio{(*pty)[1], (*pty)[1], (*pty)[1]};
        platform::redirect_stdio(stdio);
        platform::take_controlling_terminal();
        platform::exec_program(argv, env);
        platform::exit_now(127);
    }
    std::vector<int> slave{(*pty)[1]};
    platform::close_fds(slave);
    if (pid < 0) { error = "cannot start its shell"; return nullptr; }
    c->master_ = (*pty)[0];
    c->pid_ = pid;
    auto* self = c.get();
    c->io_ = std::thread([self] {
        std::array<char, 65536> buf{};
        while (!self->stop_) {
            std::array<platform::PollFd, 1> p{{{.fd = self->master_}}};
            platform::poll_fds(p, 100);
            if (!p[0].readable && !p[0].closed) continue;
            auto n = platform::read_some(self->master_, buf);
            if (!n || *n == 0) break;
            std::string replies;
            {
                std::lock_guard l(self->m_);
                self->screen_.feed(std::string_view(buf.data(), *n));
                replies = self->screen_.take_replies();
            }
            if (!replies.empty()) (void)platform::write_all(self->master_, replies);
            if (self->wake_) self->wake_();
        }
        if (auto st = platform::wait_process(self->pid_, true)) self->code_ = platform::exit_code(*st);
        self->alive_ = false;
        if (self->wake_) self->wake_();
    });
    return c;
}

Console::~Console() {
    stop_ = true;
    if (client_ && alive_) client_->close(1);
    if (pid_ > 0 && alive_) platform::send_signal(pid_, platform::sig::hangup);
    if (io_.joinable()) io_.join();
    if (master_ >= 0) { std::vector<int> fds{master_}; platform::close_fds(fds); }
}

void Console::write(std::string_view bytes) {
    if (!alive_ || bytes.empty()) return;
    if (client_) client_->send(1, bytes);
    else if (master_ >= 0) (void)platform::write_all(master_, bytes);
}

void Console::resize(int rows, int cols) {
    if (rows == rows_ && cols == cols_) return;
    rows_ = rows;
    cols_ = cols;
    {
        std::lock_guard l(m_);
        screen_.resize(rows, cols);
    }
    if (client_) client_->resize(1, rows, cols);
    else if (master_ >= 0) platform::set_terminal_size(master_, static_cast<unsigned short>(rows), static_cast<unsigned short>(cols));
}

std::pair<int, int> draw_console(Console& c, void* mono, float size, bool focused) {
    auto* font = static_cast<ImFont*>(mono);
    ImGui::PushFont(font, size);
    const float cw = ImGui::CalcTextSize("M").x;
    const float ch = ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int cols = std::max(10, static_cast<int>(avail.x / cw));
    const int rows = std::max(4, static_cast<int>(avail.y / ch));
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), rgb_(kBackground));
    std::lock_guard l(c.lock());
    auto& s = c.screen();
    std::string text;
    for (int r = 0; r < s.rows(); ++r) {
        const float y = origin.y + r * ch;
        for (int col = 0; col < s.cols(); ++col) {
            const auto& cell = s.at(r, col);
            if (cell.continuation) continue;
            const bool inverse = cell.attrs & term::Inverse;
            auto fg = color_(cell.fg, true), bg = color_(cell.bg, false);
            if (inverse) std::swap(fg, bg);
            const float x = origin.x + col * cw;
            const int span = col + 1 < s.cols() && s.at(r, col + 1).continuation ? 2 : 1;
            if (bg != kBackground) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw * span, y + ch), rgb_(bg));
            if (cell.ch != U' ' && !(cell.attrs & term::Hidden)) {
                text.clear();
                utf8_(text, cell.ch);
                const float alpha = (cell.attrs & term::Faint) ? 0.6f : 1.0f;
                // A wide character fills its two cells: drawn larger, centred in them.
                const float glyphSize = span == 2 ? size * 1.2f : size;
                float gx = x, gy = y;
                if (span == 2) {
                    const auto w = font->CalcTextSizeA(glyphSize, 1e9f, 0, text.data(), text.data() + text.size()).x;
                    gx = x + (cw * 2 - w) / 2;
                    gy = y - (glyphSize - size) * 0.6f;
                }
                dl->AddText(font, glyphSize, ImVec2(gx, gy), rgb_(fg, alpha), text.data(), text.data() + text.size());
                if (cell.attrs & term::Bold) dl->AddText(font, glyphSize, ImVec2(gx + 0.6f, gy), rgb_(fg, alpha), text.data(), text.data() + text.size());
            }
            if (cell.attrs & term::Underline) dl->AddLine(ImVec2(x, y + ch - 2), ImVec2(x + cw * span, y + ch - 2), rgb_(fg));
        }
    }
    if (s.cursor_visible() && c.alive()) {
        const float x = origin.x + s.cursor_col() * cw, y = origin.y + s.cursor_row() * ch;
        if (focused) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + ch), IM_COL32(216, 222, 233, 160));
        else dl->AddRect(ImVec2(x, y), ImVec2(x + cw, y + ch), IM_COL32(216, 222, 233, 200));
    }
    ImGui::Dummy(avail);
    ImGui::PopFont();
    return {rows, cols};
}

}  // namespace luban::os
