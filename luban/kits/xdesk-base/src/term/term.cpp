module xdesk.term;

import std;

namespace xdesk::term {

namespace {

constexpr std::size_t kHistory = 5000;

std::vector<int> params_(std::string_view s) {
    std::vector<int> out;
    int v = -1;
    for (char c : s) {
        if (c >= '0' && c <= '9') v = (v < 0 ? 0 : v) * 10 + (c - '0');
        else if (c == ';' || c == ':') { out.push_back(v); v = -1; }
    }
    out.push_back(v);
    return out;
}

int p_(const std::vector<int>& p, std::size_t i, int fallback) {
    return i < p.size() && p[i] >= 0 ? p[i] : fallback;
}

}  // namespace

int width(char32_t c) {
    if (c == 0) return 0;
    // Combining marks and zero-width joiners.
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF)
        || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200B || c == 0x200C || c == 0x200D
        || (c >= 0xFE00 && c <= 0xFE0F))
        return 0;
    // East Asian wide and fullwidth, and the emoji planes.
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF)
        || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF)
        || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F)
        || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F)
        || (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

Screen::Screen(int rows, int cols) : rows_(std::max(rows, 1)), cols_(std::max(cols, 1)) {
    main_.assign(std::size_t(rows_) * cols_, Cell{});
    alt_ = main_;
    bottom_ = rows_ - 1;
}

Cell Screen::blank() const {
    Cell c;
    c.bg = pen_.bg;
    return c;
}

void Screen::resize(int rows, int cols) {
    rows = std::max(rows, 1);
    cols = std::max(cols, 1);
    if (rows == rows_ && cols == cols_) return;
    for (auto* g : {&main_, &alt_}) {
        std::vector<Cell> next(std::size_t(rows) * cols, Cell{});
        // Keep the bottom rows (where the cursor and the prompt are).
        const int shift = std::max(0, cy_ - (rows - 1));
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                const int from = r + shift;
                if (from < rows_ && c < cols_) next[std::size_t(r) * cols + c] = (*g)[std::size_t(from) * cols_ + c];
            }
        if (g == &main_ && shift > 0)
            for (int r = 0; r < shift && r < rows_; ++r)
                history_.emplace_back(main_.begin() + std::ptrdiff_t(r) * cols_, main_.begin() + std::ptrdiff_t(r + 1) * cols_);
        *g = std::move(next);
    }
    cy_ = std::min(cy_ - std::max(0, cy_ - (rows - 1)), rows - 1);
    rows_ = rows;
    cols_ = cols;
    cx_ = std::min(cx_, cols_ - 1);
    top_ = 0;
    bottom_ = rows_ - 1;
    while (history_.size() > kHistory) history_.pop_front();
    wrap_pending_ = false;
    dirty = true;
}

std::string Screen::take_replies() { return std::exchange(replies_, {}); }

void Screen::erase(int row, int from, int to) {
    auto& g = grid();
    for (int c = std::max(0, from); c < std::min(to, cols_); ++c) g[std::size_t(row) * cols_ + c] = blank();
}

void Screen::scroll_up(int top, int bottom, int n) {
    auto& g = grid();
    n = std::min(n, bottom - top + 1);
    if (n <= 0) return;
    if (!alt_active_ && top == 0)
        for (int r = 0; r < n; ++r) {
            history_.emplace_back(g.begin() + std::ptrdiff_t(top + r) * cols_, g.begin() + std::ptrdiff_t(top + r + 1) * cols_);
            if (history_.size() > kHistory) history_.pop_front();
        }
    for (int r = top; r <= bottom - n; ++r)
        std::copy_n(g.begin() + std::ptrdiff_t(r + n) * cols_, cols_, g.begin() + std::ptrdiff_t(r) * cols_);
    for (int r = bottom - n + 1; r <= bottom; ++r) erase(r, 0, cols_);
}

void Screen::scroll_down(int top, int bottom, int n) {
    auto& g = grid();
    n = std::min(n, bottom - top + 1);
    if (n <= 0) return;
    for (int r = bottom; r >= top + n; --r)
        std::copy_n(g.begin() + std::ptrdiff_t(r - n) * cols_, cols_, g.begin() + std::ptrdiff_t(r) * cols_);
    for (int r = top; r < top + n; ++r) erase(r, 0, cols_);
}

void Screen::index() {
    if (cy_ == bottom_) scroll_up(top_, bottom_, 1);
    else if (cy_ < rows_ - 1) ++cy_;
}

