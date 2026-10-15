module imkit;

import std;
import imkit.imgui;

namespace imkit {

namespace {

ImU32 rgb_(std::uint32_t v, float a = 1.0f) {
    return IM_COL32(static_cast<int>((v >> 16) & 0xff), static_cast<int>((v >> 8) & 0xff), static_cast<int>(v & 0xff),
                    static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255));
}

ImU32 alpha_(ImU32 c, float a) {
    const auto old = static_cast<float>((c >> 24) & 0xff) / 255.0f;
    return (c & 0x00ffffffu) | (static_cast<ImU32>(std::clamp(old * a, 0.0f, 1.0f) * 255) << 24);
}

// `c` moved toward white (t > 0) or black (t < 0).
ImU32 shade_(ImU32 c, float t) {
    auto ch = [&](int shift) {
        const float v = static_cast<float>((c >> shift) & 0xff);
        const float to = t > 0 ? 255.0f : 0.0f;
        return static_cast<ImU32>(std::clamp(v + (to - v) * std::abs(t), 0.0f, 255.0f)) << shift;
    };
    return (c & 0xff000000u) | ch(0) | ch(8) | ch(16);
}

// Grey of the same lightness: what a disabled thing is drawn in.
ImU32 grey_(ImU32 c) {
    const float r = static_cast<float>(c & 0xff), g = static_cast<float>((c >> 8) & 0xff), b = static_cast<float>((c >> 16) & 0xff);
    const auto y = static_cast<ImU32>(0.3f * r + 0.59f * g + 0.11f * b);
    return (c & 0xff000000u) | y | (y << 8) | (y << 16);
}

struct State {
    Theme theme = default_theme();
    float scale { 1 };
    double time { 0 };
    bool reduce_motion { false };
    bool animating { false };
    std::function<void(std::string_view, ImVec2, ImVec2)> sink;
};
State& st() {
    static State s;
    return s;
}

ImFont* font_(bool title) {
    auto* f = static_cast<ImFont*>(title ? st().theme.title_font : st().theme.font);
    if (!f) f = static_cast<ImFont*>(st().theme.font);
    return f ? f : ImGui::GetFont();
}

float size_(float nominal) { return (nominal > 0 ? nominal : st().theme.text_size) * st().scale; }

// An item `size` (already in pixels) at the cursor; its id is pushed by the caller.
bool item_(ImVec2 size, bool& hovered, bool& held) {
    const bool clicked = ImGui::InvisibleButton("##imkit", size);
    hovered = ImGui::IsItemHovered();
    held = ImGui::IsItemActive();
    return clicked;
}

struct Id {
    explicit Id(std::string_view id) : name(id) { ImGui::PushID(id.data(), id.data() + id.size()); }
    ~Id() { ImGui::PopID(); }
    std::string_view name;
};

// The icons, 12 x 12: X is ink.
constexpr std::array<std::string_view, 12> kGear{
    "............", ".....XX.....", "..X.XXXX.X..", "...XXXXXX...", "..XXX..XXX..", ".XXX....XXX.",
    ".XXX....XXX.", "..XXX..XXX..", "...XXXXXX...", "..X.XXXX.X..", ".....XX.....", "............"};
constexpr std::array<std::string_view, 12> kPower{
    ".....XX.....", ".....XX.....", "..XX.XX.XX..", ".XX..XX..XX.", "XX...XX...XX", "XX........XX",
    "XX........XX", "XX........XX", ".XX......XX.", "..XX....XX..", "...XXXXXX...", "............"};
constexpr std::array<std::string_view, 12> kMoon{
    "............", "....XXXX....", "..XXX.......", ".XXX........", ".XX.........", "XXX.........",
    "XXX.........", ".XX.........", ".XXX......X.", "..XXX...XX..", "....XXXXX...", "............"};
constexpr std::array<std::string_view, 12> kCheck{
    "............", "............", "..........XX", ".........XX.", "........XX..", ".......XX...",
    "XX....XX....", ".XX..XX.....", "..XXXX......", "...XX.......", "............", "............"};
constexpr std::array<std::string_view, 12> kPlus{
    "............", ".....XX.....", ".....XX.....", ".....XX.....", ".....XX.....", ".XXXXXXXXXX.",
    ".XXXXXXXXXX.", ".....XX.....", ".....XX.....", ".....XX.....", ".....XX.....", "............"};
constexpr std::array<std::string_view, 12> kImport{
    ".....XX.....", ".....XX.....", ".....XX.....", ".....XX.....", "..XX.XX.XX..", "...XXXXXX...",
    "....XXXX....", ".....XX.....", "X..........X", "X..........X", "XXXXXXXXXXXX", "............"};
constexpr std::array<std::string_view, 12> kWarn{
    ".....XX.....", "....XXXX....", "....X..X....", "...XX..XX...", "...X.XX.X...", "..XX.XX.XX..",
    "..X..XX..X..", ".XX..XX..XX.", ".X........X.", "XX...XX...XX", "XXXXXXXXXXXX", "............"};
constexpr std::array<std::string_view, 12> kCross{
    "............", ".XX......XX.", "..XX....XX..", "...XX..XX...", "....XXXX....", ".....XX.....",
    "....XXXX....", "...XX..XX...", "..XX....XX..", ".XX......XX.", "............", "............"};
constexpr std::array<std::string_view, 12> kBack{
    "............", ".....X......", "....XX......", "...XX.......", "..XXXXXXXXX.", ".XXXXXXXXXX.",
    "..XXXXXXXXX.", "...XX.......", "....XX......", ".....X......", "............", "............"};
constexpr std::array<std::string_view, 12> kTerminal{
    "XXXXXXXXXXXX", "X..........X", "X.X........X", "X..X.......X", "X...X......X", "X..X.......X",
    "X.X...XXX..X", "X..........X", "X..........X", "XXXXXXXXXXXX", "............", "............"};
constexpr std::array<std::string_view, 12> kWindow{
    "XXXXXXXXXXXX", "XXXXXXXXXXXX", "X..........X", "X..........X", "X..........X", "X..........X",
    "X..........X", "X..........X", "X..........X", "XXXXXXXXXXXX", "............", "............"};
constexpr std::array<std::string_view, 12> kRun{
    "............", "..X.........", "..XX........", "..XXX.......", "..XXXX......", "..XXXXX.....",
    "..XXXXX.....", "..XXXX......", "..XXX.......", "..XX........", "..X.........", "............"};
constexpr std::array<std::string_view, 12> kDot{
    "............", "............", "............", "............", "....XXXX....", "...XXXXXX...",
    "...XXXXXX...", "....XXXX....", "............", "............", "............", "............"};

}  // namespace

