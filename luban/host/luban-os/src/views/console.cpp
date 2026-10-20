module luban.os;

import std;
import imkit.imgui;
import xlings.libs.json;
import xlings.platform;
import luban.vm;
import xdesk.term;
import :core;

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

constexpr std::uint32_t kForeground = 0xd8dee9, kBackground = 0x13151a;   // a terminal's own, not the app's palette

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
                                          std::string& error, const std::string& greeting) {
    auto client = vm::Client::connect(home, name);
    if (!client) { error = client.error(); return nullptr; }
    const auto env = vm::session_env(home, name);
    return over_(std::move(*client), name, true, std::move(wake), error, greeting,
                 [env](vm::Client& cl, int rows, int cols) { return cl.open(1, rows, cols, env); });
}

std::unique_ptr<Console> Console::over_(vm::Client client, const std::string& name, bool machine, std::function<void()> wake,
                                        std::string& error, const std::string& greeting,
                                        const std::function<bool(vm::Client&, int, int)>& open) {
    std::unique_ptr<Console> c(new Console());
    c->screen_.feed(greeting);
    c->name_ = name;
    c->machine_ = machine;
    c->wake_ = std::move(wake);
    c->client_.emplace(std::move(client));
    if (!open(*c->client_, c->rows_, c->cols_)) { error = machine ? "the machine did not take a session" : "the session did not start"; return nullptr; }
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
                                          const std::string& persist, std::function<void()> wake, std::string& error,
                                          const std::string& greeting) {
    if constexpr (platform::is_windows) {
        // On a PC the sandbox is in the Luban host machine: its shell on a
        // terminal there, through the host machine's agent.
        auto agent = vm::host_machine_agent(home);
        if (!agent) { error = agent.error(); return nullptr; }
        std::vector<std::string> argv{"/xlings/bin/xlings", "subos", "use", name};
        if (!persist.empty() && persist != "all") argv.insert(argv.end(), {"--persist", persist});
        nlohmann::json env = nlohmann::json::object();
        if (!greeting.empty()) env["XLINGS_ENTER_QUIET"] = "1";
        return over_(std::move(*agent), name, false, std::move(wake), error, greeting,
                     [argv, env](vm::Client& cl, int rows, int cols) { return cl.open_command(1, argv, rows, cols, env, false); });
    }
    auto env = platform::environment();
    env["XLINGS_HOME"] = home.string();
    // The welcome is ours; xlings' own line is not said.
    if (!greeting.empty()) env["XLINGS_ENTER_QUIET"] = "1";
    env.erase("XLINGS_SUBOS_MODE");
    env.erase("XLINGS_ACTIVE_SUBOS");
    std::vector<std::string> argv{xlings.string(), "subos", "use", name};
    if (!persist.empty() && persist != "all") argv.insert(argv.end(), {"--persist", persist});
    return process(argv, std::move(env), name, std::move(wake), error, greeting);
}