void Screen::reverse_index() {
    if (cy_ == top_) scroll_down(top_, bottom_, 1);
    else if (cy_ > 0) --cy_;
}

void Screen::newline() {
    index();
}

void Screen::put(char32_t ch) {
    const int w = width(ch);
    auto& g = grid();
    if (w == 0) {
        // A combining mark joins the cell before; this grid keeps one code
        // point per cell, so it is dropped rather than drawn on its own.
        return;
    }
    if (wrap_pending_) {
        if (autowrap_) {
            cx_ = 0;
            newline();
        }
        wrap_pending_ = false;
    }
    if (w == 2 && cx_ == cols_ - 1) {
        if (autowrap_) {
            erase(cy_, cx_, cols_);
            cx_ = 0;
            newline();
        } else {
            return;
        }
    }
    if (insert_) {
        auto row = g.begin() + std::ptrdiff_t(cy_) * cols_;
        std::move_backward(row + cx_, row + cols_ - w, row + cols_);
    }
    Cell c = pen_;
    c.ch = ch;
    c.continuation = false;
    g[std::size_t(cy_) * cols_ + cx_] = c;
    if (w == 2) {
        Cell cont = pen_;
        cont.ch = U' ';
        cont.continuation = true;
        g[std::size_t(cy_) * cols_ + cx_ + 1] = cont;
    }
    cx_ += w;
    if (cx_ >= cols_) {
        cx_ = cols_ - 1;
        wrap_pending_ = true;
    }
}

void Screen::control(char c) {
    switch (c) {
    case '\a': break;
    case '\b': if (cx_ > 0) --cx_; wrap_pending_ = false; break;
    case '\t': cx_ = std::min(cols_ - 1, (cx_ / 8 + 1) * 8); wrap_pending_ = false; break;
    case '\n': case '\v': case '\f': newline(); wrap_pending_ = false; break;
    case '\r': cx_ = 0; wrap_pending_ = false; break;
    case 0x0E: case 0x0F: break;   // shift out / in: one character set here
    default: ++unhandled; break;
    }
}

void Screen::sgr(const std::vector<int>& p) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        const int v = p[i] < 0 ? 0 : p[i];
        auto color = [&](Color& target) {
            if (i + 1 < p.size() && p[i + 1] == 5 && i + 2 < p.size()) {
                target = {Color::Kind::Indexed, static_cast<std::uint32_t>(std::clamp(p[i + 2], 0, 255))};
                i += 2;
            } else if (i + 1 < p.size() && p[i + 1] == 2 && i + 4 < p.size()) {
                target = {Color::Kind::Rgb, (std::uint32_t(std::clamp(p[i + 2], 0, 255)) << 16)
                                                | (std::uint32_t(std::clamp(p[i + 3], 0, 255)) << 8)
                                                | std::uint32_t(std::clamp(p[i + 4], 0, 255))};
                i += 4;
            }
        };
        switch (v) {
        case 0: pen_.fg = {}; pen_.bg = {}; pen_.attrs = 0; break;
        case 1: pen_.attrs |= Bold; break;
        case 2: pen_.attrs |= Faint; break;
        case 3: pen_.attrs |= Italic; break;
        case 4: pen_.attrs |= Underline; break;
        case 5: pen_.attrs |= Blink; break;
        case 7: pen_.attrs |= Inverse; break;
        case 8: pen_.attrs |= Hidden; break;
        case 9: pen_.attrs |= Strike; break;
        case 22: pen_.attrs &= ~(Bold | Faint); break;
        case 23: pen_.attrs &= ~Italic; break;
        case 24: pen_.attrs &= ~Underline; break;
        case 25: pen_.attrs &= ~Blink; break;
        case 27: pen_.attrs &= ~Inverse; break;
        case 28: pen_.attrs &= ~Hidden; break;
        case 29: pen_.attrs &= ~Strike; break;
        case 38: color(pen_.fg); break;
        case 39: pen_.fg = {}; break;
        case 48: color(pen_.bg); break;
        case 49: pen_.bg = {}; break;
        default:
            if (v >= 30 && v <= 37) pen_.fg = {Color::Kind::Indexed, std::uint32_t(v - 30)};
            else if (v >= 40 && v <= 47) pen_.bg = {Color::Kind::Indexed, std::uint32_t(v - 40)};
            else if (v >= 90 && v <= 97) pen_.fg = {Color::Kind::Indexed, std::uint32_t(v - 90 + 8)};
            else if (v >= 100 && v <= 107) pen_.bg = {Color::Kind::Indexed, std::uint32_t(v - 100 + 8)};
            else ++unhandled;
        }
    }
}