// ── the theme ─────────────────────────────────────────────────────────

Theme default_theme() {
    Theme t;
    t.bg = rgb_(0x1a1c2c);
    t.bg_deep = rgb_(0x121320);
    t.panel = rgb_(0x333c57);
    t.panel_hi = rgb_(0x566c86);
    t.panel_lo = rgb_(0x232842);
    t.line = rgb_(0x3b4766);
    t.ink = rgb_(0x0d0e17);
    t.text = rgb_(0xf4f4f4);
    t.text_dim = rgb_(0xa6bccc);
    t.text_faint = rgb_(0x75859f);
    t.accent = rgb_(0x41a6f6);
    t.accent_hi = rgb_(0x73eff7);
    t.accent_lo = rgb_(0x3b5dc9);
    t.good = rgb_(0x38b764);
    t.warn = rgb_(0xffcd75);
    t.bad = rgb_(0xd94d5c);
    t.focus = rgb_(0xffcd75);
    t.shadow = rgb_(0x0b0c14);
    return t;
}

void set_theme(const Theme& t) { st().theme = t; }
const Theme& theme() { return st().theme; }

// ── the frame ─────────────────────────────────────────────────────────

void begin_frame(float scale, double time, bool reduce_motion) {
    st().scale = scale > 0 ? scale : 1.0f;
    st().time = time;
    st().reduce_motion = reduce_motion;
    st().animating = false;
}
float scale() { return st().scale; }
double time() { return st().time; }
bool reduce_motion() { return st().reduce_motion; }
float px(float nominal) { return std::round(nominal * st().scale); }
float unit() { return static_cast<float>(st().theme.unit) * std::max(1.0f, std::round(st().scale)); }
ImVec2 snap(ImVec2 p) { return ImVec2(std::round(p.x), std::round(p.y)); }
int beat(int frames, double period, int phase) {
    if (st().reduce_motion || frames <= 1) return 0;
    st().animating = true;
    return (static_cast<int>(std::floor(st().time / period)) + phase) % frames;
}
bool animating() { return st().animating; }
void set_animating() { if (!st().reduce_motion) st().animating = true; }