std::unique_ptr<Console> Console::process(const std::vector<std::string>& argv, std::map<std::string, std::string> env,
                                          const std::string& name, std::function<void()> wake, std::string& error,
                                          const std::string& greeting) {
    auto pty = platform::open_pty();
    if (!pty) { error = "no pseudo-terminal here"; return nullptr; }
    std::unique_ptr<Console> c(new Console());
    c->screen_.feed(greeting);
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

namespace {

// Box drawing and block elements (U+2500..U+259F), drawn as shapes on the
// cell -- seamless from cell to cell, whatever the font has. Each line
// character says, for its four arms (left, up, right, down), 0 none, 1 light,
// 2 heavy, 3 double.
struct Arms { std::uint8_t l, u, r, d; };
std::optional<Arms> arms_(char32_t c) {
    switch (c) {
    case U'─': return Arms{1, 0, 1, 0}; case U'━': return Arms{2, 0, 2, 0};
    case U'│': return Arms{0, 1, 0, 1}; case U'┃': return Arms{0, 2, 0, 2};
    case U'┌': return Arms{0, 0, 1, 1}; case U'┐': return Arms{1, 0, 0, 1};
    case U'└': return Arms{0, 1, 1, 0}; case U'┘': return Arms{1, 1, 0, 0};
    case U'├': return Arms{0, 1, 1, 1}; case U'┤': return Arms{1, 1, 0, 1};
    case U'┬': return Arms{1, 0, 1, 1}; case U'┴': return Arms{1, 1, 1, 0};
    case U'┼': return Arms{1, 1, 1, 1};
    case U'┏': return Arms{0, 0, 2, 2}; case U'┓': return Arms{2, 0, 0, 2};
    case U'┗': return Arms{0, 2, 2, 0}; case U'┛': return Arms{2, 2, 0, 0};
    case U'┣': return Arms{0, 2, 2, 2}; case U'┫': return Arms{2, 2, 0, 2};
    case U'┳': return Arms{2, 0, 2, 2}; case U'┻': return Arms{2, 2, 2, 0};
    case U'╋': return Arms{2, 2, 2, 2};
    case U'═': return Arms{3, 0, 3, 0}; case U'║': return Arms{0, 3, 0, 3};
    case U'╔': return Arms{0, 0, 3, 3}; case U'╗': return Arms{3, 0, 0, 3};
    case U'╚': return Arms{0, 3, 3, 0}; case U'╝': return Arms{3, 3, 0, 0};
    case U'╠': return Arms{0, 3, 3, 3}; case U'╣': return Arms{3, 3, 0, 3};
    case U'╦': return Arms{3, 0, 3, 3}; case U'╩': return Arms{3, 3, 3, 0};
    case U'╬': return Arms{3, 3, 3, 3};
    case U'╭': return Arms{0, 0, 1, 1}; case U'╮': return Arms{1, 0, 0, 1};
    case U'╰': return Arms{0, 1, 1, 0}; case U'╯': return Arms{1, 1, 0, 0};
    case U'╴': return Arms{1, 0, 0, 0}; case U'╵': return Arms{0, 1, 0, 0};
    case U'╶': return Arms{0, 0, 1, 0}; case U'╷': return Arms{0, 0, 0, 1};
    default: return std::nullopt;
    }
}

// True when it was drawn.
bool draw_box_(ImDrawList* dl, char32_t c, float x, float y, float w, float h, ImU32 color) {
    if (auto a = arms_(c)) {
        const float t = std::max(1.0f, std::round(w / 8)), T = std::max(2.0f, std::round(w / 4));
        const float cx = std::floor(x + w / 2), cy = std::floor(y + h / 2);
        auto hline = [&](float x0, float x1, int kind) {
            if (kind == 3) {
                dl->AddRectFilled(ImVec2(x0, cy - t * 1.5f), ImVec2(x1, cy - t * 0.5f), color);
                dl->AddRectFilled(ImVec2(x0, cy + t * 0.5f), ImVec2(x1, cy + t * 1.5f), color);
            } else {
                const float k = kind == 2 ? T : t;
                dl->AddRectFilled(ImVec2(x0, cy - std::floor(k / 2)), ImVec2(x1, cy - std::floor(k / 2) + k), color);
            }
        };
        auto vline = [&](float y0, float y1, int kind) {
            if (kind == 3) {
                dl->AddRectFilled(ImVec2(cx - t * 1.5f, y0), ImVec2(cx - t * 0.5f, y1), color);
                dl->AddRectFilled(ImVec2(cx + t * 0.5f, y0), ImVec2(cx + t * 1.5f, y1), color);
            } else {
                const float k = kind == 2 ? T : t;
                dl->AddRectFilled(ImVec2(cx - std::floor(k / 2), y0), ImVec2(cx - std::floor(k / 2) + k, y1), color);
            }
        };
        // An arm reaches across the line it meets, no further: a corner is
        // closed, nothing sticks out of it.
        auto half = [&](int kind) { return kind == 3 ? t * 1.5f : std::floor((kind == 2 ? T : t) / 2); };
        auto far = [&](int kind) { return kind == 3 ? t * 1.5f : (kind == 2 ? T : t) - std::floor((kind == 2 ? T : t) / 2); };
        const int v = std::max(a->u, a->d), hk = std::max(a->l, a->r);
        if (a->l) hline(x, cx + (a->r ? 0 : v ? far(v) : 0), a->l);
        if (a->r) hline(cx - (a->l ? 0 : v ? half(v) : 0), x + w, a->r);
        if (a->u) vline(y, cy + (a->d ? 0 : hk ? far(hk) : 0), a->u);
        if (a->d) vline(cy - (a->u ? 0 : hk ? half(hk) : 0), y + h, a->d);
        return true;
    }
    if (c >= U'▀' && c <= U'▟') {
        auto box = [&](float x0, float y0, float x1, float y1, ImU32 col) {
            dl->AddRectFilled(ImVec2(std::round(x + x0 * w), std::round(y + y0 * h)), ImVec2(std::round(x + x1 * w), std::round(y + y1 * h)), col);
        };
        const ImU32 shade25 = (color & 0x00ffffff) | (static_cast<ImU32>(((color >> 24) & 0xff) * 0.25f) << 24);
        const ImU32 shade50 = (color & 0x00ffffff) | (static_cast<ImU32>(((color >> 24) & 0xff) * 0.5f) << 24);
        const ImU32 shade75 = (color & 0x00ffffff) | (static_cast<ImU32>(((color >> 24) & 0xff) * 0.75f) << 24);
        switch (c) {
        case U'▀': box(0, 0, 1, 0.5f, color); return true;
        case U'▁': box(0, 7 / 8.0f, 1, 1, color); return true;
        case U'▂': box(0, 6 / 8.0f, 1, 1, color); return true;
        case U'▃': box(0, 5 / 8.0f, 1, 1, color); return true;
        case U'▄': box(0, 0.5f, 1, 1, color); return true;
        case U'▅': box(0, 3 / 8.0f, 1, 1, color); return true;
        case U'▆': box(0, 2 / 8.0f, 1, 1, color); return true;
        case U'▇': box(0, 1 / 8.0f, 1, 1, color); return true;
        case U'█': box(0, 0, 1, 1, color); return true;
        case U'▉': box(0, 0, 7 / 8.0f, 1, color); return true;
        case U'▊': box(0, 0, 6 / 8.0f, 1, color); return true;
        case U'▋': box(0, 0, 5 / 8.0f, 1, color); return true;
        case U'▌': box(0, 0, 0.5f, 1, color); return true;
        case U'▍': box(0, 0, 3 / 8.0f, 1, color); return true;
        case U'▎': box(0, 0, 2 / 8.0f, 1, color); return true;
        case U'▏': box(0, 0, 1 / 8.0f, 1, color); return true;
        case U'▐': box(0.5f, 0, 1, 1, color); return true;
        case U'░': box(0, 0, 1, 1, shade25); return true;
        case U'▒': box(0, 0, 1, 1, shade50); return true;
        case U'▓': box(0, 0, 1, 1, shade75); return true;
        case U'▔': box(0, 0, 1, 1 / 8.0f, color); return true;
        case U'▕': box(7 / 8.0f, 0, 1, 1, color); return true;
        case U'▖': box(0, 0.5f, 0.5f, 1, color); return true;
        case U'▗': box(0.5f, 0.5f, 1, 1, color); return true;
        case U'▘': box(0, 0, 0.5f, 0.5f, color); return true;
        case U'▝': box(0.5f, 0, 1, 0.5f, color); return true;
        case U'▙': box(0, 0, 0.5f, 1, color); box(0.5f, 0.5f, 1, 1, color); return true;
        case U'▚': box(0, 0, 0.5f, 0.5f, color); box(0.5f, 0.5f, 1, 1, color); return true;
        case U'▛': box(0, 0, 1, 0.5f, color); box(0, 0.5f, 0.5f, 1, color); return true;
        case U'▜': box(0, 0, 1, 0.5f, color); box(0.5f, 0.5f, 1, 1, color); return true;
        case U'▞': box(0.5f, 0, 1, 0.5f, color); box(0, 0.5f, 0.5f, 1, color); return true;
        case U'▟': box(0.5f, 0, 1, 1, color); box(0, 0.5f, 0.5f, 1, color); return true;
        default: return false;
        }
    }
    return false;
}

// A line of what is shown: history when scrolled back, then the screen.
const term::Cell* cell_at_(term::Screen& s, int scroll, int row, int col, int cols) {
    const int history = static_cast<int>(s.scrollback());
    const int line = history - scroll + row;   // in history + screen coordinates
    if (line < history) {
        if (line < 0) return nullptr;
        const auto& l = s.history_line(static_cast<std::size_t>(line));
        return col < static_cast<int>(l.size()) ? &l[static_cast<std::size_t>(col)] : nullptr;
    }
    const int r = line - history;
    if (r >= s.rows() || col >= s.cols() || col >= cols) return nullptr;
    return &s.at(r, col);
}

bool selected_(const std::optional<std::array<int, 4>>& sel, int row, int col) {
    if (!sel) return false;
    auto [r0, c0, r1, c1] = *sel;
    if (std::pair(r1, c1) < std::pair(r0, c0)) { std::swap(r0, r1); std::swap(c0, c1); }
    const auto here = std::pair(row, col);
    return here >= std::pair(r0, c0) && here <= std::pair(r1, c1);
}

}  // namespace

ConsoleGrid draw_console(Console& c, const ConsoleLook& look) {
    auto* font = static_cast<ImFont*>(look.font);
    auto* bold = static_cast<ImFont*>(look.bold ? look.bold : look.font);
    // One effective size for measuring and drawing (A6): the nominal size at
    // the window's scale, as the font is baked for it.
    ImGui::PushFont(font, look.size);
    const float px = ImGui::GetFontSize();
    auto* baked = ImGui::GetFontBaked();
    const float cw = std::max(1.0f, std::round(baked->GetCharAdvance(static_cast<unsigned short>('M'))));
    const float ch = std::max(1.0f, std::round(px * 1.2f));
    const float baseline = std::round((ch - px) / 2);
    const ImVec2 region = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(region, ImVec2(region.x + avail.x, region.y + avail.y), rgb_(kBackground));
    const float pad = std::round(look.padding);
    const ImVec2 origin(std::round(region.x + pad), std::round(region.y + pad));
    const int cols = std::max(10, static_cast<int>((avail.x - pad * 2) / cw));
    const int rows = std::max(4, static_cast<int>((avail.y - pad * 2) / ch));
    std::lock_guard l(c.lock());
    auto& s = c.screen();
    const int scroll = std::clamp(look.scroll, 0, static_cast<int>(s.scrollback()));
    std::string text;
    for (int r = 0; r < std::min(rows, s.rows()); ++r) {
        const float y = origin.y + r * ch;
        for (int col = 0; col < std::min(cols, s.cols()); ++col) {
            const auto* cell = cell_at_(s, scroll, r, col, cols);
            if (!cell || cell->continuation) continue;
            const bool inverse = (cell->attrs & term::Inverse) != selected_(look.selection, r, col);
            auto fg = color_(cell->fg, true), bg = color_(cell->bg, false);
            if (cell->attrs & term::Bold && cell->fg.kind == term::Color::Kind::Indexed && cell->fg.value < 8)
                fg = palette_(cell->fg.value + 8);   // bold is bright, as terminals draw it
            if (inverse) std::swap(fg, bg);
            const float x = origin.x + col * cw;
            const auto* next = cell_at_(s, scroll, r, col + 1, cols);
            const int span = next && next->continuation ? 2 : 1;
            if (bg != kBackground) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw * span, y + ch), rgb_(bg));
            if (cell->ch != U' ' && cell->ch != 0 && !(cell->attrs & term::Hidden)) {
                const float alpha = (cell->attrs & term::Faint) ? 0.6f : 1.0f;
                if (!draw_box_(dl, cell->ch, x, y, cw * span, ch, rgb_(fg, alpha))) {
                    text.clear();
                    utf8_(text, cell->ch);
                    ImFont* f = (cell->attrs & term::Bold) ? bold : font;
                    // A wide character is centred in its two cells, at the same size.
                    float gx = x;
                    if (span == 2) gx = x + std::round((cw * 2 - f->CalcTextSizeA(px, 1e9f, 0, text.data(), text.data() + text.size()).x) / 2);
                    dl->AddText(f, px, ImVec2(gx, y + baseline), rgb_(fg, alpha), text.data(), text.data() + text.size());
                    // No bold face here: the regular one, a pixel over, as terminals do.
                    if ((cell->attrs & term::Bold) && bold == font)
                        dl->AddText(f, px, ImVec2(gx + std::max(1.0f, std::round(px / 16)), y + baseline), rgb_(fg, alpha),
                                    text.data(), text.data() + text.size());
                }
            }
            if (cell->attrs & term::Underline)
                dl->AddRectFilled(ImVec2(x, y + ch - std::max(1.0f, std::round(px / 14)) - 1), ImVec2(x + cw * span, y + ch - 1), rgb_(fg));
            if (cell->attrs & term::Strike)
                dl->AddRectFilled(ImVec2(x, std::round(y + ch / 2)), ImVec2(x + cw * span, std::round(y + ch / 2) + std::max(1.0f, std::round(px / 14))), rgb_(fg));
        }
    }
    if (scroll == 0 && s.cursor_visible() && c.alive()) {
        const float x = origin.x + s.cursor_col() * cw, y = origin.y + s.cursor_row() * ch;
        const ImU32 cur = IM_COL32(216, 222, 233, look.focused ? 190 : 230);
        const float thin = std::max(1.0f, std::round(px / 8));
        switch (s.cursor_shape()) {
        case term::Screen::CursorShape::Bar: dl->AddRectFilled(ImVec2(x, y), ImVec2(x + thin, y + ch), cur); break;
        case term::Screen::CursorShape::Underline: dl->AddRectFilled(ImVec2(x, y + ch - thin), ImVec2(x + cw, y + ch), cur); break;
        case term::Screen::CursorShape::Block:
            if (look.focused) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + ch), IM_COL32(216, 222, 233, 150));
            else dl->AddRect(ImVec2(x, y), ImVec2(x + cw, y + ch), cur, 0, 0, thin);
            break;
        }
    }
    if (scroll > 0) {
        // Scrolled back: said, where the eye is.
        const auto said = std::format(" {} / {} ", scroll, s.scrollback());
        const ImVec2 ts = font->CalcTextSizeA(px, 1e9f, 0, said.data(), said.data() + said.size());
        const ImVec2 a(origin.x + cols * cw - ts.x, origin.y);
        dl->AddRectFilled(a, ImVec2(a.x + ts.x, a.y + ch), IM_COL32(65, 166, 246, 220));
        dl->AddText(font, px, ImVec2(a.x, a.y + baseline), IM_COL32(13, 14, 23, 255), said.data(), said.data() + said.size());
    }
    ImGui::Dummy(avail);
    ImGui::PopFont();
    return {rows, cols, origin.x, origin.y, cw, ch};
}