void Screen::set_mode(int mode, bool on, bool priv) {
    if (!priv) {
        if (mode == 4) insert_ = on;
        else if (mode != 20) ++unhandled;
        return;
    }
    switch (mode) {
    case 1: app_cursor_ = on; break;
    case 7: autowrap_ = on; break;
    case 12: break;   // a blinking cursor
    case 25: cursor_visible_ = on; break;
    case 47: case 1047: case 1049:
        if (on == alt_active_) break;
        if (mode == 1049 && on) saved_ = {cx_, cy_, pen_};
        alt_active_ = on;
        if (on) std::fill(alt_.begin(), alt_.end(), blank());
        if (mode == 1049 && !on) { cx_ = saved_.cx; cy_ = saved_.cy; pen_ = saved_.pen; }
        top_ = 0;
        bottom_ = rows_ - 1;
        break;
    case 1000: case 1002: case 1003: mouse_mode_ = on ? mode : 0; break;
    case 1006: mouse_sgr_ = on; break;
    case 1004: case 1005: case 1015: break;
    case 2004: bracketed_paste_ = on; break;
    default: ++unhandled;
    }
}

void Screen::csi(char final, std::string_view raw, char prefix, char intermediate) {
    const auto p = params_(raw);
    const int n = p_(p, 0, 1);
    switch (final) {
    case 'A': cy_ = std::max(cy_ < top_ ? 0 : top_, cy_ - n); break;
    case 'B': case 'e': cy_ = std::min(cy_ > bottom_ ? rows_ - 1 : bottom_, cy_ + n); break;
    case 'C': case 'a': cx_ = std::min(cols_ - 1, cx_ + n); break;
    case 'D': cx_ = std::max(0, std::min(cx_, cols_ - 1) - n); break;
    case 'E': cy_ = std::min(bottom_, cy_ + n); cx_ = 0; break;
    case 'F': cy_ = std::max(top_, cy_ - n); cx_ = 0; break;
    case 'G': case '`': cx_ = std::clamp(n - 1, 0, cols_ - 1); break;
    case 'd': cy_ = std::clamp(n - 1, 0, rows_ - 1); break;
    case 'H': case 'f':
        cy_ = std::clamp(p_(p, 0, 1) - 1, 0, rows_ - 1);
        cx_ = std::clamp(p_(p, 1, 1) - 1, 0, cols_ - 1);
        break;
    case 'J': {
        const int mode = p_(p, 0, 0);
        if (mode == 0) { erase(cy_, cx_, cols_); for (int r = cy_ + 1; r < rows_; ++r) erase(r, 0, cols_); }
        else if (mode == 1) { erase(cy_, 0, cx_ + 1); for (int r = 0; r < cy_; ++r) erase(r, 0, cols_); }
        else if (mode == 2 || mode == 3) { for (int r = 0; r < rows_; ++r) erase(r, 0, cols_); if (mode == 3) history_.clear(); }
        break;
    }
    case 'K': {
        const int mode = p_(p, 0, 0);
        if (mode == 0) erase(cy_, cx_, cols_);
        else if (mode == 1) erase(cy_, 0, cx_ + 1);
        else erase(cy_, 0, cols_);
        break;
    }
    case 'L': if (cy_ >= top_ && cy_ <= bottom_) scroll_down(cy_, bottom_, n); break;
    case 'M': if (cy_ >= top_ && cy_ <= bottom_) scroll_up(cy_, bottom_, n); break;
    case 'S': scroll_up(top_, bottom_, n); break;
    case 'T': scroll_down(top_, bottom_, n); break;
    case 'P': {
        auto& g = grid();
        auto row = g.begin() + std::ptrdiff_t(cy_) * cols_;
        const int k = std::min(n, cols_ - cx_);
        std::move(row + cx_ + k, row + cols_, row + cx_);
        erase(cy_, cols_ - k, cols_);
        break;
    }
    case '@': {
        auto& g = grid();
        auto row = g.begin() + std::ptrdiff_t(cy_) * cols_;
        const int k = std::min(n, cols_ - cx_);
        std::move_backward(row + cx_, row + cols_ - k, row + cols_);
        erase(cy_, cx_, cx_ + k);
        break;
    }
    case 'X': erase(cy_, cx_, cx_ + n); break;
    case 'r':
        top_ = std::clamp(p_(p, 0, 1) - 1, 0, rows_ - 1);
        bottom_ = std::clamp(p_(p, 1, rows_) - 1, 0, rows_ - 1);
        if (top_ >= bottom_) { top_ = 0; bottom_ = rows_ - 1; }
        cx_ = 0;
        cy_ = 0;
        break;
    case 'm':
        if (prefix == 0 && intermediate == 0) sgr(p);
        break;
    case 'h': case 'l':
        for (int m : p) if (m >= 0) set_mode(m, final == 'h', prefix == '?');
        break;
    case 'n':
        if (p_(p, 0, 0) == 6) replies_ += std::format("\x1b[{};{}R", cy_ + 1, std::min(cx_, cols_ - 1) + 1);
        else if (p_(p, 0, 0) == 5) replies_ += "\x1b[0n";
        break;
    case 'c':
        if (prefix == '>') replies_ += "\x1b[>0;10;0c";
        else if (prefix == 0) replies_ += "\x1b[?62;22c";
        break;
    case 's': saved_ = {cx_, cy_, pen_}; break;
    case 'u': cx_ = saved_.cx; cy_ = saved_.cy; pen_ = saved_.pen; break;
    case 'q':
        // DECSCUSR: 0-2 block, 3-4 underline, 5-6 bar (blinking or not).
        if (intermediate == ' ') {
            const int n = p_(p, 0, 0);
            cursor_shape_ = n >= 5 ? CursorShape::Bar : n >= 3 ? CursorShape::Underline : CursorShape::Block;
        }
        break;
    case 't': break;   // window ops: nothing to do here
    default: ++unhandled;
    }
    wrap_pending_ = false;
}