// ── pixel art ─────────────────────────────────────────────────────────

void pixels(ImDrawList* dl, const PixelMap& map, ImVec2 at, float cell, const Palette& palette) {
    at = snap(at);
    for (int y = 0; y < map.height && y < static_cast<int>(map.rows.size()); ++y) {
        const auto row = map.rows[static_cast<std::size_t>(y)];
        int x = 0;
        while (x < map.width && x < static_cast<int>(row.size())) {
            const char c = row[static_cast<std::size_t>(x)];
            int run = 1;
            while (x + run < map.width && x + run < static_cast<int>(row.size()) && row[static_cast<std::size_t>(x + run)] == c) ++run;
            if (const ImU32 color = palette(c); color & 0xff000000u)
                dl->AddRectFilled(ImVec2(at.x + x * cell, at.y + y * cell), ImVec2(at.x + (x + run) * cell, at.y + (y + 1) * cell), color);
            x += run;
        }
    }
}

void icon(ImDrawList* dl, Icon i, ImVec2 at, float cell, ImU32 ink) {
    const std::array<std::string_view, 12>* rows = nullptr;
    switch (i) {
    case Icon::Gear: rows = &kGear; break;
    case Icon::Power: rows = &kPower; break;
    case Icon::Moon: rows = &kMoon; break;
    case Icon::Check: rows = &kCheck; break;
    case Icon::Plus: rows = &kPlus; break;
    case Icon::Import: rows = &kImport; break;
    case Icon::Warn: rows = &kWarn; break;
    case Icon::Cross: rows = &kCross; break;
    case Icon::Back: rows = &kBack; break;
    case Icon::Terminal: rows = &kTerminal; break;
    case Icon::Window: rows = &kWindow; break;
    case Icon::Run: rows = &kRun; break;
    case Icon::Dot: rows = &kDot; break;
    }
    pixels(dl, {kIconSize, kIconSize, std::span<const std::string_view>(*rows)}, at, cell,
           [ink](char c) -> ImU32 { return c == 'X' ? ink : 0; });
}

// ── surfaces ──────────────────────────────────────────────────────────

void panel(ImDrawList* dl, ImVec2 a, ImVec2 b, Bevel bevel, ImU32 fill) {
    const auto& t = st().theme;
    a = snap(a);
    b = snap(b);
    const float u = std::max(1.0f, unit() / 3.0f * 1.0f);   // an edge is a third of a unit: crisp, not heavy
    const ImU32 face = fill ? fill : t.panel;
    dl->AddRectFilled(a, b, face);
    if (bevel == Bevel::Flat) {
        dl->AddRect(a, b, t.line, 0, 0, u);
        return;
    }
    const ImU32 light = bevel == Bevel::Raised ? shade_(face, 0.22f) : shade_(face, -0.45f);
    const ImU32 dark = bevel == Bevel::Raised ? shade_(face, -0.45f) : shade_(face, 0.18f);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + u), light);                 // top
    dl->AddRectFilled(a, ImVec2(a.x + u, b.y), light);                 // left
    dl->AddRectFilled(ImVec2(a.x, b.y - u), b, dark);                  // bottom
    dl->AddRectFilled(ImVec2(b.x - u, a.y), b, dark);                  // right
    // An outline in ink, with the corners cut: the pixel look.
    const ImU32 ink = t.ink;
    dl->AddRectFilled(ImVec2(a.x + u, a.y - u), ImVec2(b.x - u, a.y), ink);
    dl->AddRectFilled(ImVec2(a.x + u, b.y), ImVec2(b.x - u, b.y + u), ink);
    dl->AddRectFilled(ImVec2(a.x - u, a.y + u), ImVec2(a.x, b.y - u), ink);
    dl->AddRectFilled(ImVec2(b.x, a.y + u), ImVec2(b.x + u, b.y - u), ink);
}