std::string console_text(Console& c, int scroll, std::array<int, 4> selection) {
    auto [r0, c0, r1, c1] = selection;
    if (std::pair(r1, c1) < std::pair(r0, c0)) { std::swap(r0, r1); std::swap(c0, c1); }
    std::lock_guard l(c.lock());
    auto& s = c.screen();
    std::string out;
    for (int r = r0; r <= r1; ++r) {
        std::string line;
        const int from = r == r0 ? c0 : 0, to = r == r1 ? c1 : s.cols() - 1;
        for (int col = from; col <= to; ++col) {
            const auto* cell = cell_at_(s, std::clamp(scroll, 0, static_cast<int>(s.scrollback())), r, col, s.cols());
            if (!cell || cell->continuation) continue;
            utf8_(line, cell->ch ? cell->ch : U' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        if (r != r1) out += '\n';
    }
    return out;
}

// The word at (row, col) as a selection: letters, digits and what a path or
// an address is made of; a single cell where there is none.
std::array<int, 4> console_word(Console& c, int scroll, int row, int col) {
    std::lock_guard l(c.lock());
    auto& s = c.screen();
    const int sb = std::clamp(scroll, 0, static_cast<int>(s.scrollback()));
    auto word = [&](int at) {
        const auto* cell = cell_at_(s, sb, row, at, s.cols());
        if (!cell) return false;
        const char32_t ch = cell->ch;
        return ch > 0x7f || (ch < 0x80 && (std::isalnum(static_cast<int>(ch)) || std::u32string_view(U"-_./~:@%+=#").find(ch) != std::u32string_view::npos));
    };
    if (!word(col)) return {row, col, row, col};
    int from = col, to = col;
    while (from > 0 && word(from - 1)) --from;
    while (to + 1 < s.cols() && word(to + 1)) ++to;
    return {row, from, row, to};
}

}  // namespace luban::os