namespace {
// RFC 4648 base64, what OSC 52 carries; nothing on a malformed one.
std::optional<std::string> base64_(std::string_view in) {
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += static_cast<char>((acc >> bits) & 0xff); }
    }
    return out;
}
}  // namespace

void Screen::osc(std::string_view body) {
    const auto semi = body.find(';');
    const auto code = body.substr(0, semi);
    if ((code == "0" || code == "2") && semi != std::string_view::npos) title_ = std::string(body.substr(semi + 1));
    else if (code == "52" && semi != std::string_view::npos) {
        // OSC 52 ; <selection> ; <base64> -- a query ("?") is not answered:
        // reading the host's clipboard is not a program's to ask for.
        const auto rest = body.substr(semi + 1);
        const auto second = rest.find(';');
        if (second != std::string_view::npos && rest.substr(second + 1) != "?")
            if (auto text = base64_(rest.substr(second + 1))) clipboard_ = std::move(*text);
    } else if (code != "1" && code != "7" && code != "8" && code != "133") ++unhandled;
}

void Screen::esc(char c, char intermediate) {
    if (intermediate == '(' || intermediate == ')' || intermediate == '*' || intermediate == '+') return;  // charsets
    if (intermediate == '#') return;
    switch (c) {
    case '7': saved_ = {cx_, cy_, pen_}; break;
    case '8': cx_ = saved_.cx; cy_ = saved_.cy; pen_ = saved_.pen; break;
    case 'D': index(); break;
    case 'E': cx_ = 0; index(); break;
    case 'M': reverse_index(); break;
    case 'c': *this = Screen(rows_, cols_); break;
    case '=': case '>': break;   // keypad modes
    default: ++unhandled;
    }
    wrap_pending_ = false;
}