void focus_ring(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float u = unit() * 2.0f / 3.0f;
    float alpha = 1.0f;
    if (!st().reduce_motion) {
        alpha = 0.65f + 0.35f * static_cast<float>(0.5 + 0.5 * std::sin(st().time * 5.0));
        st().animating = true;
    }
    const ImU32 c = alpha_(st().theme.focus, alpha);
    a = snap(ImVec2(a.x - u * 2, a.y - u * 2));
    b = snap(ImVec2(b.x + u * 2, b.y + u * 2));
    dl->AddRectFilled(a, ImVec2(b.x, a.y + u), c);
    dl->AddRectFilled(ImVec2(a.x, b.y - u), b, c);
    dl->AddRectFilled(ImVec2(a.x, a.y + u), ImVec2(a.x + u, b.y - u), c);
    dl->AddRectFilled(ImVec2(b.x - u, a.y + u), ImVec2(b.x, b.y - u), c);
}

// ── text ──────────────────────────────────────────────────────────────

void text(ImDrawList* dl, ImVec2 at, std::string_view s, ImU32 color, float size, bool title) {
    dl->AddText(font_(title), size_(size), snap(at), color, s.data(), s.data() + s.size());
}

ImVec2 text_size(std::string_view s, float size, bool title) {
    return font_(title)->CalcTextSizeA(size_(size), std::numeric_limits<float>::max(), 0, s.data(), s.data() + s.size());
}

namespace {
// The UTF-8 characters of `s`, as byte ranges.
std::vector<std::string_view> chars_(std::string_view s) {
    std::vector<std::string_view> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const std::size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        out.push_back(s.substr(i, std::min(n, s.size() - i)));
        i += n;
    }
    return out;
}
bool wide_(std::string_view ch) { return ch.size() >= 3; }
}  // namespace

std::vector<std::string> wrap(std::string_view s, float width, float size) {
    std::vector<std::string> lines;
    std::string line, word;
    bool space = false;   // the text had a space before what comes next
    auto width_of = [&](std::string_view t) { return text_size(t, size).x; };
    auto place = [&](const std::string& piece) {
        const std::string sep = space && !line.empty() ? " " : "";
        space = false;
        if (line.empty() || width_of(line + sep + piece) <= width) line += sep + piece;
        else { lines.push_back(line); line = piece; }
    };
    auto push_word = [&] {
        if (word.empty()) return;
        place(word);
        word.clear();
    };
    for (const auto ch : chars_(s)) {
        if (ch == "\n") { push_word(); lines.push_back(line); line.clear(); space = false; continue; }
        if (ch == " ") { push_word(); space = true; continue; }
        if (wide_(ch)) { push_word(); place(std::string(ch)); continue; }   // a CJK character breaks anywhere
        word += ch;
    }
    push_word();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

std::string fit(std::string_view s, float width, float size, bool title) {
    if (text_size(s, size, title).x <= width) return std::string(s);
    std::string out;
    for (const auto ch : chars_(s)) {
        if (text_size(out + std::string(ch) + "…", size, title).x > width) break;
        out += ch;
    }
    return out + "…";
}

// ── components ────────────────────────────────────────────────────────

bool button(std::string_view id, std::string_view label, ImVec2 size, Look look, bool enabled, bool focused,
            std::optional<Icon> icon_) {
    const auto& t = st().theme;
    Id scope(id);
    const ImVec2 sz(px(size.x), px(size.y));
    const ImVec2 at = snap(ImGui::GetCursorScreenPos());
    ImGui::SetCursorScreenPos(at);
    bool hovered = false, held = false;
    const bool clicked = item_(sz, hovered, held) && enabled;
    anchor(id);
    auto* dl = ImGui::GetWindowDrawList();
    const float u = unit() / 3.0f;
    ImU32 face = look == Look::Primary ? t.accent_lo : look == Look::Danger ? shade_(t.bad, -0.25f) : t.panel;
    if (enabled && hovered && !held) face = shade_(face, 0.12f);
    if (!enabled) face = grey_(shade_(face, -0.2f));
    const float lift = !enabled ? 0 : held ? u : hovered ? -u : 0;
    const ImVec2 a(at.x, at.y + lift), b(at.x + sz.x, at.y + sz.y + lift);
    if (look == Look::Ghost) {
        if (hovered && enabled) dl->AddRectFilled(a, b, alpha_(t.panel, 0.6f));
    } else {
        if (!held) dl->AddRectFilled(ImVec2(at.x + u, at.y + sz.y), ImVec2(at.x + sz.x + u, at.y + sz.y + u * 2), t.shadow);
        panel(dl, a, b, held ? Bevel::Sunken : Bevel::Raised, face);
    }
    const ImU32 ink = !enabled ? t.text_faint : look == Look::Ghost && !hovered ? t.text_dim : t.text;
    const float cell = std::max(1.0f, std::round(px(1.5f)));
    const float iconW = icon_ ? kIconSize * cell : 0, gap = icon_ && !label.empty() ? px(8) : 0;
    const auto shown = fit(label, sz.x - px(16) - iconW - gap);
    const ImVec2 ts = text_size(shown);
    const float x = a.x + (sz.x - ts.x - iconW - gap) / 2, cy = a.y + sz.y / 2;
    if (icon_) icon(dl, *icon_, ImVec2(x, cy - kIconSize * cell / 2), cell, ink);
    text(dl, ImVec2(x + iconW + gap, cy - ts.y / 2), shown, ink);
    if (focused) focus_ring(dl, a, b);
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

bool option_card(std::string_view id, const Picture& picture, std::string_view title, std::string_view description,
                 bool selected, ImVec2 size, bool focused, bool enabled) {
    const auto& t = st().theme;
    Id scope(id);
    const ImVec2 sz(px(size.x), px(size.y));
    const ImVec2 at = snap(ImGui::GetCursorScreenPos());
    ImGui::SetCursorScreenPos(at);
    bool hovered = false, held = false;
    const bool clicked = item_(sz, hovered, held) && enabled;
    anchor(id);
    auto* dl = ImGui::GetWindowDrawList();
    const float u = unit() / 3.0f;
    const float lift = selected ? -u * 2 : 0;
    const ImVec2 a(at.x, at.y + lift + u * 2), b(at.x + sz.x, at.y + sz.y + lift);
    if (selected) {
        dl->AddRectFilled(ImVec2(a.x + u * 2, b.y), ImVec2(b.x + u * 2, b.y + u * 3), t.shadow);
        panel(dl, a, b, Bevel::Raised, shade_(t.panel, 0.08f));
        // The outline: two units of accent, inside the edge.
        const float w = unit() * 2.0f / 3.0f;
        dl->AddRect(ImVec2(a.x + w / 2, a.y + w / 2), ImVec2(b.x - w / 2, b.y - w / 2), t.accent, 0, 0, w);
        // The tick, in a square at the corner.
        const float cell = std::max(1.0f, std::round(px(1.5f)));
        const float box = kIconSize * cell + px(6);
        const ImVec2 ta(b.x - box - px(6), a.y + px(6));
        dl->AddRectFilled(ta, ImVec2(ta.x + box, ta.y + box), t.accent);
        icon(dl, Icon::Check, ImVec2(ta.x + px(3), ta.y + px(3)), cell, t.ink);
    } else {
        dl->AddRectFilled(a, b, hovered && enabled ? shade_(t.panel_lo, 0.08f) : t.panel_lo);
        dl->AddRect(a, b, hovered && enabled ? t.panel_hi : t.line, 0, 0, std::max(1.0f, u));
    }
    const float pad = px(14);
    float y = a.y + pad;
    if (picture.draw && picture.size > 0) {
        const float cell = std::max(1.0f, std::round(px(2)));
        picture.draw(dl, ImVec2(a.x + pad, y), cell);
        y += picture.size * cell + px(10);
    }
    const ImU32 ink = !enabled ? t.text_faint : t.text;
    text(dl, ImVec2(a.x + pad, y), fit(title, sz.x - pad * 2, 18, true), ink, 18, true);
    y += text_size("Ag", 18, true).y + px(6);
    const float line_h = text_size("Ag", 14).y;
    for (const auto& line : wrap(description, sz.x - pad * 2, 14)) {
        if (y + line_h > b.y - pad / 2) break;
        text(dl, ImVec2(a.x + pad, y), line, !enabled ? t.text_faint : selected ? t.text_dim : t.text_faint, 14);
        y += line_h + px(2);
    }
    if (focused) focus_ring(dl, a, b);
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

bool list_row(std::string_view id, ImVec2 size, bool selected, bool focused) {
    const auto& t = st().theme;
    Id scope(id);
    const ImVec2 sz(size.x, px(size.y));   // the width is the caller's, already in pixels
    const ImVec2 at = snap(ImGui::GetCursorScreenPos());
    ImGui::SetCursorScreenPos(at);
    bool hovered = false, held = false;
    const bool clicked = item_(sz, hovered, held);
    anchor(id);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(at.x + sz.x, at.y + sz.y);
    if (selected) {
        dl->AddRectFilled(at, b, shade_(t.accent_lo, -0.35f));
        dl->AddRectFilled(at, ImVec2(at.x + unit(), b.y), t.accent);
    } else if (hovered) {
        dl->AddRectFilled(at, b, t.panel_lo);
    }
    // Inside the row: a list clips what is outside it.
    if (focused) focus_ring(dl, ImVec2(at.x + unit() * 2, at.y + unit() * 2), ImVec2(b.x - unit() * 2, b.y - unit() * 2));
    return clicked;
}

bool checkbox(std::string_view id, std::string_view label, bool& value, bool focused) {
    const auto& t = st().theme;
    Id scope(id);
    const float box = px(20);
    const ImVec2 ts = text_size(label);
    const ImVec2 sz(box + px(10) + ts.x, std::max(box, ts.y));
    const ImVec2 at = snap(ImGui::GetCursorScreenPos());
    ImGui::SetCursorScreenPos(at);
    bool hovered = false, held = false;
    const bool clicked = item_(sz, hovered, held);
    if (clicked) value = !value;
    anchor(id);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(at.x, at.y + (sz.y - box) / 2), b(a.x + box, a.y + box);
    panel(dl, a, b, Bevel::Sunken, value ? t.accent_lo : t.bg_deep);
    if (value) {
        const float cell = std::max(1.0f, std::floor((box - px(4)) / kIconSize));
        icon(dl, Icon::Check, ImVec2(a.x + (box - kIconSize * cell) / 2, a.y + (box - kIconSize * cell) / 2), cell, t.text);
    }
    text(dl, ImVec2(b.x + px(10), at.y + (sz.y - ts.y) / 2), label, hovered ? t.text : t.text_dim);
    if (focused) focus_ring(dl, at, ImVec2(at.x + sz.x, at.y + sz.y));
    return clicked;
}

bool toggle(std::string_view id, std::string_view label, bool& value, bool focused) {
    const auto& t = st().theme;
    Id scope(id);
    const float w = px(40), h = px(20);
    const ImVec2 ts = text_size(label);
    const ImVec2 sz(w + px(10) + ts.x, std::max(h, ts.y));
    const ImVec2 at = snap(ImGui::GetCursorScreenPos());
    ImGui::SetCursorScreenPos(at);
    bool hovered = false, held = false;
    const bool clicked = item_(sz, hovered, held);
    if (clicked) value = !value;
    anchor(id);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(at.x, at.y + (sz.y - h) / 2), b(a.x + w, a.y + h);
    panel(dl, a, b, Bevel::Sunken, value ? shade_(t.good, -0.35f) : t.bg_deep);
    // The knob: on the right and lit when on, on the left and dark when off.
    const float k = h - px(4);
    const ImVec2 ka(value ? b.x - k - px(2) : a.x + px(2), a.y + px(2));
    panel(dl, ka, ImVec2(ka.x + k, ka.y + k), Bevel::Raised, value ? t.good : t.panel_hi);
    text(dl, ImVec2(b.x + px(10), at.y + (sz.y - ts.y) / 2), label, hovered ? t.text : t.text_dim);
    if (focused) focus_ring(dl, at, ImVec2(at.x + sz.x, at.y + sz.y));
    return clicked;
}

bool text_field(std::string_view id, char* buffer, std::size_t size, float width, int flags, std::string_view hint) {
    const auto& t = st().theme;
    Id scope(id);
    const float w = px(width);
    const float h = text_size("Ag").y + px(14);
    const ImVec2 at = snap(ImGui::GetCursorScreenPos());
    auto* dl = ImGui::GetWindowDrawList();
    panel(dl, at, ImVec2(at.x + w, at.y + h), Bevel::Sunken, t.bg_deep);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.text));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), px(7)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::SetCursorScreenPos(at);
    ImGui::SetNextItemWidth(w);
    ImGui::PushFont(font_(false), st().theme.text_size);
    const bool changed = ImGui::InputText("##field", buffer, size, flags);
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    anchor(id);
    if (buffer[0] == 0 && !hint.empty() && !ImGui::IsItemActive())
        text(dl, ImVec2(at.x + px(8), at.y + px(7)), hint, t.text_faint);
    if (ImGui::IsItemActive()) focus_ring(dl, at, ImVec2(at.x + w, at.y + h));
    return changed;
}