void Screen::feed(std::string_view bytes) {
    if (!bytes.empty()) dirty = true;
    for (const unsigned char b : bytes) {
        switch (state_) {
        case State::Ground:
            if (utf8_left_ > 0) {
                if ((b & 0xC0) == 0x80) {
                    utf8_ = (utf8_ << 6) | (b & 0x3F);
                    if (--utf8_left_ == 0) put(static_cast<char32_t>(utf8_));
                    continue;
                }
                utf8_left_ = 0;
                put(U'�');
            }
            if (b == 0x1B) { state_ = State::Escape; seq_.clear(); intermediate_ = 0; }
            else if (b < 0x20 || b == 0x7F) { if (b != 0x7F) control(static_cast<char>(b)); }
            else if (b < 0x80) put(b);
            else if ((b & 0xE0) == 0xC0) { utf8_ = b & 0x1F; utf8_left_ = 1; }
            else if ((b & 0xF0) == 0xE0) { utf8_ = b & 0x0F; utf8_left_ = 2; }
            else if ((b & 0xF8) == 0xF0) { utf8_ = b & 0x07; utf8_left_ = 3; }
            else put(U'�');
            break;
        case State::Escape:
            if (b == '[') { state_ = State::Csi; seq_.clear(); }
            else if (b == ']') { state_ = State::Osc; seq_.clear(); }
            else if (b == 'P' || b == '_' || b == '^') { state_ = State::Osc; seq_.clear(); }   // DCS / APC / PM: swallowed like OSC
            else if (b >= 0x20 && b <= 0x2F) { intermediate_ = static_cast<char>(b); state_ = State::Charset; }
            else { esc(static_cast<char>(b), 0); state_ = State::Ground; }
            break;
        case State::Charset:
            esc(static_cast<char>(b), intermediate_);
            state_ = State::Ground;
            break;
        case State::Csi:
            if (b >= 0x40 && b <= 0x7E) {
                char prefix = 0, inter = 0;
                std::string_view s = seq_;
                if (!s.empty() && (s.front() == '?' || s.front() == '>' || s.front() == '=' || s.front() == '<')) {
                    prefix = s.front();
                    s.remove_prefix(1);
                }
                if (!s.empty() && s.back() >= 0x20 && s.back() <= 0x2F) { inter = s.back(); s.remove_suffix(1); }
                csi(static_cast<char>(b), s, prefix, inter);
                state_ = State::Ground;
            } else if (b == 0x1B) {
                state_ = State::Escape;
            } else if (b < 0x20) {
                control(static_cast<char>(b));
            } else if (seq_.size() < 256) {
                seq_ += static_cast<char>(b);
            }
            break;
        case State::Osc:
            if (b == 0x07) { osc(seq_); state_ = State::Ground; }
            else if (b == 0x1B) state_ = State::OscEscape;
            else if (seq_.size() < 4096) seq_ += static_cast<char>(b);
            break;
        case State::OscEscape:
            if (b == '\\') { osc(seq_); state_ = State::Ground; }
            else { osc(seq_); state_ = State::Escape; }
            break;
        }
    }
}

std::string encode_key(std::string_view key, std::uint32_t mods, bool app) {
    const bool shift = mods & 1, ctrl = mods & 2, alt = mods & 4;
    const int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    auto cursor = [&](char c) {
        if (m > 1) return std::format("\x1b[1;{}{}", m, c);
        return std::format("{}{}", app ? "\x1bO" : "\x1b[", c);
    };
    auto tilde = [&](int n) { return m > 1 ? std::format("\x1b[{};{}~", n, m) : std::format("\x1b[{}~", n); };
    if (key == "Up") return cursor('A');
    if (key == "Down") return cursor('B');
    if (key == "Right") return cursor('C');
    if (key == "Left") return cursor('D');
    if (key == "Home") return cursor('H');
    if (key == "End") return cursor('F');
    if (key == "Insert") return tilde(2);
    if (key == "Delete") return tilde(3);
    if (key == "PageUp") return tilde(5);
    if (key == "PageDown") return tilde(6);
    if (key == "Enter") return alt ? "\x1b\r" : "\r";
    if (key == "Tab") return shift ? "\x1b[Z" : "\t";
    if (key == "Backspace") return alt ? "\x1b\x7f" : ctrl ? "\x08" : "\x7f";
    if (key == "Escape") return "\x1b";
    if (key.size() >= 2 && key[0] == 'F') {
        int f = 0;
        std::from_chars(key.data() + 1, key.data() + key.size(), f);
        static const std::array<std::string_view, 4> pf{"P", "Q", "R", "S"};
        if (f >= 1 && f <= 4) return m > 1 ? std::format("\x1b[1;{}{}", m, pf[f - 1]) : std::format("\x1bO{}", pf[f - 1]);
        static const std::array<int, 8> codes{15, 17, 18, 19, 20, 21, 23, 24};
        if (f >= 5 && f <= 12) return tilde(codes[f - 5]);
    }
    if (key.size() == 1) {
        char c = key[0];
        std::string out;
        if (ctrl) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 1);
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 1);
            else if (c == ' ' || c == '@' || c == '2') c = 0;
            else if (c == '[' || c == '3') c = 0x1b;
            else if (c == '\\' || c == '4') c = 0x1c;
            else if (c == ']' || c == '5') c = 0x1d;
            else if (c == '/' || c == '_') c = 0x1f;
        }
        if (alt) out += '\x1b';
        out += c;
        return out;
    }
    return {};
}

std::string encode_paste(std::string_view text, bool bracketed) {
    if (!bracketed) return std::string(text);
    std::string out = "\x1b[200~";
    for (char c : text) if (c != '\x1b') out += c;   // nothing inside closes the bracket
    out += "\x1b[201~";
    return out;
}

}  // namespace xdesk::term