int tabs(std::string_view id, const std::vector<std::string>& labels, int current, const std::vector<std::string>& anchors) {
    const auto& t = st().theme;
    Id scope(id);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = snap(ImGui::GetCursorScreenPos());
    const float h = text_size("Ag").y + px(16);
    float x = start.x;
    int chosen = current;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const auto& label = labels[static_cast<std::size_t>(i)];
        const float w = text_size(label).x + px(28);
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        ImGui::PushID(i);
        bool hovered = false, held = false;
        if (item_(ImVec2(w, h), hovered, held)) chosen = i;
        ImGui::PopID();
        anchor(static_cast<std::size_t>(i) < anchors.size() ? anchors[static_cast<std::size_t>(i)] : std::format("{}:{}", id, i));
        const bool on = i == current;
        if (on) panel(dl, ImVec2(x, start.y), ImVec2(x + w, start.y + h + unit()), Bevel::Raised, t.panel);
        else dl->AddRectFilled(ImVec2(x, start.y + px(4)), ImVec2(x + w, start.y + h), hovered ? t.panel_lo : t.bg_deep);
        const ImVec2 ts = text_size(label);
        text(dl, ImVec2(x + (w - ts.x) / 2, start.y + (h - ts.y) / 2 + (on ? 0 : px(2))), label,
             on ? t.text : hovered ? t.text_dim : t.text_faint);
        x += w + px(4);
    }
    // The line the chosen tab is joined to.
    const float right = start.x + ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(ImVec2(start.x, start.y + h), ImVec2(std::max(right, x), start.y + h + unit() / 3.0f), t.panel_hi);
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + h + unit()));
    ImGui::Dummy(ImVec2(0, 0));
    return chosen;
}

int segmented(std::string_view id, const std::vector<std::string>& labels, int current, float width,
              const std::vector<std::string>& anchors) {
    const auto& t = st().theme;
    Id scope(id);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = snap(ImGui::GetCursorScreenPos());
    const float w = px(width), h = text_size("Ag").y + px(14);
    panel(dl, start, ImVec2(start.x + w, start.y + h), Bevel::Sunken, t.bg_deep);
    const float each = labels.empty() ? w : std::floor(w / static_cast<float>(labels.size()));
    int chosen = current;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const ImVec2 a(start.x + each * i, start.y);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        bool hovered = false, held = false;
        if (item_(ImVec2(each, h), hovered, held)) chosen = i;
        ImGui::PopID();
        anchor(static_cast<std::size_t>(i) < anchors.size() ? anchors[static_cast<std::size_t>(i)] : std::format("{}:{}", id, i));
        const bool on = i == current;
        if (on) panel(dl, ImVec2(a.x + px(2), a.y + px(2)), ImVec2(a.x + each - px(2), a.y + h - px(2)), Bevel::Raised, t.accent_lo);
        const auto shown = fit(labels[static_cast<std::size_t>(i)], each - px(10));
        const ImVec2 ts = text_size(shown);
        text(dl, ImVec2(a.x + (each - ts.x) / 2, a.y + (h - ts.y) / 2), shown, on ? t.text : hovered ? t.text_dim : t.text_faint);
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + h));
    return chosen;
}

void progress(ImDrawList* dl, ImVec2 a, ImVec2 b, double fraction) {
    const auto& t = st().theme;
    a = snap(a);
    b = snap(b);
    panel(dl, a, b, Bevel::Sunken, t.bg_deep);
    const float pad = std::max(2.0f, unit() / 1.5f);
    const float block = unit() * 2, gap = unit() / 1.5f;
    const float inner = b.x - a.x - pad * 2;
    const int count = std::max(1, static_cast<int>((inner + gap) / (block + gap)));
    const ImVec2 o(a.x + pad, a.y + pad);
    const float bh = b.y - a.y - pad * 2;
    if (fraction >= 0) {
        const int lit = static_cast<int>(std::round(std::clamp(fraction, 0.0, 1.0) * count));
        for (int i = 0; i < lit; ++i)
            dl->AddRectFilled(ImVec2(o.x + i * (block + gap), o.y), ImVec2(o.x + i * (block + gap) + block, o.y + bh),
                              i == lit - 1 ? t.accent_hi : t.accent);
    } else {
        // Busy: four blocks running along.
        st().animating = !st().reduce_motion;
        const int head = st().reduce_motion ? count / 2 : static_cast<int>(st().time * 14.0) % (count + 4);
        for (int i = head - 4; i < head; ++i)
            if (i >= 0 && i < count)
                dl->AddRectFilled(ImVec2(o.x + i * (block + gap), o.y), ImVec2(o.x + i * (block + gap) + block, o.y + bh), t.accent);
    }
}

// ── anchors ───────────────────────────────────────────────────────────

void on_anchor(std::function<void(std::string_view, ImVec2, ImVec2)> sink) { st().sink = std::move(sink); }
void anchor(std::string_view name) {
    if (!st().sink) return;
    // Only what can be seen whole: a control scrolled partly out of its list
    // is not on the screen to click.
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 cmin = dl->GetClipRectMin(), cmax = dl->GetClipRectMax();
    if (mn.x < cmin.x - 0.5f || mn.y < cmin.y - 0.5f || mx.x > cmax.x + 0.5f || mx.y > cmax.y + 0.5f) return;
    if (mx.y - mn.y < 1 || mx.x - mn.x < 1) return;
    st().sink(name, mn, mx);
}

}  // namespace imkit
